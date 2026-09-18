cmake_minimum_required(VERSION 3.20.0)

# Single-board firmware, so "west build firmware/fw" needs no -b option.
set(BOARD adafruit_feather_esp32c6/esp32c6/hpcore)

# Panel fitted on the Waveshare module. The "Rev2.1" marking of the PCB names
# the driver board, not the panel.
#   v1: UC8176 controller, BUSY idles high.
#   v2: SSD1683 controller, "V2" sticker on the back, BUSY idles low.
# Select with "west build firmware/fw -- -DPANEL=v2".
set(PANEL v1 CACHE STRING "Waveshare 4.2 inch panel revision (v1 or v2)")
set(panel_overlay ${CMAKE_CURRENT_SOURCE_DIR}/panels/waveshare_4in2_${PANEL}.overlay)
if(NOT EXISTS ${panel_overlay})
  message(FATAL_ERROR "Unknown PANEL '${PANEL}', expected v1 or v2")
endif()
list(APPEND EXTRA_DTC_OVERLAY_FILE ${panel_overlay})

# The Wi-Fi credentials stay out of git, see wifi.conf.example.
set(wifi_conf ${CMAKE_CURRENT_SOURCE_DIR}/wifi.conf)
if(EXISTS ${wifi_conf})
  list(APPEND EXTRA_CONF_FILE ${wifi_conf})
else()
  message(WARNING "fw/wifi.conf is missing, this firmware will not join any Wi-Fi network. "
                  "Copy fw/wifi.conf.example to fw/wifi.conf and fill it in.")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(epaper_notes)

list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
include(nanopb)

# Generated sources live outside src/ or below it, and include its headers.
target_include_directories(app PRIVATE src)

target_sources(app PRIVATE
  src/main.c
  src/canvas.c
  src/upload_server.c
  src/wifi_link.c
  src/fonts/font_title.c
  src/fonts/font_body.c
)

# The upload protocol, generated as proto/epaper.pb.h in the build directory.
zephyr_nanopb_sources(app proto/epaper.proto)

# assets/picture.png is dithered to 1 bpp and compiled in at build time.
set(picture_c ${CMAKE_CURRENT_BINARY_DIR}/picture.c)
add_custom_command(
  OUTPUT ${picture_c}
  COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/img2c.py
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/picture.png ${picture_c}
          --name picture --max-width 160 --max-height 160
  DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/img2c.py
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/picture.png
)
target_sources(app PRIVATE ${picture_c})
