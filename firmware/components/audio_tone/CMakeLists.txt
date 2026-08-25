idf_component_register(
    SRCS "audio_tone.c"
    INCLUDE_DIRS "include"
    REQUIRES stick_s3_board esp_driver_i2s esp_codec_dev
)
