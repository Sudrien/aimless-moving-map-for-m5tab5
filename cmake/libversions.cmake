# SPDX-License-Identifier: MIT
#
# 0037: the libraries and data this build was made from, for the
# licences page opened from the settings -- written as a header,
# libversions.h, that main includes. unprofitable-mahjong's
# m5tab5/cmake/libversions.cmake (itself defeatist's), cut to this
# firmware: each entry is a module, its checkout (a version, a git
# commit's first seven characters, or a date) and its licence.
#
# From the records the build itself uses, so the screen cannot drift
# from what was compiled:
#
#   dependencies.lock          every managed component and the version the
#                              component manager resolved (git ones by
#                              commit); its licence from the licence file
#                              in managed_components/, the copy compiled
#   gen_ark12.py               the Ark Pixel commit the font was cut from,
#                              in feckless_graphics_handler's tools/
#   tools/                     the Bosch commit the BMI270 image is
#     fetch_bmi270_config.py   fetched at (0032), if the fetch got it
#   build/world_z0.mvt.gz      the Protomaps build the world tile came
#     .source                  from (0020), written by fetch_worldtile.py;
#                              a daily build's name is its date
#   git                        this project's own commit
#
# Include AFTER project(): main/CMakeLists.txt does the two fetches while
# project() configures the components. Sets LIBVERSIONS_DIR to the
# directory holding libversions.h, which is rewritten only when its text
# changes.
#
# Also runs on its own, for a look at what it finds without ESP-IDF:
#   cmake -P cmake/libversions.cmake      (writes ./libversions/, and
#                                          reads ./world_z0.mvt.gz*)

set(_root "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(_root "${_root}" ABSOLUTE)
set(_lock "")
if(COMMAND idf_build_get_property)
    idf_build_get_property(_lock DEPENDENCIES_LOCK)
endif()
if(NOT _lock)
    set(_lock "${_root}/dependencies.lock")
endif()
set(_libs "")   # "module|checkout|licence", one each

# A file whose change should re-run this; not in script mode (cmake -P),
# which has no directory to hang it on.
macro(_lv_depends f)
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${f}")
    endif()
endmacro()

# A 40-hex git commit as its first seven; anything else as written.
function(_lv_short out ver)
    string(LENGTH "${ver}" _len)
    if(_len EQUAL 40 AND ver MATCHES "^[0-9a-f]+$")
        string(SUBSTRING "${ver}" 0 7 ver)
    endif()
    set(${out} "${ver}" PARENT_SCOPE)
endfunction()

# A component's licence, from the licence file in its directory, else
# its idf_component.yml's license field.
function(_lv_license out dir)
    set(_lic "")
    file(GLOB _files LIST_DIRECTORIES false
         "${dir}/license.txt" "${dir}/LICENSE" "${dir}/LICENSE.*"
         "${dir}/LICENCE" "${dir}/LICENSE-*")
    foreach(_f IN LISTS _files)
        file(STRINGS "${_f}" _head LIMIT_COUNT 15)
        string(JOIN "\n" _head ${_head})
        if(_head MATCHES "(Valid|SPDX)-License-Identifier: *([A-Za-z0-9.+-]+)")
            set(_lic "${CMAKE_MATCH_2}")
        elseif(_head MATCHES "Espressif Modified MIT")
            set(_lic "Espressif MIT")
        elseif(_head MATCHES "Apache License" AND _head MATCHES "Version 2\\.0")
            set(_lic "Apache-2.0")
        elseif(_head MATCHES "MIT License")
            set(_lic "MIT")
        elseif(_head MATCHES "Open Font License")
            set(_lic "OFL-1.1")
        endif()
        if(_lic)
            break()
        endif()
    endforeach()
    if(NOT _lic AND EXISTS "${dir}/idf_component.yml")
        file(STRINGS "${dir}/idf_component.yml" _f REGEX "^license:")
        if(_f MATCHES "^license: *[\"']?([^\"']+)")
            set(_lic "${CMAKE_MATCH_1}")
        endif()
    endif()
    if(NOT _lic)
        set(_lic "licence not found")
    endif()
    set(${out} "${_lic}" PARENT_SCOPE)
endfunction()

# This project, at the commit it was built from.
set(_self "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD WORKING_DIRECTORY "${_root}"
                    OUTPUT_VARIABLE _self OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0 OR NOT _self)
        set(_self "unknown")
    endif()
endif()
_lv_short(_self "${_self}")
list(APPEND _libs "Aimless Moving Map|${_self}|MIT")

# The managed components, as the lock has them. "espressif/esp_lcd_touch"
# is managed_components/espressif__esp_lcd_touch, a git one by its bare name.
set(_managed "${_root}/managed_components")
if(EXISTS "${_lock}")
    _lv_depends("${_lock}")
    file(STRINGS "${_lock}" _lines)
    set(_name "")
    set(_in_deps FALSE)
    foreach(_l IN LISTS _lines)
        if(_l STREQUAL "dependencies:")
            set(_in_deps TRUE)
        elseif(_l MATCHES "^[a-z_]+:")          # direct_dependencies, target, ...
            set(_in_deps FALSE)
        elseif(_in_deps AND _l MATCHES "^  ([^ ][^:]*):$")
            set(_name "${CMAKE_MATCH_1}")
        elseif(_in_deps AND _name AND _l MATCHES "^    version: '?([^']*)'?$")
            # four spaces: the component's own version, not a requirement
            set(_ver "${CMAKE_MATCH_1}")
            string(REGEX REPLACE "^espressif/" "" _short "${_name}")
            if(_short STREQUAL "idf")
                set(_lic "Apache-2.0")
                if(DEFINED ENV{IDF_PATH})
                    _lv_license(_lic "$ENV{IDF_PATH}")
                endif()
                list(APPEND _libs "ESP-IDF|${_ver}|${_lic}")
            elseif(NOT _short STREQUAL "cmake_utilities")
                _lv_short(_ver "${_ver}")
                string(REPLACE "/" "__" _dir "${_name}")
                _lv_license(_lic "${_managed}/${_dir}")
                list(APPEND _libs "${_short}|${_ver}|${_lic}")
            endif()
            set(_name "")
        endif()
    endforeach()
else()
    list(APPEND _libs "dependencies.lock|missing|")
endif()

# cJSON, for M5Launcher's config.conf: IDF's own json component on 5.x,
# not in the lock; on 6.x it is espressif/cjson, which the lock lists.
if(DEFINED IDF_VERSION_MAJOR AND IDF_VERSION_MAJOR LESS 6)
    list(APPEND _libs "cJSON (ESP-IDF json)|in ESP-IDF|MIT")
endif()

# The Ark Pixel font, compiled in through feckless_graphics_handler: its
# LICENSE-OFL has to travel with the firmware.
set(_ark "${_managed}/feckless_graphics_handler/tools/gen_ark12.py")
if(EXISTS "${_ark}")
    _lv_depends("${_ark}")
    file(STRINGS "${_ark}" _v REGEX "^ARK_COMMIT *=")
    string(REGEX REPLACE "^ARK_COMMIT *= *\"([0-9a-f]+)\".*$" "\\1" _v "${_v}")
    _lv_short(_v "${_v}")
    list(APPEND _libs "Ark Pixel font (TakWolf)|${_v}|OFL-1.1")
else()
    list(APPEND _libs "Ark Pixel font (TakWolf)|-|OFL-1.1")
endif()

# The BMI270's configuration image (0032), Bosch's, embedded: the commit
# fetch_bmi270_config.py pins, when the fetch got it. Without it the
# firmware has no accelerometer and nothing of Bosch's in it.
set(_bmi_py "${_root}/tools/fetch_bmi270_config.py")
set(_bmi_bin "${CMAKE_BINARY_DIR}/bmi270_config.bin")
set(_v "not fetched")
if(EXISTS "${_bmi_bin}")
    file(SIZE "${_bmi_bin}" _sz)
    if(_sz GREATER 0 AND EXISTS "${_bmi_py}")
        _lv_depends("${_bmi_py}")
        file(STRINGS "${_bmi_py}" _c REGEX "^COMMIT *=")
        string(REGEX REPLACE "^COMMIT *= *\"([0-9a-f]+)\".*$" "\\1" _c "${_c}")
        _lv_short(_v "${_c}")
    endif()
endif()
list(APPEND _libs "BMI270 config (Bosch Sensortec)|${_v}|BSD-3-Clause")

# The world at boot (0020): tile z0/0/0, OpenStreetMap data by way of
# Protomaps, embedded. Its checkout is the build it was extracted from --
# a daily build's name is its date, shown as one -- or "not fetched".
set(_world "${CMAKE_BINARY_DIR}/world_z0.mvt.gz")
set(_v "not fetched")
if(EXISTS "${_world}" AND EXISTS "${_world}.source")
    file(SIZE "${_world}" _sz)
    file(STRINGS "${_world}.source" _src LIMIT_COUNT 1)
    if(_sz GREATER 0 AND _src)
        if(_src MATCHES "^([0-9][0-9][0-9][0-9])([0-9][0-9])([0-9][0-9])$")
            set(_v "${CMAKE_MATCH_1}-${CMAKE_MATCH_2}-${CMAKE_MATCH_3}")
        else()
            set(_v "${_src}")
        endif()
    endif()
elseif(EXISTS "${_world}")
    file(SIZE "${_world}" _sz)
    if(_sz GREATER 0)
        set(_v "unknown build")     # fetched before 0037 wrote .source
    endif()
endif()
list(APPEND _libs "World map, (c) OpenStreetMap contributors (Protomaps)|${_v}|ODbL-1.0")
# What the card holds or the network serves is not in the firmware, and
# is under its provider's terms; said, so the line above is not read as
# covering it.
list(APPEND _libs "Map tiles on the card or fetched|-|their provider's")

list(LENGTH _libs _n)
set(_h "/* Generated by cmake/libversions.cmake. Do not edit. */\n")
string(APPEND _h "#pragma once\n#define LIBVERSIONS_COUNT (${_n})\n")
string(APPEND _h "/* module, checkout, licence */\n")
string(APPEND _h "static const char *const k_libversions[LIBVERSIONS_COUNT][3] = {\n")
foreach(_l IN LISTS _libs)
    string(REPLACE "|" "\", \"" _l "${_l}")
    string(APPEND _h "    { \"${_l}\" },\n")
endforeach()
string(APPEND _h "};\n")

set(LIBVERSIONS_DIR "${CMAKE_BINARY_DIR}/libversions")
file(WRITE "${LIBVERSIONS_DIR}/libversions.h.tmp" "${_h}")
configure_file("${LIBVERSIONS_DIR}/libversions.h.tmp"
               "${LIBVERSIONS_DIR}/libversions.h" COPYONLY)
message(STATUS "libversions: ${_n} entries for the licences page")
