/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Felucca memory map.
 * Code + constants execute in place from XIP (app.bin offset 0 = 0x02000120).
 * 0x01C00000..0x01C06000 holds .ram_text (code that must run while the flash
 * is busy: the flash driver).
 * Textura: the RAM-run debug gap (0x01C06000..0x01C08000), .data/.bss and the
 * pool are one region (RAM, 450 KiB): .pool follows .bss at once, so all the
 * RAM .data and .bss leave goes to the pool (the grain delay's line). crt0's
 * STATUS words at 0x01C07F00 are overwritten by .data (nothing reads them).
 * POOL : big zero-initialised buffers (FX lines, canvases), section .pool
 * .nozero: buffers that need no clearing (core 1's stack, a sector buffer),
 *   after .noinit's records in NOINIT's spare room; never cleared.
 * STACKS with 256-byte guard bands (Textura: the user stack 14 KiB, its deepest use read on the FM-1 1.3 KiB; its
 * other 10 KiB to RAM); NOINIT survives resets and UBOOT entry.
 * 0x01C7FD50.. is reserved: boot_info, boot-state mailbox, vector table. */
MEMORY
{
    XIP    (rx)  : ORIGIN = 0x02000120, LENGTH = 0x8DFBC
    RAMTEXT (rwx): ORIGIN = 0x01C00000, LENGTH = 0x6000
    RAM    (rwx) : ORIGIN = 0x01C06000, LENGTH = 0x70800
    NOINIT (rw)  : ORIGIN = 0x01C7C000, LENGTH = 0x3D50
}
SECTIONS
{
    /* the audio hot path first, at a fixed address: the FM-1 runs code from flash through a cache, and where the
     * Rhodes and effects loops (inlined into app_audio) and rh_hermite land changes their time by up to 150 us
     * (measured 2026-10-17 when the bar engine grew); pinned here, adding an engine no longer moves them */
    .text : { KEEP(*(.text.crt0)) . = ALIGN(32); KEEP(*(.text.app_audio)) *(.text.rh_hermite) *(.text*) *(.rodata*)
              . = ALIGN(4); _etext = .; } > XIP
    .ram_text : { _rt_start = .; KEEP(*(.ram_text*)) . = ALIGN(4); _rt_end = .; } > RAMTEXT AT > XIP
    _rt_load = LOADADDR(.ram_text);
    .data : { . = ALIGN(4); _data_start = .; *(.data*) *(.sdata*) . = ALIGN(4); _data_end = .; } > RAM AT > XIP
    _data_load = LOADADDR(.data);
    .bss (NOLOAD) : { _bss_start = .; *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(4); _bss_end = .; } > RAM
    .pool (NOLOAD) : { _pool_start = .; *(.pool*) . = ALIGN(4); _pool_end = .; } > RAM
    .noinit (NOLOAD) : { *(.noinit*) . = ALIGN(8); *(.nozero*) } > NOINIT
    _guard0 = 0x01C76800; _ustack_lo = 0x01C76900; _ustack_top = 0x01C7A000;
    _guard1 = 0x01C7A000; _sstack_lo = 0x01C7A100; _sstack_top = 0x01C7C000;
    /DISCARD/ : { *(.comment) *(.note*) }
}
