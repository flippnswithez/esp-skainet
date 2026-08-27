cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS
    ../../../components
    )

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wake_word_detection)

# SAT001_GCC14_DSPI_CONV_WORKAROUND
# Xtensa GCC 14.2.0 ICEs on this ESP-DSP source at -Og.
# Limit the workaround to this one source file.
idf_component_get_property(esp_dsp_dir espressif__esp-dsp COMPONENT_DIR)
idf_component_get_property(esp_dsp_lib espressif__esp-dsp COMPONENT_LIB)

set_source_files_properties(
    "${esp_dsp_dir}/modules/conv/float/dspi_conv_f32_ansi.c"
    TARGET_DIRECTORY ${esp_dsp_lib}
    PROPERTIES COMPILE_OPTIONS "-O0"
)
