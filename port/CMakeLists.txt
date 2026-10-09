# SPDX-License-Identifier: GPL-3.0-or-later
# PS5CEMU-HAR: the port's host code, added to Cemu's tree by its CEMU_PS5 option (src/CMakeLists.txt):
# the console layer (ps5/), Cemu's start-up and graphic packs (app/), Azahar's side (azahar/) with its
# DS games (melonds/), and the launcher (frontend/shell/), drawn on the GPU with the UI kit (ui/,
# docs/UI-REDESIGN.md). Cemu's own platform classes (cemu/) are added to Cemu's targets by the patch.

# the release's version, from the Makefile's VERSION (the one place it is set: tools/package.sh
# writes it into sce_sys/param.json too), read again when the Makefile changes
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../Makefile" PS5CEMU_VERSION REGEX "^VERSION := ")
string(REGEX REPLACE "^VERSION := *([^ ]+).*$" "\\1" PS5CEMU_VERSION "${PS5CEMU_VERSION}")
if(NOT PS5CEMU_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+[a-z]?$")
	message(FATAL_ERROR "the Makefile's VERSION ('${PS5CEMU_VERSION}') is not like 2.0.0 or 2.0.0d")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../Makefile")
# the emulators' pinned commits (tools/deps.json), which the diagnostics and the boot log name: they
# are no releases, so their own version numbers would say nothing (Cemu's reads 0.0.0)
file(READ "${CMAKE_CURRENT_LIST_DIR}/../tools/deps.json" PS5CEMU_DEPS_JSON)
string(JSON PS5CEMU_DEPS_COUNT LENGTH "${PS5CEMU_DEPS_JSON}" items)
math(EXPR PS5CEMU_DEPS_LAST "${PS5CEMU_DEPS_COUNT} - 1")
foreach(index RANGE ${PS5CEMU_DEPS_LAST})
	string(JSON name GET "${PS5CEMU_DEPS_JSON}" items ${index} name)
	if(name STREQUAL "cemu" OR name STREQUAL "azahar" OR name STREQUAL "melonds")
		string(JSON commit GET "${PS5CEMU_DEPS_JSON}" items ${index} commit)
		string(SUBSTRING "${commit}" 0 7 commit)
		string(TOUPPER "${name}" name)
		set(PS5CEMU_${name}_COMMIT "${commit}")
	endif()
endforeach()
set(PS5CEMU_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(PS5CEMU_DEPS "${PS5CEMU_ROOT}/.deps")
# the boilerplate's sandbox elevation client (port/ps5/privilege.cpp)
set(SANDBOX_ELEVATION "${PS5CEMU_DEPS}/ps5-native-app-boilerplate/examples/sandbox-elevation")

foreach(input "${SANDBOX_ELEVATION}/src/elevation.cpp" "${PS5CEMU_DEPS}/reshade/deps/stb/stb_image.h")
	if(NOT EXISTS "${input}")
		message(FATAL_ERROR "${input} is missing: run make deps")
	endif()
endforeach()

find_library(FREETYPE_LIBRARY freetype REQUIRED)
# the UI kit (ui/): FreeType for the glyphs Lexend lacks (ui/text.cpp), and stb's image loaders for
# covers and backdrops (ui/images.cpp, compiled static to that file), ReShade's copy
find_path(FREETYPE_INCLUDE_DIR ft2build.h PATH_SUFFIXES freetype2 REQUIRED)
set(STB_INCLUDE_DIR "${PS5CEMU_DEPS}/reshade/deps/stb")
# what pacbrew's FreeType and curl were built with
find_library(PNG16_LIBRARY png16 REQUIRED)
find_library(BZ2_LIBRARY bz2 REQUIRED)
find_library(PSL_LIBRARY psl REQUIRED)
# the box art GameTDB serves (app/boxart.cpp): JPEG, and PNG through libpng16 above
find_library(TURBOJPEG_LIBRARY turbojpeg REQUIRED)

add_library(CemuPS5 STATIC
	app/emulator.cpp
	app/emulator.h
	app/controllers.cpp
	app/boxart.cpp
	app/boxart.h
	app/catalog.cpp
	app/catalog.h
	app/compatibility.cpp
	app/compatibility.h
	app/covers.cpp
	app/gameinfo.cpp
	app/gameinfo.h
	app/graphic_packs.cpp
	app/ingame.cpp
	app/ingame.h
	app/ingame3ds.cpp
	app/ingame3ds.h
	app/install.cpp
	app/updates.cpp
	app/updates.h
	app/usb_devices.cpp
	app/usb_devices.h
	app/menu_canvas.h
	app/pack_updates.cpp
	app/pack_updates.h
	app/paths.h
	app/side_menu.h
	app/tga.h
	azahar/azahar.h
	app/boxart.cpp
	azahar/controls.cpp
	azahar/controls.h
	azahar/library.cpp
	azahar/library.h
	melonds/library.cpp
	melonds/library.h
	melonds/melonds.h
	melonds/screens.cpp
	melonds/screens.h
	melonds/shaders.h
	frontend/actions.cpp
	frontend/actions.h
	frontend/launcher.h
	frontend/settings.cpp
	frontend/settings.h
	frontend/shell.h
	frontend/shell/home.cpp
	frontend/shell/host_ps5.cpp
	frontend/shell/hub.cpp
	frontend/shell/library.cpp
	frontend/shell/pages.cpp
	frontend/shell/settings.cpp
	frontend/shell/setup.cpp
	frontend/shell/shell.cpp
	frontend/shell/shell_internal.h
	frontend/shell/widgets.cpp
	frontend/sound.cpp
	frontend/sound.h
	ps5/crash.cpp
	ps5/crash.h
	ps5/display.cpp
	ps5/display.h
	ps5/kernel.h
	ps5/log.cpp
	ps5/log.h
	ps5/notify.cpp
	ps5/notify.h
	ps5/pad.cpp
	ps5/pad.h
	ps5/privilege.cpp
	ps5/privilege.h
	ps5/threads.cpp
	ps5/threads.h
	ps5/vulkan_display.cpp
	ps5/vulkan_display.h
	ps5/window.h
	ps5/window_system.cpp
	ui/canvas.cpp
	ui/canvas.h
	ui/feedback.cpp
	ui/feedback.h
	ui/gfx.cpp
	ui/gfx.h
	ui/images.cpp
	ui/images.h
	ui/input.cpp
	ui/input.h
	ui/motion.h
	ui/qr.cpp
	ui/qr.h
	ui/shaders.h
	ui/text.cpp
	ui/text.h
	ui/tokens.h
	ui/vkfn.cpp
	ui/vkfn.h
	"${SANDBOX_ELEVATION}/src/elevation.cpp"
)

# Cemu's precompiled header (Common/precompiled.h) for the files that use Cemu's types; the
# launcher's and the console layer's files do not.
cemu_use_precompiled_header(CemuPS5)
set_source_files_properties(
	app/boxart.cpp
	app/catalog.cpp
	app/compatibility.cpp
	app/gameinfo.cpp
	azahar/controls.cpp
	azahar/library.cpp
	azahar/unavailable.cpp
	melonds/library.cpp
	melonds/screens.cpp
	melonds/unavailable.cpp
	frontend/actions.cpp
	frontend/settings.cpp
	frontend/shell/home.cpp
	frontend/shell/host_ps5.cpp
	frontend/shell/hub.cpp
	frontend/shell/library.cpp
	frontend/shell/pages.cpp
	frontend/shell/settings.cpp
	frontend/shell/setup.cpp
	frontend/shell/shell.cpp
	frontend/shell/widgets.cpp
	frontend/sound.cpp
	ps5/display.cpp
	ps5/log.cpp
	ps5/notify.cpp
	ps5/pad.cpp
	ps5/privilege.cpp
	ps5/threads.cpp
	ps5/vulkan_display.cpp
	ui/canvas.cpp
	ui/feedback.cpp
	ui/gfx.cpp
	ui/images.cpp
	ui/input.cpp
	ui/qr.cpp
	ui/text.cpp
	ui/vkfn.cpp
	"${SANDBOX_ELEVATION}/src/elevation.cpp"
	PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

target_include_directories(CemuPS5 PUBLIC
	"${CMAKE_CURRENT_LIST_DIR}"
	"${CMAKE_CURRENT_LIST_DIR}/app"
	"${CMAKE_CURRENT_LIST_DIR}/cemu"
)
target_include_directories(CemuPS5 PRIVATE
	# the platform layer RADV is linked with: its heap's and executable memory's statistics
	"${PS5CEMU_DEPS}/PS5_PayloadSDK/platform/include"
	"${SANDBOX_ELEVATION}"
	"${FREETYPE_INCLUDE_DIR}"
	"${STB_INCLUDE_DIR}"
)
# the version the launcher and the boot log show (the app's param.json has it as 02.000.004)
target_compile_definitions(CemuPS5 PUBLIC PS5CEMU_VERSION="${PS5CEMU_VERSION}"
	PS5CEMU_CEMU_COMMIT="${PS5CEMU_CEMU_COMMIT}" PS5CEMU_AZAHAR_COMMIT="${PS5CEMU_AZAHAR_COMMIT}"
	PS5CEMU_MELONDS_COMMIT="${PS5CEMU_MELONDS_COMMIT}")

target_link_libraries(CemuPS5 PUBLIC
	CemuAudio
	CemuCafe
	CemuCommon
	CemuComponents
	CemuConfig
	CemuGui
	CemuInput
	CemuResource
	CemuUtil
	imguiImpl
	"${FREETYPE_LIBRARY}"
	"${PNG16_LIBRARY}"
	"${BZ2_LIBRARY}"
	"${PSL_LIBRARY}"
	"${TURBOJPEG_LIBRARY}"
	ZLIB::ZLIB
	libzip::zip
)

# Azahar's core and its PS5 frontend (tools/build-azahar.sh builds them in Azahar's tree): the
# archives its build lists. Without that build, 3DS games say why they do not start
# (azahar/unavailable.cpp).
set(PS5CEMU_AZAHAR_LIBRARIES "" CACHE FILEPATH "The list of Azahar's archives (build/azahar/azahar_ps5_libraries.txt)")
if(PS5CEMU_AZAHAR_LIBRARIES AND EXISTS "${PS5CEMU_AZAHAR_LIBRARIES}")
	file(STRINGS "${PS5CEMU_AZAHAR_LIBRARIES}" AZAHAR_ARCHIVES)
	target_link_libraries(CemuPS5 PUBLIC ${AZAHAR_ARCHIVES})
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PS5CEMU_AZAHAR_LIBRARIES}")
	message(STATUS "PS5CEMU-HAR: Azahar's core from ${PS5CEMU_AZAHAR_LIBRARIES}")
else()
	target_sources(CemuPS5 PRIVATE azahar/unavailable.cpp)
	message(STATUS "PS5CEMU-HAR: no Azahar build; 3DS games will not start")
endif()

# melonDS's core and its PS5 frontend, for the 3DS side's DS games (tools/build-melonds.sh builds them
# in melonDS's tree): the archives its build lists. Without that build, DS games are listed and say why
# they do not start (melonds/unavailable.cpp).
set(PS5CEMU_MELONDS_LIBRARIES "" CACHE FILEPATH "The list of melonDS's archives (build/melonds/melonds_ps5_libraries.txt)")
if(PS5CEMU_MELONDS_LIBRARIES AND EXISTS "${PS5CEMU_MELONDS_LIBRARIES}")
	file(STRINGS "${PS5CEMU_MELONDS_LIBRARIES}" MELONDS_ARCHIVES)
	target_link_libraries(CemuPS5 PUBLIC ${MELONDS_ARCHIVES})
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PS5CEMU_MELONDS_LIBRARIES}")
	message(STATUS "PS5CEMU-HAR: melonDS's core from ${PS5CEMU_MELONDS_LIBRARIES}")
else()
	target_sources(CemuPS5 PRIVATE melonds/unavailable.cpp)
	message(STATUS "PS5CEMU-HAR: no melonDS build; DS games will not start")
endif()
