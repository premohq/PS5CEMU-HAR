# SPDX-License-Identifier: GPL-3.0-or-later
# PS5CEMU-HAR: melonDS's PS5 frontend (core.cpp, platform.cpp, input.cpp, sound.cpp), built in
# melonDS's tree (tools/build-melonds.sh names this directory MELONDS_EXTERNAL_FRONTEND) against its
# core. The rest of port/melonds (the launcher's DS games, the screens on VideoOut, melonds.h) is
# built with the app (port/CMakeLists.txt), which links what this builds: melonds_ps5_libraries.txt
# lists the archives.

# The app links Azahar too, whose teakra (the DSi's DSP, which DS games never start) and xxHash are
# other versions of melonDS's copies: melonDS's are renamed, so each emulator keeps its own.
target_compile_definitions(teakra PUBLIC Teakra=melonTeakra)
target_compile_definitions(core PRIVATE Teakra=melonTeakra XXH_NAMESPACE=melonds_)
# FatFs's clock asks for POSIX 1988 (_POSIX_SOURCE), whose time.h on the PS5 (FreeBSD's) has no
# localtime_r: the 2008 one, which has
set_source_files_properties("${CMAKE_SOURCE_DIR}/src/fatfs/ffsystem.c" DIRECTORY "${CMAKE_SOURCE_DIR}/src"
    PROPERTIES COMPILE_DEFINITIONS _POSIX_C_SOURCE=200809L)
# the JIT's code in the platform layer's executable direct memory (patch 0002)
if (PS5_PLATFORM_INCLUDE)
    target_include_directories(core PRIVATE ${PS5_PLATFORM_INCLUDE})
endif()

add_library(melonds_ps5 STATIC
    core.cpp
    input.cpp
    platform.cpp
    sound.cpp
    frontend.h
)
# the in-game menu's header names Vulkan's types: the headers the app's Vulkan code is built with (Cemu's)
target_include_directories(melonds_ps5 PRIVATE "${CMAKE_CURRENT_LIST_DIR}/.." ${PS5_VULKAN_INCLUDE})
target_compile_definitions(melonds_ps5 PRIVATE Teakra=melonTeakra)
target_link_libraries(melonds_ps5 PUBLIC core)

# The archives the app links for melonDS, in link order: an executable whose link only writes them
# down (port/azahar/record-libraries.sh), into melonds_ps5_libraries.txt in the build directory.
add_executable(melonds_ps5_libraries libraries.cpp)
target_link_libraries(melonds_ps5_libraries PRIVATE melonds_ps5)
set(CMAKE_CXX_LINK_EXECUTABLE
    "bash \"${CMAKE_CURRENT_LIST_DIR}/../azahar/record-libraries.sh\" \"${CMAKE_BINARY_DIR}/melonds_ps5_libraries.txt\" <TARGET> <LINK_LIBRARIES>")
