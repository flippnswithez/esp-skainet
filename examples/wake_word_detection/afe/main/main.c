/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_board_init.h"
#include "model_path.h"
#include "string.h"
#include "esp_heap_caps.h"

int detect_flag = 0;
static const esp_afe_sr_iface_t *afe_handle = NULL;
static volatile int task_flag = 0;

#define SAT_CAPTURE_SAMPLE_RATE 16000
#define SAT_CAPTURE_MS 2000
#define SAT_CAPTURE_SAMPLES     ((SAT_CAPTURE_SAMPLE_RATE * SAT_CAPTURE_MS) / 1000)

static int16_t *capture_buffer = NULL;
static volatile size_t capture_samples = 0;
static volatile bool capture_active = false;
static volatile int32_t capture_peak = 0;
static volatile bool capture_send_active = false;

static EventGroupHandle_t wifi_event_group;
static int wifi_retry_count = 0;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {

        wifi_event_sta_disconnected_t *disc =
            (wifi_event_sta_disconnected_t *)event_data;

        printf("SAT-001 Wi-Fi disconnected, reason=%d\n",
               disc->reason);

        if (wifi_retry_count < 10) {
            wifi_retry_count++;
            esp_wifi_connect();
            printf("SAT-001 Wi-Fi reconnect attempt %d\n",
                   wifi_retry_count);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }

    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        printf("SAT-001 Wi-Fi connected: " IPSTR "\n",
               IP2STR(&event->ip_info.ip));

        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool sat_wifi_init(void)
{
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_event_group = xEventGroupCreate();
    assert(wifi_event_group);

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL
        )
    );

    wifi_config_t wifi_config = {0};

    strlcpy((char *)wifi_config.sta.ssid,
            CONFIG_SAT_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));

    strlcpy((char *)wifi_config.sta.password,
            CONFIG_SAT_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));

    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config)
    );
    ESP_ERROR_CHECK(esp_wifi_start());

    printf("SAT-001 connecting to Wi-Fi...\n");

    EventBits_t bits =
        xEventGroupWaitBits(
            wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY
        );

    if (bits & WIFI_CONNECTED_BIT) {
        return true;
    }

    printf("SAT-001 Wi-Fi connection failed\n");
    return false;
}

static void send_capture_task(void *arg)
{
    (void)arg;

    const size_t total_bytes =
        SAT_CAPTURE_SAMPLES * sizeof(int16_t);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

    if (sock < 0) {
        printf("SAT-001 socket creation failed\n");
        capture_send_active = false;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(CONFIG_SAT_SERVER_PORT);

    if (inet_pton(AF_INET,
                  CONFIG_SAT_SERVER_IP,
                  &dest.sin_addr) != 1) {

        printf("SAT-001 invalid server IP: %s\n",
               CONFIG_SAT_SERVER_IP);

        close(sock);
        capture_send_active = false;
        vTaskDelete(NULL);
        return;
    }

    printf("SAT-001 connecting to Arvis %s:%d\n",
           CONFIG_SAT_SERVER_IP,
           CONFIG_SAT_SERVER_PORT);

    if (connect(sock,
                (struct sockaddr *)&dest,
                sizeof(dest)) != 0) {

        printf("SAT-001 TCP connection failed\n");

        close(sock);
        capture_send_active = false;
        vTaskDelete(NULL);
        return;
    }

    size_t sent = 0;
    const uint8_t *bytes =
        (const uint8_t *)capture_buffer;

    while (sent < total_bytes) {

        int written =
            send(sock,
                 bytes + sent,
                 total_bytes - sent,
                 0);

        if (written <= 0) {
            printf("SAT-001 TCP send failed after %u bytes\n",
                   (unsigned)sent);
            break;
        }

        sent += (size_t)written;
    }

    printf("SAT-001 sent %u of %u audio bytes\n",
           (unsigned)sent,
           (unsigned)total_bytes);

    if (sent == total_bytes) {
        printf("SAT-001 waiting for Arvis response audio...\n");

        uint8_t *net_buffer = malloc(1024);
        int16_t *pcm_buffer = malloc(512 * sizeof(int16_t));

        if (!net_buffer || !pcm_buffer) {
            printf("SAT-001 response buffer allocation failed\n");
        } else {
            size_t response_bytes = 0;
            bool have_low_byte = false;
            uint8_t low_byte = 0;

            while (1) {
                int received =
                    recv(sock, net_buffer, 1024, 0);

                if (received == 0) {
                    break;
                }

                if (received < 0) {
                    printf("SAT-001 response receive failed\n");
                    break;
                }

                response_bytes += (size_t)received;

                size_t samples = 0;

                for (int i = 0; i < received; i++) {
                    uint8_t byte = net_buffer[i];

                    if (!have_low_byte) {
                        low_byte = byte;
                        have_low_byte = true;
                    } else {
                        pcm_buffer[samples++] =
                            (int16_t)(
                                (uint16_t)low_byte |
                                ((uint16_t)byte << 8)
                            );

                        have_low_byte = false;
                    }
                }

                if (samples > 0) {
                    esp_err_t play_ret =
                        esp_audio_play(
                            pcm_buffer,
                            samples * sizeof(int16_t),
                            pdMS_TO_TICKS(1000)
                        );

                    if (play_ret != ESP_OK) {
                        printf(
                            "SAT-001 response playback failed: %s\n",
                            esp_err_to_name(play_ret)
                        );
                        break;
                    }
                }
            }

            printf("SAT-001 received %u response audio bytes\n",
                   (unsigned)response_bytes);

            if (response_bytes > 0) {
                printf("SAT-001 response playback complete\n");
            }
        }

        free(net_buffer);
        free(pcm_buffer);
    }

    shutdown(sock, SHUT_RDWR);
    close(sock);

    capture_send_active = false;
    vTaskDelete(NULL);
}

static void start_mic_capture(void)
{
    if (capture_active || capture_send_active) {
        printf("SAT-001 capture busy\n");
        return;
    }

    capture_samples = 0;
    capture_peak = 0;
    capture_active = true;

    printf("SAT-001 microphone capture started: %d ms\n",
           SAT_CAPTURE_MS);
}

static void play_wake_chime(void)
{
    static const int16_t sine_lut[16] = {
         0,  1913,  3535,  4619,
      5000,  4619,  3535,  1913,
         0, -1913, -3535, -4619,
     -5000, -4619, -3535, -1913
    };

    const int sample_rate = 16000;
    const int duration_ms = 140;
    const int sample_count = sample_rate * duration_ms / 1000;

    int16_t *tone = malloc(sample_count * sizeof(int16_t));
    if (!tone) {
        printf("wake chime allocation failed\n");
        return;
    }

    uint32_t phase = 0;

    for (int i = 0; i < sample_count; i++) {
        int frequency = (i < sample_count / 2) ? 880 : 1175;

        uint32_t phase_inc =
            (uint32_t)(((uint64_t)frequency << 32) / sample_rate);

        phase += phase_inc;

        int32_t sample = sine_lut[phase >> 28];

        /* Short fade-in/out to avoid clicks. */
        int edge = sample_count / 12;

        if (i < edge) {
            sample = sample * i / edge;
        } else if (i >= sample_count - edge) {
            sample = sample * (sample_count - 1 - i) / edge;
        }

        tone[i] = (int16_t)sample;
    }

    esp_err_t ret =
        esp_audio_play(tone, sample_count * sizeof(int16_t),
                       pdMS_TO_TICKS(500));

    if (ret != ESP_OK) {
        printf("wake chime playback failed: %s\n",
               esp_err_to_name(ret));
    }

    free(tone);
}

void feed_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int nch = afe_handle->get_feed_channel_num(afe_data);
    int feed_channel = esp_get_feed_channel();
    assert(nch==feed_channel);
    int16_t *i2s_buff = malloc(audio_chunksize * sizeof(int16_t) * feed_channel);
    assert(i2s_buff);

    while (task_flag) {
        esp_get_feed_data(true, i2s_buff, audio_chunksize * sizeof(int16_t) * feed_channel);

        if (capture_active && capture_buffer) {
            for (int i = 0;
                 i < audio_chunksize && capture_samples < SAT_CAPTURE_SAMPLES;
                 i++) {

                /*
                 * SAT-001 receives two interleaved microphone channels.
                 * Store channel 0 as 16 kHz mono PCM for this bench capture.
                 */
                int16_t sample = i2s_buff[i * feed_channel];
                capture_buffer[capture_samples++] = sample;

                int32_t level = sample;
                if (level < 0) {
                    level = -level;
                }

                if (level > capture_peak) {
                    capture_peak = level;
                }
            }

            if (capture_samples >= SAT_CAPTURE_SAMPLES) {
                capture_active = false;

                printf("SAT-001 microphone capture complete: %u samples, peak=%ld\n",
                       (unsigned)capture_samples,
                       (long)capture_peak);

                capture_send_active = true;

                if (xTaskCreate(
                        send_capture_task,
                        "sat_send",
                        6 * 1024,
                        NULL,
                        4,
                        NULL
                    ) != pdPASS) {

                    printf("SAT-001 failed to create send task\n");
                    capture_send_active = false;
                }
            }
        }

        afe_handle->feed(afe_data, i2s_buff);
    }
    if (i2s_buff) {
        free(i2s_buff);
        i2s_buff = NULL;
    }
    vTaskDelete(NULL);
}

void detect_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    int afe_chunksize = afe_handle->get_fetch_chunksize(afe_data);
    int16_t *buff = malloc(afe_chunksize * sizeof(int16_t));
    assert(buff);
    printf("------------detect start------------\n");

    // modify wakenet detection threshold
    afe_handle->set_wakenet_threshold(afe_data, 1, 0.6); // set model1's threshold to 0.6
    afe_handle->set_wakenet_threshold(afe_data, 2, 0.6); // set model2's threshold to 0.6
    afe_handle->reset_wakenet_threshold(afe_data, 1);    // reset model1's threshold to default
    afe_handle->reset_wakenet_threshold(afe_data, 2);    // reset model2's threshold to default

    while (task_flag) {
        afe_fetch_result_t* res = afe_handle->fetch(afe_data); 
        if (!res || res->ret_value == ESP_FAIL) {
            printf("fetch error!\n");
            break;
        }
        // printf("vad state: %d\n", res->vad_state);

        if (res->wakeup_state == WAKENET_DETECTED) {
            printf("wakeword detected\n");
	        printf("model index:%d, word index:%d\n", res->wakenet_model_index, res->wake_word_index);
            play_wake_chime();
            start_mic_capture();
            printf("-----------LISTENING-----------\n");
        }
    }
    if (buff) {
        free(buff);
        buff = NULL;
    }
    vTaskDelete(NULL);
}

void app_main()
{
    ESP_ERROR_CHECK(esp_board_init(16000, 1, 16));
    // ESP_ERROR_CHECK(esp_sdcard_init("/sdcard", 10));

    capture_buffer = heap_caps_malloc(
        SAT_CAPTURE_SAMPLES * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (!capture_buffer) {
        capture_buffer = malloc(
            SAT_CAPTURE_SAMPLES * sizeof(int16_t)
        );
    }

    assert(capture_buffer);

    printf("SAT-001 capture buffer ready: %u bytes\n",
           (unsigned)(SAT_CAPTURE_SAMPLES * sizeof(int16_t)));

    if (!sat_wifi_init()) {
        printf("SAT-001 continuing without network send capability\n");
    }

    srmodel_list_t *models = esp_srmodel_init("model");
    if (models) {
        for (int i=0; i<models->num; i++) {
            if (strstr(models->model_name[i], ESP_WN_PREFIX) != NULL) {
                printf("wakenet model in flash: %s\n", models->model_name[i]);
            }
        }
    }

    afe_config_t *afe_config = afe_config_init(esp_get_input_format(), models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    
    // print/modify wake word model. 
    if (afe_config->wakenet_model_name) {
        printf("wakeword model in AFE config: %s\n", afe_config->wakenet_model_name);
    }
    if (afe_config->wakenet_model_name_2) {
        printf("wakeword model in AFE config: %s\n", afe_config->wakenet_model_name_2);
    }

    afe_handle = esp_afe_handle_from_config(afe_config);
    esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(afe_config);
    

    // 
    afe_config_free(afe_config);
    
    task_flag = 1;
    xTaskCreatePinnedToCore(&feed_Task, "feed", 8 * 1024, (void*)afe_data, 5, NULL, 0);
    xTaskCreatePinnedToCore(&detect_Task, "detect", 4 * 1024, (void*)afe_data, 5, NULL, 1);
}
