idf_component_register(
    SRCS "audio_tone.c"
    INCLUDE_DIRS "include"
    REQUIRES voice_board esp_driver_i2s esp_codec_dev heap
)
