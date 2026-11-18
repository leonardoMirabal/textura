# SPDX-License-Identifier: GPL-3.0-only
# Textura as a desktop plugin: CLAP, and VST3 + Audio Unit v2 through clap-wrapper (MIT). Built the way Frodhes'
# plugin is.
#   tools/plugin_deps.sh                                       the pinned dependencies into .local/plugin-deps
#   cmake -S plugin -B build/plugin -G Ninja && cmake --build build/plugin      (or: make plugin)
# The bundles land in build/plugin/bundles: Textura.clap, Textura.vst3, Textura.component. Nothing is installed.
# Windows x64 and Linux x86_64 (CLAP + VST3): the same tree (plugin/cmake/win-x64-clangcl.cmake, linux_exports.map);
# not built here yet.
# The firmware build never sees this tree; this tree only reads firmware/src, firmware/gen, engines and web
# (plugin/src/engine_unit.c includes firmware/src/textura.c unchanged, over web/web_hal.c).
cmake_minimum_required(VERSION 3.21)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "the oldest macOS the plugin runs on")
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "arm64 by default (\"arm64;x86_64\": universal)")
endif()
project(textura_plugin LANGUAGES C CXX)
if(NOT WIN32)
    enable_language(ASM)           # (the coroutine's switch routines; Windows uses Fibers: plugin/src/coro/coro.h)
endif()
if(APPLE)
    enable_language(OBJC OBJCXX)   # (clap-wrapper's Audio Unit and VST3 sources: at the top level, or Ninja lacks the rules)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(TXP_VERSION "0.0.0-dev" CACHE STRING "the version text (the release's X.Y.Z)")
if(TXP_VERSION MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+)")
    set(TXP_BUNDLE_VERSION "${CMAKE_MATCH_1}")
else()
    set(TXP_BUNDLE_VERSION "0.0.0")
endif()
get_filename_component(TXP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# the C flags of the plugin's own code: GNU-style, or clang-cl's spelling of them (Windows: the CRT shims in
# plugin/compat/win, force-included / on the include path)
if(MSVC)
    set(TXP_C_STD /clang:-std=gnu11 /clang:-ffp-contract=off)
    set(TXP_C_WARN /W3)
    set(TXP_C_WIN "/FI${TXP_ROOT}/plugin/compat/win/txp_win.h")
    set(TXP_C_WIN_INC "${TXP_ROOT}/plugin/compat/win")
else()
    set(TXP_C_STD -std=gnu11 -ffp-contract=off)
    set(TXP_C_WARN -Wall -Wextra)
    set(TXP_C_WIN "")
    set(TXP_C_WIN_INC "")
endif()

# Linux: self-contained modules, no undefined symbols, only the entry points exported, a non-executable stack
if(UNIX AND NOT APPLE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    foreach(_k SHARED MODULE)
        string(APPEND CMAKE_${_k}_LINKER_FLAGS " -static-libstdc++ -static-libgcc -Wl,--no-undefined -Wl,-z,noexecstack"
               " -Wl,--as-needed -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/linux_exports.map -s")
    endforeach()
    find_package(Threads REQUIRED)
endif()

include(cmake/deps.cmake)
# no build-machine paths in the binaries: the tree and the pinned checkouts are written as "textura/..." and "deps/..."
get_filename_component(_txp_root_real "${TXP_ROOT}" REALPATH)
get_filename_component(_txp_deps_real "${TXP_DEPS_DIR}" REALPATH)
foreach(_m IN ITEMS "${TXP_ROOT}=textura" "${_txp_root_real}=textura" "${TXP_DEPS_DIR}=deps" "${_txp_deps_real}=deps")
    if(MSVC)
        add_compile_options("/clang:-ffile-prefix-map=${_m}")
    else()
        add_compile_options("-ffile-prefix-map=${_m}")
    endif()
endforeach()
include(cmake/slots.cmake)

if(APPLE)
    set(CLAP_WRAPPER_BUILD_AUV2 ON CACHE BOOL "" FORCE)
    set(TXP_FORMATS CLAP VST3 AUV2)
else()
    set(TXP_FORMATS CLAP VST3)
endif()
add_subdirectory("${CLAP_WRAPPER_ROOT}" clap-wrapper EXCLUDE_FROM_ALL)

# the plugin: a static library with the CLAP entry's three functions (clap-wrapper's "clap first" layout)
add_library(textura_clap_impl STATIC "${TXP_ROOT}/plugin/src/textura_clap.c")
target_link_libraries(textura_clap_impl PUBLIC textura_engine clap)
if(UNIX AND NOT APPLE)
    target_link_libraries(textura_clap_impl PUBLIC Threads::Threads m ${CMAKE_DL_LIBS})
endif()
target_compile_definitions(textura_clap_impl PRIVATE "TXP_VERSION=\"${TXP_VERSION}\"")
target_compile_options(textura_clap_impl PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 ${TXP_C_STD} ${TXP_C_WARN} ${TXP_C_WIN}>)
target_include_directories(textura_clap_impl PRIVATE ${TXP_C_WIN_INC})
if(NOT MSVC)
    target_compile_options(textura_clap_impl PRIVATE -fvisibility=hidden)
endif()
set_target_properties(textura_clap_impl PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(TXP_BUNDLES "${CMAKE_BINARY_DIR}/bundles")
make_clapfirst_plugins(
    TARGET_NAME textura
    IMPL_TARGET textura_clap_impl
    OUTPUT_NAME "Textura"
    ENTRY_SOURCE "${TXP_ROOT}/plugin/src/clap_entry.cpp"
    BUNDLE_IDENTIFIER "io.github.leonardomirabal.textura"
    BUNDLE_VERSION "${TXP_BUNDLE_VERSION}"
    COPY_AFTER_BUILD FALSE
    PLUGIN_FORMATS ${TXP_FORMATS}
    WINDOWS_FOLDER_VST3 TRUE
    ASSET_OUTPUT_DIRECTORY "${TXP_BUNDLES}"
    AUV2_MANUFACTURER_NAME "Textura"
    AUV2_MANUFACTURER_CODE "Txtr"
    AUV2_SUBTYPE_CODE "Txt1"
    AUV2_INSTRUMENT_TYPE "aumu"
)
if(APPLE)   # (the Audio Unit's description: the CLAP's own, not the wrapper's "<name> CLAP to AU Wrapper")
    add_custom_command(TARGET textura_auv2-build-helper POST_BUILD
        COMMAND /usr/libexec/PlistBuddy -c "Set :AudioComponents:0:description Physically modelled nature sounds"
                "${CMAKE_CURRENT_BINARY_DIR}/textura_auv2-build-helper-output/auv2_Info.plist")
endif()

# ---- tests (make test-plugin): the coroutine, the headless CLAP host (tools/plugin_gate.py)
if(WIN32)
    add_executable(txp_test_coro "${TXP_ROOT}/plugin/tests/test_coro.c")
else()
    add_executable(txp_test_coro "${TXP_ROOT}/plugin/tests/test_coro.c" "${TXP_ROOT}/plugin/src/coro/coro_arm64.S"
                   "${TXP_ROOT}/plugin/src/coro/coro_x86_64.S")
endif()
target_compile_options(txp_test_coro PRIVATE -O2 ${TXP_C_WARN} ${TXP_C_WIN})
add_executable(txp_clap_render "${TXP_ROOT}/plugin/tests/clap_render.c")
target_include_directories(txp_clap_render PRIVATE "${CLAP_SDK_ROOT}/include" ${TXP_C_WIN_INC})
target_compile_options(txp_clap_render PRIVATE -O1 ${TXP_C_WARN} ${TXP_C_WIN})
target_link_libraries(txp_clap_render PRIVATE ${CMAKE_DL_LIBS})
if(UNIX AND NOT APPLE)
    target_link_libraries(txp_clap_render PRIVATE m)
endif()
add_custom_target(textura_tests ALL DEPENDS txp_test_coro txp_clap_render)
add_custom_target(textura_plugins ALL DEPENDS textura_all)
