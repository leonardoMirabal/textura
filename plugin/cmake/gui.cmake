# SPDX-License-Identifier: GPL-3.0-only
# The plugin window: frui's canvas and font (plugin/ui, from Frodhes), the embedded font, the window's core
# (plugin/gui_core.c), its CLAP gui extension (plugin/gui_clap.c) and one platform view behind plugin/gui_platform.h:
#   textura_ui        frui + the core + the extension + the font (any platform)
#   textura_gui_mac   macOS: plugin/gui_mac.m (Cocoa + QuartzCore)
#   textura_gui_win   Windows: plugin/gui_win.c (user32 + gdi32 only, shell32 for the manual's link; not built or tested yet)
#   textura_gui_x11   Linux: plugin/gui_x11.c (libX11 only; not built or tested yet)
# TXP_GUI_VIEW names the one the plugin links. The font, Inter Tight (SIL OFL 1.1, its licence in assets/fonts/), is
# fetched by tools/get_fonts.sh into .local/fonts (sha256-checked; make plugin runs it), never committed.
set(_txg_font "${TXP_ROOT}/.local/fonts/InterTight[wght].ttf")
if(NOT EXISTS "${_txg_font}")
    message(FATAL_ERROR "the window's font is missing (${_txg_font}): run sh tools/get_fonts.sh (make plugin does)")
endif()
set(_txg_font_c "${CMAKE_BINARY_DIR}/gen/txg_font.c")
add_custom_command(OUTPUT "${_txg_font_c}"
    COMMAND ${CMAKE_COMMAND} "-DIN=${_txg_font}" "-DOUT=${_txg_font_c}" -DNAME=txg_font_text
            -P "${TXP_ROOT}/plugin/cmake/embed.cmake"
    DEPENDS "${_txg_font}" "${TXP_ROOT}/plugin/cmake/embed.cmake"
    COMMENT "plugin: embedding the window's font" VERBATIM)

add_library(textura_ui STATIC
    "${TXP_ROOT}/plugin/ui/frui_font.c" "${TXP_ROOT}/plugin/ui/frui_gfx.c" "${TXP_ROOT}/plugin/gui_core.c"
    "${TXP_ROOT}/plugin/gui_clap.c" "${_txg_font_c}")
target_include_directories(textura_ui PUBLIC "${TXP_ROOT}/plugin" "${TXP_ROOT}/plugin/ui" ${TXP_C_WIN_INC})
target_link_libraries(textura_ui PUBLIC clap)
target_compile_options(textura_ui PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 ${TXP_C_STD} ${TXP_C_WARN} ${TXP_C_WIN}>)
set_target_properties(textura_ui PROPERTIES POSITION_INDEPENDENT_CODE ON)
# no fused multiply-adds (arm64 fuses by default, x86_64 cannot): the window's arithmetic the same on every system
if(MSVC)
    target_compile_options(textura_ui PRIVATE /clang:-ffp-contract=off)
else()
    target_compile_options(textura_ui PRIVATE -ffp-contract=off)
endif()
if(NOT MSVC)
    target_compile_options(textura_ui PRIVATE -fvisibility=hidden)
    target_link_libraries(textura_ui PUBLIC m)
endif()

if(APPLE)
    add_library(textura_gui_mac STATIC "${TXP_ROOT}/plugin/gui_mac.m")
    target_link_libraries(textura_gui_mac PUBLIC textura_ui "-framework Cocoa" "-framework QuartzCore")
    target_compile_options(textura_gui_mac PRIVATE -O2 -ffp-contract=off -fvisibility=hidden ${TXP_C_WARN})
    set_target_properties(textura_gui_mac PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set(TXP_GUI_VIEW textura_gui_mac)
elseif(WIN32)
    add_library(textura_gui_win STATIC "${TXP_ROOT}/plugin/gui_win.c")
    target_link_libraries(textura_gui_win PUBLIC textura_ui user32 gdi32 shell32)
    target_compile_options(textura_gui_win PRIVATE -O2 ${TXP_C_STD} ${TXP_C_WARN} ${TXP_C_WIN})
    set(TXP_GUI_VIEW textura_gui_win)
elseif(UNIX)
    find_package(X11 REQUIRED)
    add_library(textura_gui_x11 STATIC "${TXP_ROOT}/plugin/gui_x11.c")
    target_include_directories(textura_gui_x11 PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(textura_gui_x11 PUBLIC textura_ui ${X11_X11_LIB})
    target_compile_options(textura_gui_x11 PRIVATE -O2 ${TXP_C_STD} ${TXP_C_WARN} -fvisibility=hidden)
    set_target_properties(textura_gui_x11 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set(TXP_GUI_VIEW textura_gui_x11)
endif()
