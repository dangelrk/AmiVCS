# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
# amivcs-gui-qt -- Qt 6 Widgets git-gui-style frontend for AMIVCS
# (docs/design/amivcs-gui-plan.md). Unlike amicloud-commander-qt
# (which compiles libcloudcore/libcloudfs/libcloudhttp's .c sources
# directly into its own target alongside core/common/panel.c), this app
# needs no C source compiled together with the C++ sources -- it links
# the prebuilt static libs the same way the CLI apps' Makefile.pc do, via
# a "build_libs" step that runs each lib's own Makefile.pc first.
#
# Build:
#   cmake -S apps/amivcs-gui-qt -B build/amivcs-gui-qt
#   cmake --build build/amivcs-gui-qt
# (or simply "make qt" in the project root, see docs/BUILD.md)
# Output: <repo-root>/bin/pc/amivcs-gui (shared top-level bin dir)
cmake_minimum_required(VERSION 3.16)
project(amivcs-gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# No .ui files -- AUTOUIC would misinterpret our ui_hooks.h include as a
# "ui_<name>.h generated from <name>.ui", same reason amicloud-commander-qt
# leaves it off.

find_package(Qt6 REQUIRED COMPONENTS Widgets)

find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(CURL libcurl)
endif()
if(NOT CURL_FOUND)
    set(CURL_INCLUDE_DIRS /opt/homebrew/include /usr/local/include)
    set(CURL_LIBRARIES curl)
    set(CURL_CFLAGS_OTHER "")
    link_directories(/opt/homebrew/lib /usr/local/lib)
endif()

set(APPS_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(ROOT_DIR    ${APPS_DIR}/..)
set(LIBS_DIR    ${ROOT_DIR}/core)
set(LIB_OUT_DIR ${ROOT_DIR}/lib/pc)

set(CORE_DIR ${LIBS_DIR}/libcloudcore)
set(FS_DIR   ${LIBS_DIR}/libcloudfs)
set(HTTP_DIR ${LIBS_DIR}/libcloudhttp)
set(VCS_DIR  ${ROOT_DIR}/libs/libcloudvcs)
set(GUI_DIR  ${ROOT_DIR}/libs/libcloudvcsgui)

add_custom_target(amivcs_gui_build_libs
    COMMAND ${CMAKE_MAKE_PROGRAM} -C ${CORE_DIR} -f Makefile.pc
    COMMAND ${CMAKE_MAKE_PROGRAM} -C ${HTTP_DIR} -f Makefile.pc
    COMMAND ${CMAKE_MAKE_PROGRAM} -C ${FS_DIR}   -f Makefile.pc
    COMMAND ${CMAKE_MAKE_PROGRAM} -C ${VCS_DIR}  -f Makefile.pc
    COMMAND ${CMAKE_MAKE_PROGRAM} -C ${GUI_DIR}  -f Makefile.pc
    COMMENT "Building libcloudcore/fs/http/vcs/vcsgui via their own Makefile.pc"
    VERBATIM
)

add_executable(amivcs-gui
    src/main.cpp
    src/mainwindow.cpp
    src/logtablemodel.cpp
    src/graphdelegate.cpp
    src/remotejob.cpp
    src/remotedialog.cpp
)
add_dependencies(amivcs-gui amivcs_gui_build_libs)

target_include_directories(amivcs-gui PRIVATE
    ${CORE_DIR}/include/cloudcore
    ${HTTP_DIR}/include/cloudhttp
    ${FS_DIR}/include/cloudfs
    ${VCS_DIR}/include/cloudvcs
    ${GUI_DIR}/include/cloudvcsgui
    ${CURL_INCLUDE_DIRS}
)
target_link_directories(amivcs-gui PRIVATE ${LIB_OUT_DIR})
target_link_libraries(amivcs-gui PRIVATE
    Qt6::Widgets
    cloudvcsgui cloudvcs cloudhttp cloudfs cloudcore
    ${CURL_LIBRARIES}
)
target_compile_options(amivcs-gui PRIVATE ${CURL_CFLAGS_OTHER})

set_target_properties(amivcs-gui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../bin/pc
)
