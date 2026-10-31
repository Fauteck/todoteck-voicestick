# Die Board-Schicht: ein Header fuer beide Geraete, eine Quelldatei nur fuer
# die Uhr. Der StickS3 laeuft weiter ueber stick_s3_board -- voice_board.h
# reicht dessen Funktionen nur unter einem geraeteneutralen Namen durch, damit
# die Komponente, die seit dem Upstream existiert, unangetastet bleibt.
set(srcs "")
if(CONFIG_VOICE_BOARD_WATCH206)
    list(APPEND srcs "board_watch206.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES stick_s3_board driver esp_driver_i2c esp_driver_gpio esp_timer
)
