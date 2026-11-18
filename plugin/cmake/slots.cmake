# SPDX-License-Identifier: GPL-3.0-only
# The engine: plugin/src/engine_unit.c compiled TXP_SLOTS (16) times, -DTXP_SLOT=0..15, each into its own object
# (the app's statics are one set per object: 16 independent apps in one binary), plus the instance layer, the
# coroutine and the sound engines (engines/*.c, once: they keep no mutable globals, every slot shares them). Every
# slot is the HQ build (firmware/src/lanes.c LN_HQ: the CPU governor's pool has no end, the grain delay plays all its
# grains). Before the archive is written, plugin/cmake/nm_check.cmake fails the build if any slot object defines a
# global symbol other than its table txp_slot_<N> (two copies of the app must never share a symbol).
set(TXP_SLOTS 16)
# the app's build switches (tools/plugin_gate.py builds the host render with the same LN_HQ)
set(TXP_ENGINE_DEFS LN_HQ=1 "APP_VER=\"${TXP_VERSION}\"")
if(MSVC)
    set(TXP_ENGINE_FLAGS -O2 ${TXP_C_STD} /W3 -Wno-unused-function -Wno-unused-variable ${TXP_C_WIN})
else()
    set(TXP_ENGINE_FLAGS -O2 -std=gnu11 -ffp-contract=off -fvisibility=hidden -Wall -Wno-unused-function
                         -Wno-unused-variable)
endif()
option(TXP_LTO "full LTO on the slot objects (merges their identical constant tables)" ON)
if(TXP_LTO)
    list(APPEND TXP_ENGINE_FLAGS -flto=full)
endif()
set(_txp_objs "")
set(_txp_slot_targets "")
math(EXPR _last "${TXP_SLOTS} - 1")
foreach(n RANGE 0 ${_last})
    add_library(txp_slot_${n} OBJECT "${TXP_ROOT}/plugin/src/engine_unit.c")
    target_compile_definitions(txp_slot_${n} PRIVATE TXP_SLOT=${n} ${TXP_ENGINE_DEFS})
    target_include_directories(txp_slot_${n} PRIVATE "${TXP_ROOT}/plugin/compat" "${TXP_ROOT}/firmware/src"
                               "${TXP_ROOT}/firmware/gen" "${TXP_ROOT}/engines" "${TXP_ROOT}/web" ${TXP_C_WIN_INC})
    target_compile_options(txp_slot_${n} PRIVATE ${TXP_ENGINE_FLAGS})
    set_target_properties(txp_slot_${n} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    list(APPEND _txp_objs "$<TARGET_OBJECTS:txp_slot_${n}>")
    list(APPEND _txp_slot_targets txp_slot_${n})
endforeach()

if(WIN32)
    set(_txp_coro_src "")          # (Fibers: plugin/src/coro/coro.h)
else()
    set(_txp_coro_src "${TXP_ROOT}/plugin/src/coro/coro_arm64.S" "${TXP_ROOT}/plugin/src/coro/coro_x86_64.S")
endif()
file(GLOB _txp_eng_src "${TXP_ROOT}/engines/*.c")
add_library(textura_engine STATIC
    "${TXP_ROOT}/plugin/src/bridge.c"
    ${_txp_coro_src}
    ${_txp_eng_src}
    ${_txp_objs})
target_include_directories(textura_engine PUBLIC "${TXP_ROOT}/plugin/src"
                           PRIVATE "${TXP_ROOT}/engines" ${TXP_C_WIN_INC})
target_compile_options(textura_engine PRIVATE $<$<COMPILE_LANGUAGE:C>:${TXP_ENGINE_FLAGS}>)
set_target_properties(textura_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_dependencies(textura_engine ${_txp_slot_targets})
# (the object list goes in a file, one path a line: Windows' Ninja nests a PRE_LINK command's quotes otherwise)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/txp_slot_objs.txt" CONTENT "$<JOIN:${_txp_objs},\n>\n")
add_custom_command(TARGET textura_engine PRE_LINK
    COMMAND ${CMAKE_COMMAND} "-DOBJS_FILE=${CMAKE_BINARY_DIR}/txp_slot_objs.txt" -DNM=${CMAKE_NM} -DWEAK_CRT_OK=$<BOOL:${WIN32}> -P "${TXP_ROOT}/plugin/cmake/nm_check.cmake"
    COMMENT "plugin: the slot objects export nothing but their tables (nm)"
    VERBATIM)
