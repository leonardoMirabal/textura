/* SPDX-License-Identifier: GPL-3.0-only */
/* The user scenes' bank (firmware/src/scn_bank.c, as scenes.c includes it on the FM-1) on the flash chip model
 * (tests/fl_host_model.h, as tests/set_ab_test.c), each flash slice gated as on the device with a USB audio stream
 * open (FL_SLICE_OK: the first 200 us of each 1 ms). A save must never hold the main loop long: its longest single
 * call (sb_start, sb_poll) at most SB_CALL_US, the whole save counted in main-loop passes (the old save erased a sector
 * in one call: ~0.2-0.3 s with the slices gated, 4 of them a save; -DSB_OLD builds that step for comparison). A power
 * cut at every call of a save (the cut operation's bytes left as they were, done, or half done), from a valid newer
 * copy and from none (the slices ungated there, the parts and passes faster: the same steps, quicker): the load takes the old scenes or the new ones (the new only once the header's program was
 * started), never neither while one was valid; the next save works. (make fw-test)
 * Build: cc -O1 -Wall -Wno-unused-function -Ifirmware/hal -o build/fw/scn_bank_test tests/scn_bank_test.c */
#define FL_HOST_MODEL "../../tests/fl_host_model.h"
#include "../firmware/src/store.h"
static int fl_align_ok(void);
#define FL_SLICE_OK() fl_align_ok()
#include "fm1_flash.h"
#include <stdlib.h>
static int gate = 1;                            /* (the cut runs: off, the slices at once; the order is the same) */
static int fl_align_ok(void) { return !gate || (T4_CNT / 24u) % 1000u < 200u; }

static int fails;
static void check(const char *what, int ok)
{
    if (!ok) { printf("FAIL  %s\n", what); fails++; }
}

/* ---- what scenes.c gives scn_bank.c ---- */
#define SCN_NU 16u
#define SCN_SLOT 1020u
#define SCN_CHUNK 255u
#define SCN_BANK_SECT 4u
#define SCN_MAGIC 0x42535854u
#define BANK_A SCENES_AT
#define BANK_B (SCENES_AT + SCN_BANK_SECT * 0x1000u)
typedef struct { uint32_t magic, seq, len, crc; } scn_hdr_t;
static int scn_rd(uint32_t off, void *dst, uint32_t n)
{
    int r;
    uint64_t keep = M.off_max;
    m_cli(); r = fl_read_ram(off, dst, n); m_sti();
    M.off_max = keep;
    return r;
}
static uint32_t scn_crc(uint32_t c, const uint8_t *p, uint32_t n)
{
    uint32_t k;
    while (n--) { c ^= *p++; for (k = 0; k < 8u; k++) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1u))); }
    return c;
}
static uint32_t ver;                            /* the scenes' version a save writes (its bytes: vb) */
static uint8_t vb(uint32_t v, uint32_t i) { return (uint8_t)(i * 131u + v * 29u + (i >> 8) * 7u + 3u); }
static void scn_chunk(uint32_t j, uint8_t *b)
{
    uint32_t i;
    for (i = 0; i < SCN_CHUNK; i++) b[i] = vb(ver, j * SCN_CHUNK + i);
}
#ifdef SB_OLD                                   /* (the save before: each step one blocking call) */
static int sb_old(uint32_t at, const uint8_t *src, uint32_t n)
{
    uint32_t took;
    return src ? fl_write(at, src, n) : fl_erase4k(at, &took);
}
#define SB_START(at, src, n) sb_old(at, src, n)
#define SB_POLL() 0
#else
#define SB_START(at, src, n) fl_op_start(at, src, n)
#define SB_POLL() fl_op_poll()
#endif
#include "../firmware/src/scn_bank.c"

#define SB_CALL_US 3500u                        /* a call at most: the first slice's wait for its moment (FL_ALIGN1_US),
                                                 * a slice, the ISRs meanwhile */
static void reboot(uint32_t erase_us, uint32_t prog_us)
{
    m_reset(0x856014u, 1, erase_us, prog_us, 400u);
    fl_can_split = -1;
    memset(&fl_aop, 0, sizeof fl_aop);
    memset(&sb, 0, sizeof sb);
}
static void put(uint32_t at, uint32_t v, uint32_t seq)   /* a copy as a whole save leaves it */
{
    scn_hdr_t h;
    uint32_t i, c = 0xFFFFFFFFu;
    memset(m_mem + at, 0xFF, SCN_BANK_SECT * 0x1000u);
    for (i = 0; i < SCN_NU * SCN_SLOT; i++) m_mem[at + sizeof h + i] = vb(v, i);
    c = scn_crc(c, m_mem + at + sizeof h, SCN_NU * SCN_SLOT);
    h.magic = SCN_MAGIC; h.seq = seq; h.len = SCN_NU * SCN_SLOT; h.crc = ~c;
    memcpy(m_mem + at, &h, sizeof h);
}
static int load_is(uint32_t *seq)               /* the version the load gives (-1 none, -2 rubbish) */
{
    uint32_t bank, i, v;
    scn_bank_pick(BANK_A, BANK_B, &bank, seq);
    if (!bank) return -1;
    v = vb(0, 0) == m_mem[bank + 16u] ? 0u : 0xFFu;
    for (v = 0; v < 16u; v++) {
        for (i = 0; i < SCN_NU * SCN_SLOT && m_mem[bank + 16u + i] == vb(v, i); i++) {}
        if (i == SCN_NU * SCN_SLOT) return (int)v;
    }
    return -2;
}
/* a save of version v into at (seq), cut after `cut` calls (0: none); the calls, the longest call (us), *hdr once the
 * header's program was started */
static uint32_t pass_us = 400u;                 /* the rest of a main-loop pass between two calls */
static uint32_t save_cut(uint32_t v, uint32_t at, uint32_t seq, uint32_t cut, int mode, int *hdr, uint32_t *call_us)
{
    uint32_t calls = 0, t;
    int rc;
    ver = v; *hdr = 0; *call_us = 0;
    t = (uint32_t)M.now;
    rc = sb_start(at, seq);
    if ((uint32_t)(M.now - t) / 24u > *call_us) *call_us = (uint32_t)(M.now - t) / 24u;
    calls++;
    while (rc == 1 && (!cut || calls < cut)) {
        m_advance(pass_us * 24u);               /* the rest of a main-loop pass */
        if (sb.step >= SCN_BANK_SECT + SB_NCH) *hdr = 1;
        t = (uint32_t)M.now;
        rc = sb_poll();
        if ((uint32_t)(M.now - t) / 24u > *call_us) *call_us = (uint32_t)(M.now - t) / 24u;
        calls++;
    }
    if (sb.step >= SCN_BANK_SECT + SB_NCH) *hdr = 1;
    if (cut && M.busy) {                        /* the power goes mid-operation */
        uint32_t i;
        if (M.op == 1) {
            for (i = 0; i < 4096u; i++)
                m_mem[M.op_addr + i] = mode == 1 ? 0xFFu : mode == 2 ? (uint8_t)(m_mem[M.op_addr + i] | rand()) : m_mem[M.op_addr + i];
        } else
            for (i = 0; i < M.op_n; i++) {
                uint8_t *b = &m_mem[(M.op_addr & ~0xFFu) + ((M.op_addr + i) & 0xFFu)];
                *b &= mode == 1 ? M.op_data[i] : mode == 2 ? (uint8_t)(M.op_data[i] | rand()) : 0xFFu;
            }
    }
    check("the save without a model violation", M.bad == 0);
    if (!cut) check("a whole save ends valid", rc == 0);
    return calls;
}

int main(void)
{
    uint32_t cut, calls, us, cuts = 0, seq;
    int st, hdr, got;
    char msg[200];
    srand(1);

    /* a whole save at the parts' real times (a 20 ms erase, a 1.5 ms program): its longest call, its passes */
    reboot(20000u, 1500u);
    put(BANK_A, 1u, 5u);
    memset(m_mem + BANK_B, 0xFF, SCN_BANK_SECT * 0x1000u);
    {
        uint64_t t0 = M.now;
        calls = save_cut(2u, BANK_B, 6u, 0, 0, &hdr, &us);
        snprintf(msg, sizeof msg, "a scene save: its longest call %u us (<= %u), %u main-loop passes, %llu ms", us,
                 SB_CALL_US, calls, (unsigned long long)((M.now - t0) / 24000u));
        printf("scn_bank: %s\n", msg);
        check(msg, us <= SB_CALL_US);
    }
    reboot(20000u, 1500u);
    check("after it: the new scenes, seq 6", load_is(&seq) == 2 && seq == 6u);

    /* a power cut at every call of a save (the parts and the passes faster, the same slices), from a newer copy in A
     * and from none */
    pass_us = 100u; gate = 0;
    for (st = 0; st < 2; st++) {
        int old = st == 0 ? 1 : -1;
        uint32_t full = 0;
        for (cut = 1;; cut++) {
            int mode = (int)(cut % 3u);
            reboot(1000u, 150u);
            if (st == 0) { put(BANK_A, 1u, 5u); put(BANK_B, 0u, 4u); }
            else { memset(m_mem + BANK_A, 0xFF, SCN_BANK_SECT * 0x1000u); memset(m_mem + BANK_B, 0xFF, SCN_BANK_SECT * 0x1000u); }
            if (cut == 1u) check("before the save: the newer copy", load_is(&seq) == old);
            calls = save_cut(2u, BANK_B, 6u, cut, mode, &hdr, &us);
            reboot(1000u, 150u);
            got = load_is(&seq);
            snprintf(msg, sizeof msg, "start %d, cut at call %u, mode %d: the old scenes (%d) or the new, got %d", st, cut,
                     mode, old, got);
            check(msg, got == old || got == 2);
            snprintf(msg, sizeof msg, "start %d, cut at call %u, mode %d: the new ones only after the header", st, cut, mode);
            check(msg, got != 2 || hdr);
            cuts++;
            if (got == 2 || got == -1) {        /* the next save: into the other copy, whole */
                uint32_t at = got == 2 ? BANK_A : BANK_B, sq = got == 2 ? 7u : 6u;
                save_cut(3u, at, sq, 0, 0, &hdr, &us);
            } else save_cut(3u, BANK_B, 6u, 0, 0, &hdr, &us);
            reboot(1000u, 150u);
            snprintf(msg, sizeof msg, "start %d, cut at call %u: the next save loads", st, cut);
            check(msg, load_is(&seq) == 3);
            if (calls < cut) { full = calls; break; }   /* (a cut past the save's end: it was whole) */
        }
        printf("scn_bank: start %d: %u calls a save\n", st, full);
    }
    printf("scn_bank: %u cuts\n", cuts);
    printf(fails ? "scn_bank: %d FAILED\n" : "scn_bank: all ok\n", fails);
    return fails != 0;
}
