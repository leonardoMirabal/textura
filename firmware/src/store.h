/* SPDX-License-Identifier: GPL-3.0-only */
/* Where Dinghy keeps your data in flash, per board (platform.c: settings_*, blob_*, slot_*). Macros and compile-time
 * checks only, so the host tests include it (tests/smk_flash_test.c, tests/set_ab_test.c).
 *
 *              FM-1 (Textura)                SMK-37 (Pro, Elite, MKE-P37)
 *   settings   0xAE000, 0xAF000 (A / B)      0x9D000, 1 sector
 *   scenes     0xB0000.. bank A / B, 4 each  -
 *   blobs      0xB8000.. NBLOB sectors (<=2) 0x9E000.. NBLOB sectors
 *   slots      none (NSLOT 0)                0xC2000.. NSLOT x SLOT_SECT (USRFLASH, SLOOP's sample slots)
 *   staging    0xA9000 (V16) / 0xE0000 (V15) 0xBB000 (ota.c)
 *   owner tag  -                             0xAF000: "SMK37DATA" + DATA_FAMILY (smk_owner_check)
 *
 * Textura's FM-1 store (2026-11-16): 0xAE000..0xB9FFF, twelve sectors, its own STORE_MAGIC ("TXST"). Why there: the
 * stock tables (decoded from M-VAVE's packages) are V15 (FM-1_015) PRCT 0-0x93000, VM 0x93000-0xE9000, BTIF 0xE9000,
 * USR 0xEA000-0xFC000, and V16 (FM-1_016) PRCT 0-0xA9000, VM 0xBA000-0xDA000, SEQ64 0xDA000-0xEC000, BTIF 0xEC000;
 * key_mac 0xFF000. 0xA9000..0xB9FFF is in no region of V16's, so a unit that came from V16 keeps its stock data as it
 * was (a way back to stock finds it); Frodhes (the sister firmware, installed on the same units) stages its updates on
 * such a unit at 0xA9000..0xADFFF and keeps its own store at 0xFC000..0xFEFFF, and Textura stays clear of both, so an
 * install of one over the other loses nothing of the other's. On a unit that came from V15 the store lies inside its
 * VM (V15 has no free sector there); the stock firmware formats its VM when it is installed again. The places before
 * this one (settings 0xFC000 = Frodhes' record A, scene banks 0x97000..0x9EFFF) are refused now; no data was shipped
 * there, so there is no migration.
 *
 *   0xA9000..0xADFFF  the update staging on a V16 unit (ota.c, picked at power-on: platform.c tx_ota_pick)
 *   0xAE000           settings A  } set_store.c: a 256-byte record, a save writes the sector the load would not pick,
 *   0xAF000           settings B  } so a power cut mid-save keeps the older record
 *   0xB0000..0xB3FFF  scene bank A  } scenes.c: the 16 user scenes, the two copies written in turn (the same rule)
 *   0xB4000..0xB7FFF  scene bank B  }
 *   0xB8000..0xB9FFF  spare (NBLOB up to 2)
 *   0xE0000..0xE4FFF  the update staging on a V15 unit (its VM's end, where Felucca and Frodhes stage on V15)
 *
 * On the FM-1 the update loader may drop update records anywhere in 0x93000..0xFBFFF (ldr_core.c), but only a sector
 * whose last 256 bytes hold a record with a valid CRC (none of ours do; a settings sector's end is erased). The
 * SMK-37's store is shared by the ports under the fm1-to-smk37 kit's convention: whose data it is says the owner tag,
 * anything else is cleared once at boot (the stock settings, another port's), never parsed. Calibration (0xC1000),
 * BT pairing, the stock resources and key_mac are never in reach: FL_RANGE_OK below is the app's allow-list, and the
 * RAM flash driver refuses every protected range on its own as well (FL_GUARD, hal/fm1_flash_map.h). */
#pragma once
#include <stdint.h>

#ifndef STORE_MAGIC
#define STORE_MAGIC 0x54535854u                 /* "TXST" (Textura; Dinghy's "DNGY"): settings and blobs (slots: "SLOT") */
#endif
#ifndef NBLOB                                   /* (Frodhes: the app may set fewer, frodhes_store.h; NSLOT 0: none) */
#define NBLOB 0u                                /* blobs: one 4 KiB sector each (Textura: none) */
#endif
#ifndef NSLOT
#define NSLOT 0u                                /* slots: SLOT_SECT sectors each, for something bigger (samples, songs;
                                                 * Textura: none, the FM-1 has no room for them) */
#endif
#define SLOT_SECT 4u

#if BOARD_SMK37
#define SETTINGS_AT 0x0009D000u
#define BLOBS_AT 0x0009E000u
#define OWNER_AT 0x000AF000u                    /* (the kit's place for it) */
#define SLOTS_AT 0x000C2000u
#ifndef DATA_FAMILY
#define DATA_FAMILY "DINGHY"                    /* the owner tag's family: 1..7 of A-Z 0-9 (tools/build.py DATA_FAMILY) */
#endif
#define FL_RANGE_OK(off, n) (FL_IN(off, n, FL_OTA_LO, FL_OTA_HI) || FL_IN(off, n, SETTINGS_AT, SETTINGS_AT + 0x1000u) || \
                             FL_IN(off, n, BLOBS_AT, BLOBS_AT + NBLOB * 0x1000u) || FL_IN(off, n, OWNER_AT, OWNER_AT + 0x1000u) || \
                             FL_IN(off, n, SLOTS_AT, SLOTS_AT + NSLOT * SLOT_SECT * 0x1000u))
#else
#define SETTINGS_AT 0x000AE000u                 /* the settings record A, B (set_store.c) */
#define SETTINGS_B 0x000AF000u
#define SCENES_AT 0x000B0000u                   /* the scene banks A, B (scenes.c), SCENES_SECT sectors in all */
#define SCENES_SECT 8u
#define BLOBS_AT 0x000B8000u                    /* (spare: NBLOB up to 2) */
#define SLOTS_AT 0x000BA000u                    /* (none: NSLOT 0) */
#define STORE_HI 0x000BA000u                    /* the store's end: V16's VM starts here */
#define TX_OTA_V16 0x000A9000u                  /* the update staging on a unit that came from V16 (in no region of
                                                 * V16's; Frodhes' too) */
#define TX_OTA_V15 0x000E0000u                  /* ... or from V15 (its VM's end: Felucca's, Frodhes') */
#define OTA_AREA tx_ota_area                    /* (ota.c; platform.c: picked at power-on) */
#define FL_RANGE_OK(off, n) (FL_IN(off, n, SETTINGS_AT, SCENES_AT + SCENES_SECT * 0x1000u) || \
                             FL_IN(off, n, BLOBS_AT, BLOBS_AT + NBLOB * 0x1000u) || \
                             FL_IN(off, n, TX_OTA_V16, TX_OTA_V16 + 0x5000u) || FL_IN(off, n, TX_OTA_V15, TX_OTA_V15 + 0x5000u))
#endif
#ifdef APP_FL_RANGE
/* Frodhes (a platform delta, README): the app's own, tighter allow-list in place of the one above: the update staging,
 * the settings sector and APP_FL_RANGE (frodhes_store.h: the preset bank A / B, the pattern A / B; on the SMK-37 also
 * the owner tag) */
#undef FL_RANGE_OK
#define FL_RANGE_OK(off, n) (FL_IN(off, n, FL_OTA_LO, FL_OTA_HI) || FL_IN(off, n, SETTINGS_AT, SETTINGS_AT + 0x1000u) || \
                             APP_FL_RANGE(off, n))
#endif
#include "../hal/fm1_flash_map.h"           /* (Frodhes: relative, as midi_uart.c's fm1_uart.h: the host builds have no -Ifirmware/hal) */
#define SECTOR_MAX (4096u - 12u)                /* settings, a blob: the sector minus the header */
#define SLOT_MAX (SLOT_SECT * 0x1000u - 12u)

#if BOARD_SMK37
_Static_assert(SETTINGS_AT >= FL_DATA_LO && SETTINGS_AT + 0x1000u <= BLOBS_AT, "SMK store: settings outside FL_DATA");
_Static_assert(BLOBS_AT + NBLOB * 0x1000u <= OWNER_AT && OWNER_AT + 0x1000u <= FL_DATA_HI, "SMK store: blobs / owner tag");
#if NSLOT
_Static_assert(SLOTS_AT >= FL_SMP_LO && SLOTS_AT + NSLOT * SLOT_SECT * 0x1000u <= FL_SMP_HI, "SMK store: slots outside FL_SMP");
_Static_assert(FL_WIN_SAFE(SLOTS_AT, SLOTS_AT + NSLOT * SLOT_SECT * 0x1000u), "SMK store: the slots overlap a protected range");
#endif
_Static_assert(FL_WIN_SAFE(SETTINGS_AT, OWNER_AT + 0x1000u), "SMK store: overlaps a protected range");
_Static_assert(sizeof DATA_FAMILY - 1u >= 1u && sizeof DATA_FAMILY - 1u <= 7u, "SMK store: family 1..7 characters");

/* the 16-byte owner tag: "SMK37DATA" + the family, zero padded (a 7-letter family fills it, no NUL) */
static void smk_owner_tag(uint8_t tag[16])
{
    static const char FAM[] = DATA_FAMILY;
    uint32_t i;
    for (i = 0; i < 16u; i++)
        tag[i] = i < 9u ? (uint8_t)"SMK37DATA"[i] : i - 9u < sizeof FAM - 1u ? (uint8_t)FAM[i - 9u] : 0u;
}
#else
/* the map above: clear of every V16 region, of Frodhes' staging and store, of the old places; nothing next to it */
#define TX_SECTOR_OK(at) ((at) % 0x1000u == 0u && FL_GUARD(at, 0x1000u))
_Static_assert(SETTINGS_B == SETTINGS_AT + 0x1000u && SCENES_AT == SETTINGS_B + 0x1000u &&
               SCENES_AT + SCENES_SECT * 0x1000u <= BLOBS_AT && BLOBS_AT + NBLOB * 0x1000u <= STORE_HI && NSLOT == 0u,
               "FM-1 store: settings A / B, the scene banks, the spare, in this order");
_Static_assert(SETTINGS_AT >= TX_OTA_V16 + 0x5000u && STORE_HI <= 0xBA000u,
               "FM-1 store: in V16's gap 0xA9000..0xB9FFF, after Frodhes' V16 staging 0xA9000..0xADFFF, before V16's VM");
_Static_assert(SETTINGS_AT >= 0xA9000u && STORE_HI <= TX_OTA_V15 && STORE_HI <= 0xFC000u,
               "FM-1 store: after V16's PRCT, before the V15 staging 0xE0000 and Frodhes' store 0xFC000..0xFEFFF");
_Static_assert(TX_SECTOR_OK(SETTINGS_AT) && TX_SECTOR_OK(SETTINGS_B) && TX_SECTOR_OK(SCENES_AT) &&
               TX_SECTOR_OK(SCENES_AT + (SCENES_SECT - 1u) * 0x1000u) && TX_SECTOR_OK(TX_OTA_V16) &&
               TX_SECTOR_OK(TX_OTA_V16 + 0x4000u) && TX_SECTOR_OK(TX_OTA_V15) && TX_SECTOR_OK(TX_OTA_V15 + 0x4000u),
               "FM-1 store: every sector it writes in the allow-list");
_Static_assert(!FL_GUARD(TX_OTA_V16 - 0x100u, 0x100u) && !FL_GUARD(STORE_HI, 0x100u) &&
               !FL_GUARD(SCENES_AT + SCENES_SECT * 0x1000u + NBLOB * 0x1000u, 0x100u) &&
               !FL_GUARD(TX_OTA_V15 - 0x100u, 0x100u) && !FL_GUARD(TX_OTA_V15 + 0x5000u, 0x100u) &&
               !FL_GUARD(0xFC000u, 0x1000u) && !FL_GUARD(0xFD000u, 0x1000u) && !FL_GUARD(0xFE000u, 0x1000u) &&
               !FL_GUARD(0x97000u, 0x1000u) && !FL_GUARD(0x9E000u, 0x1000u) && !FL_GUARD(0xBA000u, 0x1000u) &&
               !FL_GUARD(0xDA000u, 0x1000u) && !FL_GUARD(0xEC000u, 0x1000u) && !FL_GUARD(0xFF000u, 0x100u),
               "FM-1 store: nothing next to it, nor Frodhes' store, the old places, V16's VM / SEQ64 / BTIF, key_mac");
#endif
