/* SPDX-License-Identifier: GPL-3.0-only */
/* The user scenes' bank in flash (scenes.c includes it; tests/scn_bank_test.c runs it on the flash chip model): two
 * copies of SCN_BANK_SECT sectors, each [scn_hdr_t: magic, seq, len, crc][SCN_NU records of SCN_SLOT bytes]; the load
 * takes the valid copy (magic, length, CRC) with the newer seq. A save rewrites the older copy: its sectors erased,
 * the records written a chunk at a time (scn_chunk: the saved scene from the caller, the others as the newer copy
 * holds them), the header last, so a power cut mid-save leaves the newer copy as it was.
 * The save never holds the main loop: each step starts one flash operation (a sector erase, a page program:
 * SB_START, fm1_flash.h fl_op_start on the FM-1) and the next passes poll it (SB_POLL: fl_op_poll, one slice at a
 * time), as set_store.c's settings save (a whole-erase call held the main loop ~0.2-0.3 s with the slices gated on the
 * USB audio, 4 of them a save). The includer gives scn_rd, scn_crc, scn_chunk(j, b) (chunk j's SCN_CHUNK bytes),
 * scn_hdr_t, SCN_MAGIC, SCN_NU, SCN_SLOT, SCN_CHUNK, SCN_BANK_SECT, SB_START(at, src, n) (src 0: erase the sector at
 * at; else program n bytes inside one page; 1 running, 0 done, < 0 failed) and SB_POLL() (1 running, 0 done, < 0
 * failed). */
static int scn_bank_valid(uint32_t at, scn_hdr_t *h)
{
    uint8_t b[SCN_CHUNK];
    uint32_t c = 0xFFFFFFFFu, o;
    if (scn_rd(at, h, sizeof *h) || h->magic != SCN_MAGIC || h->len != SCN_NU * SCN_SLOT) return 0;
    for (o = 0; o < h->len; o += SCN_CHUNK) {
        if (scn_rd(at + sizeof *h + o, b, SCN_CHUNK)) return 0;
        c = scn_crc(c, b, SCN_CHUNK);
    }
    return ~c == h->crc;
}
/* the copy the load takes (a, b: the two copies' places): *bank 0 and *seq 0 when neither is valid */
static void scn_bank_pick(uint32_t a, uint32_t b, uint32_t *bank, uint32_t *seq)
{
    scn_hdr_t ha, hb;
    int va = scn_bank_valid(a, &ha), vb = scn_bank_valid(b, &hb);
    *bank = 0; *seq = 0;
    if (va && (!vb || (int32_t)(ha.seq - hb.seq) > 0)) { *bank = a; *seq = ha.seq; }
    else if (vb) { *bank = b; *seq = hb.seq; }
}

#define SB_NCH (SCN_NU * (SCN_SLOT / SCN_CHUNK))
static struct {
    uint32_t at, seq, crc, pat;                  /* the copy written, its seq, the running CRC, a program's rest: place */
    const uint8_t *pp;                           /* (its bytes) */
    uint16_t step, pn;                           /* the step (sectors, chunks, the header), the rest's length */
    uint8_t on, b[SCN_CHUNK];                    /* (the chunk being programmed: kept until its program ends) */
    scn_hdr_t h;
} sb;
static int sb_prog(uint32_t at, const uint8_t *p, uint32_t n)   /* the part up to the page's end now, the rest after */
{
    uint32_t k = 256u - (at & 0xFFu);
    if (k > n) k = n;
    sb.pat = at + k; sb.pp = p + k; sb.pn = (uint16_t)(n - k);
    return SB_START(at, p, k);
}
static int sb_issue(void)                        /* the step's operation */
{
    if (sb.step < SCN_BANK_SECT) return SB_START(sb.at + sb.step * 0x1000u, 0, 0);
    if (sb.step < SCN_BANK_SECT + SB_NCH) {
        uint32_t j = sb.step - SCN_BANK_SECT;
        scn_chunk(j, sb.b);
        sb.crc = scn_crc(sb.crc, sb.b, SCN_CHUNK);
        return sb_prog(sb.at + sizeof(scn_hdr_t) + j * SCN_CHUNK, sb.b, SCN_CHUNK);
    }
    sb.h.magic = SCN_MAGIC; sb.h.seq = sb.seq; sb.h.len = SCN_NU * SCN_SLOT; sb.h.crc = ~sb.crc;
    return sb_prog(sb.at, (const uint8_t *)&sb.h, sizeof sb.h);
}
/* a save into the copy at at with seq: 1 started (sb_poll each pass), < 0 failed */
static int sb_start(uint32_t at, uint32_t seq)
{
    int rc;
    sb.at = at; sb.seq = seq; sb.crc = 0xFFFFFFFFu; sb.step = 0; sb.pn = 0; sb.on = 1;
    if ((rc = sb_issue()) < 0) sb.on = 0;
    return rc < 0 ? rc : 1;
}
/* once a main-loop pass: at most one slice or one new operation. 1 running, 0 done (the header written: the caller
 * checks the copy), < 0 failed (the copy stays invalid; the other one holds the scenes) */
static int sb_poll(void)
{
    int rc;
    if (!sb.on) return 0;
    if ((rc = SB_POLL()) == 1) return 1;
    if (rc >= 0 && sb.pn) rc = SB_START(sb.pat, sb.pp, sb.pn), sb.pn = 0;   /* (a program's rest past the page) */
    else if (rc >= 0) {
        if (sb.step >= SCN_BANK_SECT + SB_NCH) { sb.on = 0; return 0; }
        sb.step++;
        rc = sb_issue();
    }
    if (rc < 0) { sb.on = 0; return rc; }
    return 1;
}
