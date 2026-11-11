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

# On Windows libcurl's own CMake package (-DCMAKE_PREFIX_PATH=<curl install dir>)
if(WIN32)
    find_package(CURL REQUIRED CONFIG)
    set(CURL_INCLUDE_DIRS "")
    set(CURL_LIBRARIES CURL::libcurl)
    set(CURL_CFLAGS_OTHER "")
else()
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

# The libraries are built by their own (GNU make) Makefile.pc -- not by
# ${CMAKE_MAKE_PROGRAM}, which is ninja with the Ninja generator. Runs on
# every build; the sub-makes only rebuild what changed.
find_program(GNU_MAKE NAMES gmake make mingw32-make)
if(NOT GNU_MAKE)
    message(FATAL_ERROR "GNU make not found -- needed to build the libraries (Makefile.pc)")
endif()
set(LIB_ARCHIVES
    ${LIB_OUT_DIR}/libcloudvcsgui.a
    ${LIB_OUT_DIR}/libcloudvcs.a
    ${LIB_OUT_DIR}/libcloudhttp.a
    ${LIB_OUT_DIR}/libcloudfs.a
    ${LIB_OUT_DIR}/libcloudcore.a
)
add_custom_target(amivcs_gui_build_libs
    COMMAND ${GNU_MAKE} -C ${CORE_DIR} -f Makefile.pc
    COMMAND ${GNU_MAKE} -C ${HTTP_DIR} -f Makefile.pc
    COMMAND ${GNU_MAKE} -C ${FS_DIR}   -f Makefile.pc
    COMMAND ${GNU_MAKE} -C ${VCS_DIR}  -f Makefile.pc
    COMMAND ${GNU_MAKE} -C ${GUI_DIR}  -f Makefile.pc
    BYPRODUCTS ${LIB_ARCHIVES}
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
# Full archive paths (not just -lcloudvcs ...) so the build system knows
# the files: a rebuilt archive (e.g. after a core/ change) relinks the GUI.
target_link_libraries(amivcs-gui PRIVATE
    Qt6::Widgets
    ${LIB_ARCHIVES}
    ${CURL_LIBRARIES}
)
target_compile_options(amivcs-gui PRIVATE ${CURL_CFLAGS_OTHER})

set_target_properties(amivcs-gui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../bin/pc
    WIN32_EXECUTABLE ON
)
