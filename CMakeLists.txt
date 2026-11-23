cmake_minimum_required(VERSION 3.16.0)

# THE SYSTEM VERSION lives in version.txt (one line, SemVer) and only changes via
# ferramentas/lancar.sh - the rules are in docs/VERSIONAMENTO.md.
#
# The number that goes into the binary (esp_app_get_description()->version, what
# appears in Settings, on the console and in the boot log) also says WHERE the build
# came from:
#   0.8.0              - exactly the commit of tag v0.8.0: the released version
#   0.8.0+12.a1b2c3d   - 12 commits after it, at commit a1b2c3d
# Without git (a loose copy of the code), it is just the number from version.txt.
#
# READ AT CONFIGURE TIME, not on every build: a new commit re-runs configuration
# (CMAKE_CONFIGURE_DEPENDS on logs/HEAD), but an uncommitted change does not
# show in the number - "+N.hash" means "built from", not "identical to".
file(STRINGS "${CMAKE_SOURCE_DIR}/version.txt" M5TABOS_VER LIMIT_COUNT 1)
string(STRIP "${M5TABOS_VER}" M5TABOS_VER)
if(NOT M5TABOS_VER MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "version.txt must contain a version X.Y.Z, it has '${M5TABOS_VER}'")
endif()
set(PROJECT_VER "${M5TABOS_VER}")

find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --long --match "v[0-9]*"
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE M5TABOS_DESC OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET RESULT_VARIABLE M5TABOS_GIT_RES)
  if(M5TABOS_GIT_RES EQUAL 0 AND M5TABOS_DESC MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)-([0-9]+)-g([0-9a-f]+)$")
    set(M5TABOS_TAG "${CMAKE_MATCH_1}")
    set(M5TABOS_N   "${CMAKE_MATCH_2}")
    set(M5TABOS_SHA "${CMAKE_MATCH_3}")

    # THE VERSION ONLY MOVES FORWARD. version.txt below the last tag is a
    # manual mistake (merge, revert, old copy): it stops the build before a
    # "0.6" binary comes out after 0.7 was released.
    if(M5TABOS_VER VERSION_LESS M5TABOS_TAG)
      message(FATAL_ERROR "version.txt (${M5TABOS_VER}) is BELOW the last released version (v${M5TABOS_TAG}).")
    endif()
    if(NOT M5TABOS_N EQUAL 0)
      set(PROJECT_VER "${M5TABOS_VER}+${M5TABOS_N}.${M5TABOS_SHA}")
    endif()
  endif()

  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --absolute-git-dir
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE M5TABOS_GITDIR OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(EXISTS "${M5TABOS_GITDIR}/logs/HEAD")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${M5TABOS_GITDIR}/logs/HEAD")
  endif()
endif()
message(STATUS "M5tabOS ${PROJECT_VER}")

# CHIP REVISION (see sdkconfig.defaults.chip_v1 / _v3). The P4 v1.x and v3.x
# need different binaries. Each revision builds in ITS OWN build folder, with the
# sdkconfig INSIDE it - an sdkconfig generated for one revision cannot be
# reused by the other (the revision keys only apply the first time):
#     idf.py build                            -> build/,    P4 v1.x (default)
#     idf.py -B build_v3 -DM5_CHIP=v3 build   -> build_v3/, P4 v3.x
set(M5_CHIP "v1" CACHE STRING "ESP32-P4 revision: v1 (Tab5 so far) or v3")
if(NOT M5_CHIP MATCHES "^v[13]$")
  message(FATAL_ERROR "M5_CHIP must be v1 or v3, it is '${M5_CHIP}'")
endif()
set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults;${CMAKE_SOURCE_DIR}/sdkconfig.defaults.chip_${M5_CHIP}")
message(STATUS "M5tabOS for ESP32-P4 ${M5_CHIP}.x")

# THE VARIANT (see docs/VARIANTES.md). THERE IS ONLY ONE CORE: the variants only change
# WHICH apps go in and whether Berry goes in - never the system code. A
# fix made once applies to all of them.
#     idf.py build                                      -> build/,       completa
#     idf.py -B build_berry -DM5_VARIANTE=berry build   -> build_berry/, berry
set(M5_VARIANTE "completa" CACHE STRING "System variant: completa or berry (variantes/*.cmake)")
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/variantes/${M5_VARIANTE}.cmake")
  message(FATAL_ERROR "M5_VARIANTE '${M5_VARIANTE}' does not exist in variantes/")
endif()
include("${CMAKE_SOURCE_DIR}/variantes/${M5_VARIANTE}.cmake")
# Cache (FORCE): the components (main/) read from here - the variant is in charge.
set(M5_BERRY ${M5_BERRY} CACHE BOOL "Berry inside the binary (set by the variant)" FORCE)
set(M5_SEM_APPS_VARIANTE "${M5_SEM_APPS_VARIANTE}" CACHE STRING "Apps left out because of the variant" FORCE)
set(M5_SO_SISTEMA ${M5_SO_SISTEMA} CACHE BOOL "System apps only (set by the variant)" FORCE)
if(NOT M5_BERRY)
  set(EXCLUDE_COMPONENTS berry)   # the interpreter is not even compiled
endif()
if(M5_VARIANTE_ROTULO)
  set(PROJECT_VER "${PROJECT_VER}-${M5_VARIANTE_ROTULO}")   # Settings/console/log say which one it is
endif()
message(STATUS "M5tabOS variant ${M5_VARIANTE} (Berry: ${M5_BERRY})")

# Off the default (another chip revision or another variant), the sdkconfig lives
# INSIDE the build folder - an sdkconfig belongs to ONE configuration.
if(NOT M5_CHIP STREQUAL "v1" OR NOT M5_VARIANTE STREQUAL "completa")
  set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# A WARNING IS A WARNING, ON ANY ESP-IDF. The official ESP-IDF (the one in the CI image,
# espressif/idf) compiles with -Wall -Werror -Wextra and replaces -Werror with
# -Werror=all: every warning becomes an error. The one that comes with PlatformIO (the one on
# the development machine) has those three options COMMENTED OUT in tools/cmake/
# build.cmake - the same code compiled here and broke on CI, with
# snprintf "may truncate" warnings (format-truncation) in old files. Removing
# -Werror from the list BEFORE project(), the replacement does not happen: the warnings
# still show (-Wall/-Wextra stay), they just do not stop the build - and both
# ESP-IDFs compile the same. On the PlatformIO one this changes nothing.
idf_build_get_property(m5_opcoes COMPILE_OPTIONS)
list(REMOVE_ITEM m5_opcoes "-Werror" "-Werror=all")
idf_build_set_property(COMPILE_OPTIONS "${m5_opcoes}")

project(M5tabOS)
