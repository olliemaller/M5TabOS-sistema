# GLOB_RECURSE picks up the sources in main/ AND in any subfolder - so each
# application lives in its own folder without anyone touching this file.
#
# Filtering by source extension (instead of the "*.*" PlatformIO generates by
# default) so as not to throw headers and .yml into SRCS.
#
# CONFIGURE_DEPENDS makes ninja re-evaluate the glob on every build, so a new .cpp
# goes in without having to delete .pio/build. BUT it is forbidden in "script mode", and
# IDF runs this file in script mode (cmake -P) to discover the
# component's dependencies before configuring. CMAKE_SCRIPT_MODE_FILE only
# exists in that pass - it is the way to tell the two apart.
if(CMAKE_SCRIPT_MODE_FILE)
    FILE(GLOB_RECURSE app_sources
         ${CMAKE_CURRENT_SOURCE_DIR}/*.c
         ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
else()
    FILE(GLOB_RECURSE app_sources CONFIGURE_DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/*.c
         ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
endif()

# APPS LEFT OUT OF THIS BUILD, without deleting the folder: -DM5_SEM_APPS="cobra;radar" removes
# the folders main/app_cobra/ and main/app_radar/ from the sources and from the app list.
# It is what ferramentas/m5burner.sh uses so the published firmware does not ship
# what is still a project in progress.
set(M5_SEM_APPS "" CACHE STRING "Apps (folder name without app_) left out of this build")

# THE SYSTEM APPS, in the home screen's factory order (see "A LISTA DE
# APLICATIVOS" (THE APP LIST) below). It is also what the sistema and berry variants
# ship, and what ferramentas/publicar.sh copies to the public repository.
set(m5_apps_sistema mesh espnow config arquivos notas musica video terminal editor tarefas energia imu camera exemplo)

# WHO IS LEFT OUT: those from -DM5_SEM_APPS, those from the VARIANT (variantes/*.cmake,
# see docs/VARIANTES.md) and, with M5_SO_SISTEMA, every app that is not a system app.
set(m5_apps_fora ${M5_SEM_APPS} ${M5_SEM_APPS_VARIANTE})
# The Editor is Berry's: it only exists in the variant that has Berry.
if(NOT M5_BERRY)
    list(APPEND m5_apps_fora editor)
endif()
if(M5_SO_SISTEMA)
    file(GLOB m5_todas LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/app_*)
    foreach(d ${m5_todas})
        get_filename_component(n ${d} NAME)
        string(REGEX REPLACE "^app_" "" n ${n})
        if(IS_DIRECTORY ${d} AND NOT n IN_LIST m5_apps_sistema)
            list(APPEND m5_apps_fora ${n})
        endif()
    endforeach()
endif()
foreach(n ${m5_apps_fora})
    list(FILTER app_sources EXCLUDE REGEX "/main/app_${n}/")
endforeach()

# BERRY ONLY IN THE VARIANT THAT ASKS FOR IT: without it, services/berry_* and services/berry/
# (the modules) are not even compiled (the components/berry component goes too - see the top-level CMakeLists.txt),
# and services/apps_de_fora.cpp becomes the empty version (M5_COM_BERRY=0).
if(NOT M5_BERRY)
    list(FILTER app_sources EXCLUDE REGEX "/main/services/berry[_/]")
endif()

# INCLUDE_DIRS "." puts main/ itself on the search path. Without it, a
# file in main/app_azimutal/ would have to write #include "../app.h"; with it,
# it writes #include "app.h" from any depth - BUT ONLY while app.h
# sits directly inside "." (main/). IT IS NOT RECURSIVE: each entry here is
# ONE exact folder, not a whole tree - unlike the GLOB_RECURSE above,
# which picks up .cpp files at any depth by itself.
#
# kernel/, hal/ and services/ were born (a reorganisation of the
# files that until then sat loose directly in main/, grouping them by
# layer - see the folder names themselves). The headers there (app.h,
# tab5_grove.hpp, tab5_mesh.hpp...) stopped being found by #include
# "app.h" hitting only "." - which is why each new folder goes in here, and no
# #include in any .cpp of the project needed to change.
#
# compartilhado/ brings the message contract with the C6 firmware. It does NOT
# go into SRCS - it is header-only, and the glob above does not even pass through it.
# LDFRAGMENTS: linker.lf sends the .bss of apps and services to PSRAM (see the
# header of that file for the rule on who may go in).
# THE BUILD STAMP ("Compilado em" (built on), in Settings > About). It used to be
# __DATE__/__TIME__ inside app_config.cpp - and that only changes when THAT
# file recompiles: touching any other part of the system left the date
# stale. Now a .c generated at build time (main/carimbo.cmake) is rewritten on every
# 'idf.py build'; the target that generates it always runs, before libmain.
set(m5_carimbo_c "${CMAKE_CURRENT_BINARY_DIR}/m5tabos_compilado.c")
if(NOT CMAKE_SCRIPT_MODE_FILE)
    execute_process(COMMAND ${CMAKE_COMMAND} -DOUT=${m5_carimbo_c}
                            -P ${CMAKE_CURRENT_SOURCE_DIR}/carimbo.cmake)
    list(APPEND app_sources ${m5_carimbo_c})
endif()

# THE APP LIST COMES FROM THE FOLDERS. Every folder main/app_<nome>/ is an app,
# and its app_t is called APP_<NOME> (in capitals): app_mesh -> APP_MESH.
# kernel/apps.cpp includes the file generated here. So an app goes in by copying the
# folder and comes out by deleting the folder - nobody edits any list, and a build without
# the personal apps (the public copy) compiles the same way.
#
# Factory order: the system apps in the order below, then the others in
# alphabetical order. The order that applies on the home screen is the user's (saved in NVS
# by name - see ajustes_ordem_apps), so this only decides the first boot.
set(m5_apps_inc "${CMAKE_CURRENT_BINARY_DIR}/apps_lista.inc")
if(NOT CMAKE_SCRIPT_MODE_FILE)
    file(GLOB m5_app_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS
         ${CMAKE_CURRENT_SOURCE_DIR}/app_*)
    set(m5_apps_achados "")
    foreach(d ${m5_app_dirs})
        if(IS_DIRECTORY ${d})
            get_filename_component(n ${d} NAME)
            string(REGEX REPLACE "^app_" "" n ${n})
            if(NOT n IN_LIST m5_apps_fora)
                list(APPEND m5_apps_achados ${n})
            endif()
        endif()
    endforeach()
    list(SORT m5_apps_achados)
    set(m5_apps_ordem "")
    foreach(n ${m5_apps_sistema})
        if(n IN_LIST m5_apps_achados)
            list(APPEND m5_apps_ordem ${n})
        endif()
    endforeach()
    foreach(n ${m5_apps_achados})
        if(NOT n IN_LIST m5_apps_ordem)
            list(APPEND m5_apps_ordem ${n})
        endif()
    endforeach()
    set(m5_txt "// Gerado por main/CMakeLists.txt a partir das pastas main/app_* - nao editar.\n")
    foreach(n ${m5_apps_ordem})
        string(TOUPPER ${n} N)
        string(APPEND m5_txt "APP(APP_${N})\n")
    endforeach()
    if(EXISTS "${m5_apps_inc}")
        file(READ "${m5_apps_inc}" m5_antes)
    endif()
    if(NOT "${m5_antes}" STREQUAL "${m5_txt}")
        file(WRITE "${m5_apps_inc}" "${m5_txt}")
    endif()
endif()

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "." "kernel" "hal" "services" "../compartilhado"
                       EMBED_TXTFILES "../idiomas/en.po"
                       LDFRAGMENTS "linker.lf")

if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_custom_target(m5tabos_carimbo
                      COMMAND ${CMAKE_COMMAND} -DOUT=${m5_carimbo_c}
                              -P ${CMAKE_CURRENT_SOURCE_DIR}/carimbo.cmake
                      BYPRODUCTS ${m5_carimbo_c}
                      VERBATIM)
    add_dependencies(${COMPONENT_LIB} m5tabos_carimbo)
endif()

# THE CAMERA'S SCCB GOES THROUGH THE INTERNAL I2C LOCK. esp_video
# creates the sensor channel with sccb_new_i2c_io(); the link diverts that call to
# hal/tab5_sccb.cpp, which uses M5GFX's transactions (serialised per port).
# See the big warning in that file - without editing third-party components.
# --undefined forces the linker to pull hal/tab5_sccb.o out of libmain: it is read
# BEFORE esp_video, and without this nobody has asked for the __wrap_ yet when it goes by.
# THE DPI DRIVER'S OWN PLAIN DMA2D (esp_async_fbcpy): a rectangle copy in
# a continuous line, without the PPA scaling engine. The header is private to esp_lcd;
# the code is already in its library, only the .h path is missing.
target_include_directories(${COMPONENT_LIB} PRIVATE "$ENV{IDF_PATH}/components/esp_lcd/priv_include")

# THE BROWSER PORTAL (services/tab5_web_portal): the web server root.
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/services/tab5_web_portal.html" TEXT)
# THE CAMERA IN THE BROWSER (services/tab5_camera_web): served from flash.
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/services/tab5_camera_web.html" TEXT)
# THE MESH PAGE IN THE BROWSER (services/tab5_mesh_web): served from flash.
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/services/tab5_mesh_web.html" TEXT)
# THE TERMINAL IN THE BROWSER (services/tab5_terminal_web): the page and the compressed
# xterm.js (services/xterm/ORIGEM.txt) - the PC does not need internet.
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/services/tab5_terminal_web.html" TEXT)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/services/xterm/xterm.js.gz" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/services/xterm/xterm.css.gz" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/services/xterm/addon-fit.js.gz" BINARY)

# Whoever needs to know whether Berry is in the binary (services/apps_de_fora.cpp,
# the console 'berry' command) asks via M5_COM_BERRY.
if(M5_BERRY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE M5_COM_BERRY=1)
    # THE EXAMPLE APP GOES INSIDE THE FIRMWARE: on the first boot with an SD card
    # without apps/Berry, the system writes it there (services/berry_apps.cpp) - whoever
    # flashes via M5Burner starts with a Berry app on the home screen already.
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/../berry_exemplos/Ola.be" TEXT)
    # THE WEB EDITOR PAGE (services/berry_web): served from flash, whole.
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/services/berry_web.html" TEXT)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE M5_COM_BERRY=0)
endif()

# Where the apps_lista.inc generated up above lives.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# TINYUSB READS ITS CONFIGURATION FROM HERE. The library looks for "tusb_config.h"
# by name; the file lives in main/ (services/tab5_pendrive.cpp also
# uses it) and this folder goes only into its search path. The component name
# changes if TinyUSB ever comes from a local copy instead of the registry.
idf_build_get_property(build_components BUILD_COMPONENTS)
if(tinyusb IN_LIST build_components)
    set(tinyusb_nome tinyusb)
else()
    set(tinyusb_nome espressif__tinyusb)
endif()
idf_component_get_property(tusb_lib ${tinyusb_nome} COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=sccb_new_i2c_io"
                                                 "-Wl,--undefined=__wrap_sccb_new_i2c_io")

# THE SCREEN'S DMA CHANNEL IS DELETED WHEN STOPPED (hal/tab5_dma.cpp):
# esp_lcd_panel_del of the DPI panel deletes the DW-GDMA channel while still running, and with
# the camera keeping the group up the waking screen crashed when reusing the channel.
# AND THAT DMA'S INTERRUPT IS BORN AT LEVEL 3 AND ALWAYS ON CORE 1 (the blue
# flicker: the delayed ISR left the panel one frame without a picture).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=dw_gdma_del_channel"
                                                 "-Wl,--wrap=dw_gdma_new_channel"
                                                 "-Wl,--wrap=dw_gdma_channel_register_event_callbacks"
                                                 "-Wl,--undefined=__wrap_dw_gdma_del_channel"
                                                 "-Wl,--undefined=__wrap_dw_gdma_new_channel"
                                                 "-Wl,--undefined=__wrap_dw_gdma_channel_register_event_callbacks")

# CAMERA WHITE PROTECTION. esp_video gets the list of ISP algorithms
# from esp_ipa_pipeline_get_config(); the wrap returns the same list with our own
# IPA at the end (hal/tab5_camera_ipa.c, see the reason there). Same --undefined:
# it is what pulls in the .o, and with it the entry in the .esp_ipa_detect section.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_ipa_pipeline_get_config"
                                                 "-Wl,--undefined=__wrap_esp_ipa_pipeline_get_config")

# CLOCK-CHANGE MARKERS. Every esp_pm frequency change goes
# through rtc_clk_cpu_freq_set_config_fast() - and before it IDF readjusts the
# PSRAM bus. Diverted to hal/tab5_marcos.cpp, which notes the instant
# for the monitor to cross-reference with the late frame. Same --undefined as SCCB.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=rtc_clk_cpu_freq_set_config_fast"
                                                 "-Wl,--undefined=__wrap_rtc_clk_cpu_freq_set_config_fast")

# More suspects for the late frame: card sectors and large cache
# syncs (hal/tab5_marcos.cpp).
# THE C6 SLEEPING ASSOCIATED (hal/tab5_c6_sono.cpp): when re-mounting SDIO,
# esp_hosted asks whether the host came back from deep sleep - and only then does it NOT reset the
# C6. The wrap answers "back via GPIO" while the link is being re-mounted.
# esp_event_post swallows the repeated STA_START from the re-arm during re-mounting
# (otherwise: duplicate netif_add and a panic in lwIP).
# eh_host_port_restart_host does not restart on the expected failure during teardown
# (the C6 disappears before deinit stops reading).
# eh_host_port_sdio_wait_slave_intr lets the sdio_read task park
# before deinit (deleted by force in transit, it brought the system down).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=eh_host_port_wakeup_reason_get"
                                                 "-Wl,--wrap=esp_wifi_remote_channel_rx"
                                                 "-Wl,--wrap=esp_wifi_internal_tx"
                                                 "-Wl,--wrap=esp_wifi_internal_tx_by_ref"
                                                 "-Wl,--wrap=eh_host_feat_rpc_request"
                                                 "-Wl,--wrap=eh_host_feat_rpc_request_sync"
                                                 "-Wl,--wrap=eh_host_feat_rpc_request_sync_best_effort"
                                                 "-Wl,--wrap=eh_host_feat_rpc_send"
                                                 "-Wl,--wrap=esp_event_post"
                                                 "-Wl,--wrap=eh_host_port_restart_host"
                                                 "-Wl,--wrap=eh_host_port_sdio_wait_slave_intr"
                                                 "-Wl,--undefined=__wrap_eh_host_port_wakeup_reason_get")

target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=sdmmc_read_sectors"
                                                 "-Wl,--wrap=sdmmc_write_sectors"
                                                 "-Wl,--wrap=esp_cache_msync"
                                                 "-Wl,--undefined=__wrap_sdmmc_read_sectors"
                                                 "-Wl,--undefined=__wrap_sdmmc_write_sectors"
                                                 "-Wl,--undefined=__wrap_esp_cache_msync")

# THE PSRAM HEAP AT BOOT (tab5_heap_psram.cpp): every large PSRAM allocation
# checks the heap header first - it is where M5.begin used to crash.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=heap_caps_calloc"
                                                 "-Wl,--undefined=__wrap_heap_caps_calloc")

