cmake_minimum_required(VERSION 3.16)

# Zielgeraet: `idf.py build` baut den StickS3 wie bisher,
# `idf.py -B build-watch206 -DVOICE_BOARD=watch206 build` die Waveshare-Uhr.
#
# Der Schalter setzt drei Dinge, die zusammengehoeren:
#   - die sdkconfig.defaults-Datei(en), aus der Kconfig die Board-Wahl liest
#     (CONFIG_VOICE_BOARD_WATCH206 -> Pins, Treiber, Layout),
#   - einen eigenen sdkconfig-Pfad im Bauordner, damit sich die beiden
#     Fassungen nicht gegenseitig die Konfiguration ueberschreiben,
#   - den Projektnamen. Der landet in der App-Beschreibung jedes Abbilds, und
#     daran lehnt der OTA-Pfad (voice_ble.c) ein Abbild fuer das andere Geraet
#     ab, bevor es gebootet wird.
set(VOICE_BOARD "sticks3" CACHE STRING "Zielgeraet: sticks3 oder watch206")

if(VOICE_BOARD STREQUAL "watch206")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.watch206")
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
    set(VOICE_PROJECT_NAME voice_watch)
elseif(VOICE_BOARD STREQUAL "sticks3")
    set(VOICE_PROJECT_NAME voice_stick)
else()
    message(FATAL_ERROR "VOICE_BOARD=${VOICE_BOARD} unbekannt (sticks3 oder watch206)")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${VOICE_PROJECT_NAME})
