cmake_minimum_required(VERSION 3.16)

# exFAT (0018): a patched copy of IDF's fatfs in components/fatfs, made at
# the first configure. Before project.cmake, because IDF scans
# components/ and collects their Kconfig there, and a fatfs that appears
# afterwards builds against a config that lacks its symbols. idfcopy.cmake
# records which IDF the copy came from and stops a configure under
# another. Both are defeatist's; set TAB5_NO_EXFAT to build without.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/idfcopy.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/exfat.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aimless_moving_map_for_m5tab5)
