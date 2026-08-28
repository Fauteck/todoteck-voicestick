idf_component_register(
    SRCS "wrist_wake.c"
    INCLUDE_DIRS "include"
    REQUIRES stick_s3_board esp_driver_i2c
)
