/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/*
 * ============================================================================
 * N.E.R.V.E. SAT-001 -- room-satellite BENCH prototype firmware
 * ============================================================================
 *
 * Governance: N.E.R.V.E. task NV-SAT-001, readiness BLOCKED by NV-HW-001.
 * This firmware is BENCH VALIDATION ONLY. It exists to characterise endpoint
 * behaviour and produce facts that NV-HW-001 will later consume.
 *
 * *** THE TRANSPORT BELOW IS TEMPORARY BENCH SCAFFOLDING ***
 * Raw, unauthenticated, unencrypted TCP to a fixed IPv4 address and port.
 * It has no identity, no pairing, no token, and no confidentiality. It MUST
 * NOT evolve into the production protocol. The production transport is a
 * dedicated authenticated N.E.R.V.E. satellite transport carrying binary PCM,
 * with the satellite paired as a device through the EXISTING N.E.R.V.E.
 * pairing/token flow. Do not build that here.
 *
 * Wake word "Jarvis" is a TEMPORARY stock WakeNet model. The final phrase is
 * "Okay Nerve"; the Espressif model request is pending.
 *
 * The XMOS XU316 firmware is NOT touched by this project and must not be.
 *
 * ---------------------------------------------------------------------------
 * Endpoint facts established here (recorded for NV-HW-001)
 * ---------------------------------------------------------------------------
 * 1. bsp_get_input_format() == "MM": two MICROPHONE channels and NO playback
 *    reference channel. The ESP-side AFE therefore has NO acoustic echo
 *    canceller. Speaker output reaches the microphones unattenuated, so the
 *    endpoint MUST run half-duplex: wake detection and capture are hard-gated
 *    off while the speaker is active. Whether the XU316 performs its own AEC
 *    is UNVERIFIED and cannot be verified without inspecting XU316 firmware,
 *    which is out of scope.
 * 2. The ESP32-S3 has no mute control line to the XU316. Mute is an XU316-side
 *    hardware function. Firmware can only OBSERVE it, which it does by
 *    detecting sustained digitally-exact silence on the microphone stream.
 * 3. The RGB indicator is XU316-driven and is not addressable from the S3.
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"

#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_vadn_models.h"
#include "esp_board_init.h"
#include "model_path.h"
#include "esp_heap_caps.h"

/* ==========================================================================
 * Bench tuning constants
 * ========================================================================== */

#define SAT_SAMPLE_RATE            16000

/* Utterance shaping */
#define SAT_MAX_UTTERANCE_MS       8000   /* hard cap on a single utterance   */
#define SAT_UTTERANCE_HEADROOM_MS  1200   /* extra buffer for VAD lead-in     */
#define SAT_NO_SPEECH_TIMEOUT_MS   3000   /* wake but no speech -> abort      */
#define SAT_SILENCE_HANGOVER_MS     600   /* trailing silence ends utterance  */
#define SAT_MIN_SPEECH_MS           200   /* below this is not an utterance   */

/* Fallback speech detector. res->data_volume is AFE's own mean-square frame
 * energy in dBFS (pre-AGC). If VAD never triggers -- misconfiguration, a
 * missing model, an unexpectedly quiet room -- this keeps the endpoint
 * functional instead of aborting every utterance as no_speech. */
#define SAT_ENERGY_SPEECH_DBFS   (-45.0f)

/* AFE VAD shaping (see esp_afe_config.h) */
#define SAT_VAD_MIN_SPEECH_MS       128
#define SAT_VAD_MIN_NOISE_MS        320
#define SAT_VAD_DELAY_MS            256   /* size of AFE's own lead-in cache  */

/* Network deadlines -- every one of these exists so that a single bad Arvis
 * connection can NEVER leave SAT-001 permanently busy. */
#define SAT_CONNECT_TIMEOUT_MS     3000
#define SAT_SEND_TIMEOUT_MS        4000
#define SAT_RECV_FIRST_TIMEOUT_MS  6000   /* connect -> first response byte   */
#define SAT_RECV_IDLE_TIMEOUT_MS   2500   /* gap between response chunks      */
#define SAT_RESPONSE_TOTAL_MS     20000   /* absolute cap on the response     */

/* Half-duplex speaker gating */
#define SAT_PLAYBACK_DRAIN_MS       150   /* I2S DMA still draining after write */
#define SAT_SPEAK_TAIL_GUARD_MS     250   /* room reverb after the last sample  */

/* Mute observation */
#define SAT_MUTE_DETECT_MS          500   /* sustained exact silence => muted */
#define SAT_MUTE_RELEASE_MS         100

#define SAT_CAPTURE_CAPACITY_SAMPLES \
    (((SAT_MAX_UTTERANCE_MS + SAT_UTTERANCE_HEADROOM_MS) * SAT_SAMPLE_RATE) / 1000)

#define SAT_NET_CHUNK_BYTES        1024

/* ==========================================================================
 * Satellite state model
 *
 * Conceptual states recorded under NV-SAT-001. This is SAT-LOCAL bench
 * behaviour only -- it is NOT an implementation of a Brain-side protocol and
 * defines no wire format.
 * ========================================================================== */

typedef enum {
    SAT_STATE_BOOTING = 0,
    SAT_STATE_CONNECTING,
    SAT_STATE_IDLE,
    SAT_STATE_LISTENING,
    SAT_STATE_STREAMING,
    SAT_STATE_THINKING,
    SAT_STATE_SPEAKING,
    SAT_STATE_MUTED,
    SAT_STATE_ERROR,
} sat_state_t;

static const char *SAT_STATE_NAMES[] = {
    "BOOTING", "CONNECTING", "IDLE", "LISTENING",
    "STREAMING", "THINKING", "SPEAKING", "MUTED", "ERROR",
};

static portMUX_TYPE sat_state_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile sat_state_t sat_state = SAT_STATE_BOOTING;
static volatile int64_t sat_state_since_us = 0;

static sat_state_t sat_state_get(void)
{
    sat_state_t s;
    portENTER_CRITICAL(&sat_state_lock);
    s = sat_state;
    portEXIT_CRITICAL(&sat_state_lock);
    return s;
}

static void sat_state_set(sat_state_t next)
{
    sat_state_t prev;
    int64_t now = esp_timer_get_time();
    int64_t held_ms;

    portENTER_CRITICAL(&sat_state_lock);
    prev = sat_state;
    if (prev == next) {
        portEXIT_CRITICAL(&sat_state_lock);
        return;
    }
    held_ms = (now - sat_state_since_us) / 1000;
    sat_state = next;
    sat_state_since_us = now;
    portEXIT_CRITICAL(&sat_state_lock);

    printf("SAT-001 state %s -> %s (held %lldms)\n",
           SAT_STATE_NAMES[prev], SAT_STATE_NAMES[next], (long long)held_ms);
}

/* ==========================================================================
 * Shared runtime state
 * ========================================================================== */

static const esp_afe_sr_iface_t *afe_handle = NULL;
static volatile int task_flag = 0;

static int16_t *capture_buffer = NULL;
static volatile size_t capture_samples = 0;

/* Set by the network task, actioned by detect_Task so that every AFE API call
 * stays on a single task. */
static volatile bool sat_speaking = false;
static volatile bool sat_net_busy = false;

/* Observed XU316 hardware mute. Written by feed_Task, read everywhere. */
static volatile bool sat_muted = false;

/* Cycle metrics */
static volatile uint32_t sat_cycle_count = 0;
static volatile uint32_t sat_net_fail_count = 0;
static volatile uint32_t sat_abort_count = 0;
static volatile uint32_t sat_wifi_reconnects = 0;

static int64_t sat_t_wake_us = 0;
static int64_t sat_t_listen_us = 0;

/* ==========================================================================
 * Wi-Fi
 * ========================================================================== */

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

        printf("SAT-001 Wi-Fi disconnected, reason=%d\n", disc->reason);

        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

        /*
         * Never give up permanently. The first attempts are fast; after that
         * back off so a down AP does not spin the radio. A satellite that
         * cannot self-heal its own link is useless in a room.
         */
        wifi_retry_count++;
        sat_wifi_reconnects++;

        if (wifi_retry_count > 10) {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            vTaskDelay(pdMS_TO_TICKS(5000));
        }

        printf("SAT-001 Wi-Fi reconnect attempt %d\n", wifi_retry_count);
        esp_wifi_connect();

    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        printf("SAT-001 Wi-Fi connected: " IPSTR "\n", IP2STR(&event->ip_info.ip));

        wifi_retry_count = 0;
        xEventGroupClearBits(wifi_event_group, WIFI_FAIL_BIT);
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool sat_wifi_is_up(void)
{
    if (!wifi_event_group) {
        return false;
    }
    return (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
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
            WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};

    strlcpy((char *)wifi_config.sta.ssid,
            CONFIG_SAT_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));

    strlcpy((char *)wifi_config.sta.password,
            CONFIG_SAT_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));

    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    printf("SAT-001 connecting to Wi-Fi...\n");
    sat_state_set(SAT_STATE_CONNECTING);

    /*
     * Bounded wait. If the link is not up in time we still boot: wake-word
     * detection is LOCAL and must work regardless of the network, and the
     * event handler keeps retrying in the background.
     */
    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(20000));

    if (bits & WIFI_CONNECTED_BIT) {
        return true;
    }

    printf("SAT-001 Wi-Fi not up yet; continuing, retry runs in background\n");
    return false;
}

/* ==========================================================================
 * Bench TCP transport -- TEMPORARY SCAFFOLDING, see file header
 * ========================================================================== */

static void sat_sock_set_timeouts(int sock, int recv_ms, int send_ms)
{
    struct timeval rv = { .tv_sec = recv_ms / 1000,
                          .tv_usec = (recv_ms % 1000) * 1000 };
    struct timeval sv = { .tv_sec = send_ms / 1000,
                          .tv_usec = (send_ms % 1000) * 1000 };

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rv, sizeof(rv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &sv, sizeof(sv));
}

/*
 * Non-blocking connect bounded by SAT_CONNECT_TIMEOUT_MS.
 *
 * A blocking connect() to a host that is up but not listening, or silently
 * dropping SYNs, can stall for the platform TCP timeout (tens of seconds).
 * That is the single most likely way to strand the satellite, so it is
 * bounded explicitly rather than left to the stack default.
 */
static bool sat_tcp_connect(int sock, const struct sockaddr_in *dest, int timeout_ms)
{
    int flags = fcntl(sock, F_GETFL, 0);

    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        printf("SAT-001 could not set non-blocking mode\n");
        return false;
    }

    int rc = connect(sock, (const struct sockaddr *)dest, sizeof(*dest));

    if (rc != 0 && errno != EINPROGRESS) {
        printf("SAT-001 connect failed immediately: errno=%d\n", errno);
        return false;
    }

    if (rc != 0) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);

        struct timeval tv = { .tv_sec = timeout_ms / 1000,
                              .tv_usec = (timeout_ms % 1000) * 1000 };

        int sel = select(sock + 1, NULL, &wset, NULL, &tv);

        if (sel == 0) {
            printf("SAT-001 connect timed out after %dms\n", timeout_ms);
            return false;
        }

        if (sel < 0) {
            printf("SAT-001 connect select failed: errno=%d\n", errno);
            return false;
        }

        int so_error = 0;
        socklen_t len = sizeof(so_error);

        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 ||
            so_error != 0) {
            printf("SAT-001 connect refused: so_error=%d\n", so_error);
            return false;
        }
    }

    /* Back to blocking; deadlines are enforced by SO_RCVTIMEO/SO_SNDTIMEO. */
    fcntl(sock, F_SETFL, flags);
    return true;
}

/*
 * Round-trip one utterance.
 *
 * INVARIANT: this task has exactly ONE exit path. Whatever happens -- socket
 * failure, connect timeout, partial send, a server that accepts and then goes
 * silent forever, allocation failure -- control reaches `cleanup`, the busy
 * flag is cleared and the state machine returns to IDLE. One failed Arvis
 * connection can never leave SAT-001 permanently busy.
 */
static void send_capture_task(void *arg)
{
    (void)arg;

    const size_t total_bytes = capture_samples * sizeof(int16_t);

    int sock = -1;
    uint8_t *net_buffer = NULL;
    int16_t *pcm_buffer = NULL;

    size_t sent = 0;
    size_t response_bytes = 0;
    size_t played_samples = 0;
    bool speaking_started = false;

    int64_t t_start = esp_timer_get_time();
    int64_t t_connected = 0;
    int64_t t_sent = 0;
    int64_t t_first_byte = 0;
    int64_t t_playback_end = 0;

    const uint32_t fails_before = sat_net_fail_count;

    struct sockaddr_in dest = {0};

    if (total_bytes == 0) {
        printf("SAT-001 nothing to send\n");
        goto cleanup;
    }

    if (!sat_wifi_is_up()) {
        printf("SAT-001 uplink skipped: Wi-Fi down\n");
        sat_net_fail_count++;
        goto cleanup;
    }

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

    if (sock < 0) {
        printf("SAT-001 socket creation failed: errno=%d\n", errno);
        sat_net_fail_count++;
        goto cleanup;
    }

    dest.sin_family = AF_INET;
    dest.sin_port = htons(CONFIG_SAT_SERVER_PORT);

    if (inet_pton(AF_INET, CONFIG_SAT_SERVER_IP, &dest.sin_addr) != 1) {
        printf("SAT-001 invalid server IP: %s\n", CONFIG_SAT_SERVER_IP);
        sat_net_fail_count++;
        goto cleanup;
    }

    printf("SAT-001 connecting to Arvis %s:%d (bench TCP)\n",
           CONFIG_SAT_SERVER_IP, CONFIG_SAT_SERVER_PORT);

    if (!sat_tcp_connect(sock, &dest, SAT_CONNECT_TIMEOUT_MS)) {
        sat_net_fail_count++;
        goto cleanup;
    }

    t_connected = esp_timer_get_time();

    sat_sock_set_timeouts(sock, SAT_RECV_FIRST_TIMEOUT_MS, SAT_SEND_TIMEOUT_MS);

    /* ---- uplink ---- */
    {
        const uint8_t *bytes = (const uint8_t *)capture_buffer;

        while (sent < total_bytes) {
            int written = send(sock, bytes + sent, total_bytes - sent, 0);

            if (written <= 0) {
                printf("SAT-001 TCP send failed after %u bytes: errno=%d\n",
                       (unsigned)sent, errno);
                sat_net_fail_count++;
                goto cleanup;
            }

            sent += (size_t)written;
        }
    }

    t_sent = esp_timer_get_time();

    printf("SAT-001 sent %u audio bytes in %lldms\n",
           (unsigned)sent, (long long)((t_sent - t_connected) / 1000));

    /* ---- downlink ---- */
    sat_state_set(SAT_STATE_THINKING);

    net_buffer = malloc(SAT_NET_CHUNK_BYTES);
    pcm_buffer = malloc((SAT_NET_CHUNK_BYTES / 2) * sizeof(int16_t));

    if (!net_buffer || !pcm_buffer) {
        printf("SAT-001 response buffer allocation failed\n");
        goto cleanup;
    }

    {
        bool have_low_byte = false;
        uint8_t low_byte = 0;
        int64_t deadline = t_sent + ((int64_t)SAT_RESPONSE_TOTAL_MS * 1000);

        while (1) {
            if (esp_timer_get_time() > deadline) {
                printf("SAT-001 response exceeded %dms budget; abandoning\n",
                       SAT_RESPONSE_TOTAL_MS);
                sat_net_fail_count++;
                break;
            }

            int received = recv(sock, net_buffer, SAT_NET_CHUNK_BYTES, 0);

            if (received == 0) {
                /* Orderly close: Arvis finished. */
                break;
            }

            if (received < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                    printf("SAT-001 response stalled (no data for %dms); abandoning\n",
                           response_bytes == 0 ? SAT_RECV_FIRST_TIMEOUT_MS
                                               : SAT_RECV_IDLE_TIMEOUT_MS);
                } else {
                    printf("SAT-001 response receive failed: errno=%d\n", errno);
                }
                sat_net_fail_count++;
                break;
            }

            if (response_bytes == 0) {
                t_first_byte = esp_timer_get_time();

                /*
                 * Enter SPEAKING BEFORE the first sample reaches the speaker.
                 * detect_Task observes this and hard-gates WakeNet, so our own
                 * output can neither self-trigger the wake word nor be captured
                 * as a new utterance. There is no AEC on this endpoint, so this
                 * gate is the only thing preventing it.
                 */
                sat_speaking = true;
                speaking_started = true;
                sat_state_set(SAT_STATE_SPEAKING);

                /* Tighten the timeout now that the stream has started. */
                sat_sock_set_timeouts(sock, SAT_RECV_IDLE_TIMEOUT_MS,
                                      SAT_SEND_TIMEOUT_MS);
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
                        (int16_t)((uint16_t)low_byte | ((uint16_t)byte << 8));
                    have_low_byte = false;
                }
            }

            if (samples > 0) {
                esp_err_t play_ret = esp_audio_play(
                    pcm_buffer, samples * sizeof(int16_t), pdMS_TO_TICKS(1000));

                if (play_ret != ESP_OK) {
                    printf("SAT-001 response playback failed: %s\n",
                           esp_err_to_name(play_ret));
                    break;
                }

                played_samples += samples;
            }
        }
    }

    t_playback_end = esp_timer_get_time();

    printf("SAT-001 received %u response bytes, played %u samples\n",
           (unsigned)response_bytes, (unsigned)played_samples);

cleanup:
    if (sock >= 0) {
        shutdown(sock, SHUT_RDWR);
        close(sock);
    }

    free(net_buffer);
    free(pcm_buffer);

    /*
     * Let the I2S DMA drain, then hold the gate open a little longer for room
     * reverb, before handing the microphones back to WakeNet. esp_audio_play()
     * returns once bytes are accepted by DMA, NOT when they have been heard.
     */
    if (speaking_started) {
        vTaskDelay(pdMS_TO_TICKS(SAT_PLAYBACK_DRAIN_MS + SAT_SPEAK_TAIL_GUARD_MS));
    }

    sat_speaking = false;

    /* ---- cycle metrics (NV-HW-001 endpoint facts) ---- */
    {
        int64_t now = esp_timer_get_time();

        long long connect_ms  = t_connected ? (t_connected - t_start) / 1000 : -1;
        long long uplink_ms   = t_sent ? (t_sent - t_connected) / 1000 : -1;
        long long respond_ms  = t_first_byte ? (t_first_byte - t_sent) / 1000 : -1;
        long long playback_ms = (t_first_byte && t_playback_end)
                                    ? (t_playback_end - t_first_byte) / 1000 : -1;
        long long audio_ms    = (long long)played_samples * 1000 / SAT_SAMPLE_RATE;
        long long total_ms    = (now - t_start) / 1000;

        printf("SAT-001 METRICS net cycle=%lu connect_ms=%lld uplink_ms=%lld "
               "uplink_bytes=%u response_ms=%lld response_bytes=%u "
               "playback_ms=%lld playback_audio_ms=%lld total_ms=%lld "
               "net_fails=%lu\n",
               (unsigned long)sat_cycle_count, connect_ms, uplink_ms,
               (unsigned)sent, respond_ms, (unsigned)response_bytes,
               playback_ms, audio_ms, total_ms,
               (unsigned long)sat_net_fail_count);

        printf("SAT-001 METRICS heap internal_free=%u internal_min=%u "
               "spiram_free=%u largest_internal=%u\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }

    capture_samples = 0;

    /*
     * This task entered STREAMING/THINKING/SPEAKING, so it is responsible for
     * leaving the machine in a sane state. Without this the satellite reported
     * THINKING forever after any failed round trip.
     */
    if (sat_net_fail_count != fails_before) {
        sat_state_set(SAT_STATE_ERROR);
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    sat_state_set(sat_wifi_is_up() ? SAT_STATE_IDLE : SAT_STATE_CONNECTING);

    sat_net_busy = false;

    vTaskDelete(NULL);
}

/* ==========================================================================
 * Wake acknowledgement chime
 * ========================================================================== */

static void play_wake_chime(void)
{
    static const int16_t sine_lut[16] = {
         0,  1913,  3535,  4619,
      5000,  4619,  3535,  1913,
         0, -1913, -3535, -4619,
     -5000, -4619, -3535, -1913
    };

    const int sample_rate = SAT_SAMPLE_RATE;
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

    esp_err_t ret = esp_audio_play(tone, sample_count * sizeof(int16_t),
                                   pdMS_TO_TICKS(500));

    if (ret != ESP_OK) {
        printf("wake chime playback failed: %s\n", esp_err_to_name(ret));
    }

    free(tone);

    /* Let the chime clear the DMA and the room before we start listening. */
    vTaskDelay(pdMS_TO_TICKS(SAT_PLAYBACK_DRAIN_MS));
}

/* ==========================================================================
 * feed_Task -- I2S -> AFE, plus hardware-mute observation
 * ========================================================================== */

void feed_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int nch = afe_handle->get_feed_channel_num(afe_data);
    int feed_channel = esp_get_feed_channel();
    assert(nch == feed_channel);

    int16_t *i2s_buff = malloc(audio_chunksize * sizeof(int16_t) * feed_channel);
    assert(i2s_buff);

    const int frame_ms = (audio_chunksize * 1000) / SAT_SAMPLE_RATE;
    int silent_ms = 0;
    int active_ms = 0;

    while (task_flag) {
        esp_get_feed_data(true, i2s_buff,
                          audio_chunksize * sizeof(int16_t) * feed_channel);

        /*
         * Hardware mute observation.
         *
         * The S3 has no mute line to the XU316, so mute can only be observed.
         * A live microphone always carries a noise floor, so a frame of
         * digitally EXACT zeros means the XU316 has muted the stream -- a
         * quiet room does not look like this.
         */
        bool frame_silent = true;

        for (int i = 0; i < audio_chunksize * feed_channel; i++) {
            if (i2s_buff[i] != 0) {
                frame_silent = false;
                break;
            }
        }

        if (frame_silent) {
            active_ms = 0;
            if (silent_ms < SAT_MUTE_DETECT_MS) {
                silent_ms += frame_ms;
                if (silent_ms >= SAT_MUTE_DETECT_MS && !sat_muted) {
                    sat_muted = true;
                    printf("SAT-001 hardware mute ENGAGED (observed)\n");
                }
            }
        } else {
            silent_ms = 0;
            if (active_ms < SAT_MUTE_RELEASE_MS) {
                active_ms += frame_ms;
                if (active_ms >= SAT_MUTE_RELEASE_MS && sat_muted) {
                    sat_muted = false;
                    printf("SAT-001 hardware mute RELEASED (observed)\n");
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

/* ==========================================================================
 * detect_Task -- wake, VAD-driven capture, half-duplex gating
 *
 * ALL AFE API calls live on this task. The network task signals intent with
 * flags rather than calling into AFE from a second thread.
 * ========================================================================== */

static void sat_append_samples(const int16_t *src, size_t count)
{
    if (!src || count == 0) {
        return;
    }

    size_t space = SAT_CAPTURE_CAPACITY_SAMPLES - capture_samples;

    if (count > space) {
        count = space;
    }

    if (count > 0) {
        memcpy(&capture_buffer[capture_samples], src, count * sizeof(int16_t));
        capture_samples += count;
    }
}

static void sat_dispatch_capture(void)
{
    sat_net_busy = true;
    sat_state_set(SAT_STATE_STREAMING);

    if (xTaskCreate(send_capture_task, "sat_send", 6 * 1024, NULL, 4, NULL)
            != pdPASS) {
        printf("SAT-001 failed to create send task\n");
        sat_net_busy = false;
        capture_samples = 0;
        sat_state_set(SAT_STATE_IDLE);
    }
}

void detect_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    int afe_chunksize = afe_handle->get_fetch_chunksize(afe_data);
    const int frame_ms = (afe_chunksize * 1000) / SAT_SAMPLE_RATE;

    bool listening = false;
    bool wakenet_gated = false;
    bool had_speech = false;
    int listen_ms = 0;
    int speech_ms = 0;
    int silence_ms = 0;
    int vad_speech_ms = 0;
    int energy_speech_ms = 0;
    float peak_dbfs = -120.0f;

    printf("------------detect start------------\n");
    printf("SAT-001 fetch chunk=%d samples (%dms/frame)\n",
           afe_chunksize, frame_ms);

    sat_state_set(sat_wifi_is_up() ? SAT_STATE_IDLE : SAT_STATE_CONNECTING);

    while (task_flag) {
        afe_fetch_result_t *res = afe_handle->fetch(afe_data);

        if (!res || res->ret_value == ESP_FAIL) {
            printf("SAT-001 fetch error\n");
            sat_state_set(SAT_STATE_ERROR);
            vTaskDelay(pdMS_TO_TICKS(100));
            sat_state_set(SAT_STATE_IDLE);
            continue;
        }

        /* ---------------- half-duplex speaker gate ---------------- */
        if (sat_speaking && !wakenet_gated) {
            afe_handle->disable_wakenet(afe_data);
            wakenet_gated = true;
            printf("SAT-001 wake detection GATED (speaker active, no AEC)\n");

            /* Anything in flight is ours or was interrupted; drop it. */
            listening = false;
            capture_samples = 0;
        }

        if (!sat_speaking && wakenet_gated) {
            /*
             * Discard the AFE frames captured while our own speaker was
             * running before re-arming, otherwise the tail of the response
             * is still sitting in the pipeline waiting to be misheard.
             */
            afe_handle->reset_buffer(afe_data);
            afe_handle->enable_wakenet(afe_data);
            wakenet_gated = false;
            printf("SAT-001 wake detection RE-ARMED\n");

            if (!sat_net_busy) {
                sat_state_set(sat_wifi_is_up() ? SAT_STATE_IDLE
                                               : SAT_STATE_CONNECTING);
            }
            continue;
        }

        if (wakenet_gated) {
            continue;
        }

        /* ---------------- mute ---------------- */
        if (sat_muted) {
            if (listening) {
                printf("SAT-001 capture abandoned: muted mid-utterance\n");
                listening = false;
                capture_samples = 0;
                sat_abort_count++;
            }
            sat_state_set(SAT_STATE_MUTED);
            continue;
        }

        if (sat_state_get() == SAT_STATE_MUTED) {
            sat_state_set(sat_wifi_is_up() ? SAT_STATE_IDLE : SAT_STATE_CONNECTING);
        }

        /* ---------------- idle link tracking ---------------- */
        if (!listening && !sat_net_busy) {
            sat_state_t now = sat_state_get();
            if (sat_wifi_is_up() && now == SAT_STATE_CONNECTING) {
                sat_state_set(SAT_STATE_IDLE);
            } else if (!sat_wifi_is_up() && now == SAT_STATE_IDLE) {
                sat_state_set(SAT_STATE_CONNECTING);
            }
        }

        /* ---------------- wake ---------------- */
        if (res->wakeup_state == WAKENET_DETECTED) {
            if (listening || sat_net_busy) {
                printf("SAT-001 wake ignored: busy (%s)\n",
                       SAT_STATE_NAMES[sat_state_get()]);
            } else {
                sat_t_wake_us = esp_timer_get_time();

                printf("wakeword detected\n");
                printf("model index:%d, word index:%d\n",
                       res->wakenet_model_index, res->wake_word_index);

                play_wake_chime();

                capture_samples = 0;
                listening = true;
                had_speech = false;
                listen_ms = 0;
                speech_ms = 0;
                silence_ms = 0;
                vad_speech_ms = 0;
                energy_speech_ms = 0;
                peak_dbfs = -120.0f;

                sat_t_listen_us = esp_timer_get_time();
                sat_state_set(SAT_STATE_LISTENING);

                printf("-----------LISTENING-----------\n");
                printf("SAT-001 METRICS wake_to_listen_ms=%lld\n",
                       (long long)((sat_t_listen_us - sat_t_wake_us) / 1000));

                /*
                 * Drop AFE frames buffered while the chime played -- they
                 * contain the chime, not the user.
                 */
                afe_handle->reset_buffer(afe_data);
                continue;
            }
        }

        if (!listening) {
            continue;
        }

        /* ---------------- VAD-driven capture ---------------- */

        /*
         * vad_cache is AFE's own lead-in cache: the audio that preceded the
         * VAD trigger. Prepending it is what stops the first word being cut,
         * and it comes from the library rather than a hand-rolled ring buffer.
         */
        if (res->vad_cache_size > 0 && res->vad_cache) {
            sat_append_samples(res->vad_cache,
                               (size_t)res->vad_cache_size / sizeof(int16_t));
        }

        if (res->data && res->data_size > 0) {
            sat_append_samples(res->data,
                               (size_t)res->data_size / sizeof(int16_t));
        }

        listen_ms += frame_ms;

        bool vad_says_speech = (res->vad_state == VAD_SPEECH);
        bool energy_says_speech = (res->data_volume > SAT_ENERGY_SPEECH_DBFS);

        if (res->data_volume > peak_dbfs) {
            peak_dbfs = res->data_volume;
        }

        if (vad_says_speech) {
            vad_speech_ms += frame_ms;
        }
        if (energy_says_speech) {
            energy_speech_ms += frame_ms;
        }

        if (vad_says_speech || energy_says_speech) {
            had_speech = true;
            speech_ms += frame_ms;
            silence_ms = 0;
        } else if (had_speech) {
            silence_ms += frame_ms;
        }

        /* ---- end-of-utterance decisions ---- */

        bool complete = false;
        bool abort_utterance = false;
        const char *reason = "";

        if (had_speech && speech_ms >= SAT_MIN_SPEECH_MS &&
            silence_ms >= SAT_SILENCE_HANGOVER_MS) {
            complete = true;
            reason = "silence";

        } else if (listen_ms >= SAT_MAX_UTTERANCE_MS) {
            if (had_speech && speech_ms >= SAT_MIN_SPEECH_MS) {
                complete = true;
                reason = "max_utterance";
            } else {
                abort_utterance = true;
                reason = "max_utterance_no_speech";
            }

        } else if (capture_samples >= SAT_CAPTURE_CAPACITY_SAMPLES) {
            complete = true;
            reason = "buffer_full";

        } else if (!had_speech && listen_ms >= SAT_NO_SPEECH_TIMEOUT_MS) {
            abort_utterance = true;
            reason = "no_speech";
        }

        if (!complete && !abort_utterance) {
            continue;
        }

        listening = false;
        sat_cycle_count++;

        printf("SAT-001 METRICS utterance cycle=%lu reason=%s listen_ms=%d "
               "speech_ms=%d vad_speech_ms=%d energy_speech_ms=%d "
               "peak_dbfs=%.1f trailing_silence_ms=%d captured_samples=%u "
               "captured_ms=%u aborts=%lu\n",
               (unsigned long)sat_cycle_count, reason, listen_ms, speech_ms,
               vad_speech_ms, energy_speech_ms, (double)peak_dbfs,
               silence_ms, (unsigned)capture_samples,
               (unsigned)(capture_samples * 1000 / SAT_SAMPLE_RATE),
               (unsigned long)sat_abort_count);

        if (abort_utterance) {
            printf("SAT-001 utterance abandoned (%s); nothing sent\n", reason);
            capture_samples = 0;
            sat_abort_count++;
            sat_state_set(sat_wifi_is_up() ? SAT_STATE_IDLE
                                           : SAT_STATE_CONNECTING);
            continue;
        }

        sat_dispatch_capture();
    }

    vTaskDelete(NULL);
}

/* ==========================================================================
 * app_main
 * ========================================================================== */

void app_main(void)
{
    sat_state_since_us = esp_timer_get_time();

    ESP_ERROR_CHECK(esp_board_init(SAT_SAMPLE_RATE, 1, 16));

    capture_buffer = heap_caps_malloc(
        SAT_CAPTURE_CAPACITY_SAMPLES * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!capture_buffer) {
        capture_buffer = malloc(SAT_CAPTURE_CAPACITY_SAMPLES * sizeof(int16_t));
    }

    assert(capture_buffer);

    printf("SAT-001 capture buffer ready: %u bytes (%d ms max utterance)\n",
           (unsigned)(SAT_CAPTURE_CAPACITY_SAMPLES * sizeof(int16_t)),
           SAT_MAX_UTTERANCE_MS);

    sat_wifi_init();

    srmodel_list_t *models = esp_srmodel_init("model");

    if (models) {
        for (int i = 0; i < models->num; i++) {
            printf("model in flash: %s\n", models->model_name[i]);
        }
    }

    afe_config_t *afe_config = afe_config_init(
        esp_get_input_format(), models, AFE_TYPE_SR, AFE_MODE_LOW_COST);

    /*
     * VAD is what turns a fixed 2-second grab into a real utterance. VADNet is
     * selected in sdkconfig (CONFIG_SR_VADN_VADNET1_MEDIUM); bind it explicitly
     * so behaviour does not depend on afe_config_init defaults.
     */
    afe_config->vad_init = true;
    afe_config->vad_mode = VAD_MODE_2;
    afe_config->vad_min_speech_ms = SAT_VAD_MIN_SPEECH_MS;
    afe_config->vad_min_noise_ms = SAT_VAD_MIN_NOISE_MS;
    afe_config->vad_delay_ms = SAT_VAD_DELAY_MS;

    if (models) {
        char *vad_model = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
        if (vad_model) {
            afe_config->vad_model_name = vad_model;
            printf("SAT-001 VAD model: %s\n", vad_model);
        } else {
            printf("SAT-001 no VADNet model in flash; using WebRTC VAD\n");
        }
    }

    if (afe_config->wakenet_model_name) {
        printf("wakeword model in AFE config: %s\n", afe_config->wakenet_model_name);
    }
    if (afe_config->wakenet_model_name_2) {
        printf("wakeword model in AFE config: %s\n", afe_config->wakenet_model_name_2);
    }

    printf("SAT-001 input format: %s (no reference channel => NO AEC, "
           "half-duplex gating required)\n", esp_get_input_format());

    afe_config_check(afe_config);

    afe_handle = esp_afe_handle_from_config(afe_config);
    esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(afe_config);
    assert(afe_data);

    afe_config_free(afe_config);

    printf("SAT-001 METRICS boot internal_free=%u spiram_free=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    task_flag = 1;
    xTaskCreatePinnedToCore(&feed_Task, "feed", 8 * 1024, (void *)afe_data, 5, NULL, 0);
    xTaskCreatePinnedToCore(&detect_Task, "detect", 8 * 1024, (void *)afe_data, 5, NULL, 1);
}
