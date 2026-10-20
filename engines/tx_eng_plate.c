/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #23b Thunder sheet / metal plate (D18 damping).
 *
 * A thin steel plate (aspect 2.4 : 1, a thunder sheet's 1.2 x 0.5 m) hangs free; a key strikes it (velocity = the
 * mallet's speed, the key = where: low keys near the centre, high keys toward the edge), a held key rolls it (soft
 * repeated hits, D22's "roll" that builds up until turbulent). Physics:
 *   modes    Kirchhoff plate, simply supported (standard): f_mn = (pi / 2) kappa ((m / a)^2 + (n / b)^2), kappa =
 *            h sqrt(E / (12 rho (1 - nu^2))) = 1527 h m/s (steel); modal density n = S / (2 kappa) per Hz. The lowest
 *            PARTICLES modes above 30 Hz are exact (PLATE core oscillators); each takes the strike's energy per Hz
 *            / n, times its shape at the strike point 4 sin^2(m pi x) sin^2(n pi y).
 *   bands    the denser rest (above the last exact mode) as 17 band energies, W_b = energy per Hz x bandwidth.
 *   strike   a mallet contact t_c (0.3 .. 6 ms, MALLET): energy spectrum flat up to f_c = 1 / (2 t_c), then -12 dB / oct
 *            (ours); the energy per Hz ~ v^2 x plate mobility (1 / (rho h kappa) ~ 1 / h^2: we use 1 / h, ours).
 *   damping  D18 (0.5 mm): xi omega = 8e-3 omega^0.6 per mode and band, plus a hand on the sheet (DAMP).
 *   crash    wave turbulence (D22, D24): above a chaos threshold the exact modes lose energy into their bands (D22's
 *            injection kappa from the modes) and the bands diffuse it up the spectrum, faster the more energy there
 *            is: a hard hit builds a bright crash over 0.15-0.5 s, a soft one stays a linear plate. Geometric
 *            hardening raises the low modes' pitch while the energy is high (ours: up to +3 %), so a hard hit glides
 *            down as it decays. */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_plate.h"

#define OUT_TRIM 4703               /* output gain, Q12 (+1.2 dB): max momentary (one velocity-100 press): -21.2 LUFS at LEVEL 100 -> target -20 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

enum { P_SIZE, P_THICK, P_MALLET, P_CRASH, P_DAMP, NP };

static const tx_param_t PARAMS[NP] = {
    {"SIZE", 127, 94, "plate area: 0.05 .. 2 m^2 (log; 2.4 : 1): bigger = lower, denser modes (thunder sheet 0.6)"},
    {"THICK", 127, 20, "thickness: 0.3 .. 3 mm steel (log): thin = low, dense, loud; thick = sparse, bell-like"},
    {"MALLET", 127, 64, "mallet: soft (contact 6 ms, dark) .. hard (0.3 ms, bright)"},
    {"CRASH", 127, 90, "wave turbulence: off (a linear plate) .. strong (hits cascade up into a roaring crash)"},
    {"DAMP", 127, 10, "hand on the sheet: free (+0.5 / s) .. held (+30 / s): how long it rings"},
};

#define NB TX_PLATE_BANDS
#define NBANK 16u                  /* bands on the SVF bank; band 16 is the high-pass noise */
#define E_REF 24000                 /* energy per Hz (Q30) of a full strike on the 0.5 mm plate (calibrated) */
#define THR (1u << 17)             /* chaos threshold: the modes' energy (Q30, -39 dB) */
#define W_CUT 64u                  /* a band under -72 dB stops */

typedef struct {
    tx_cloud_t cloud;
    tx_event_t ev[TX_CLOUD_EV_MAX];
    tx_pmode_t m[TX_PLATE_MODES];
    uint8_t mm[TX_PLATE_MODES], mn[TX_PLATE_MODES];   /* mode numbers */
    tx_svf_t sv[NBANK];
    int32_t f[NBANK], gin[NBANK], gb[NBANK];
    tx_noise_t hi, xlo;            /* filter coefficients and states: the top band, the crossover */
    uint32_t rng;
    int32_t gh;
    uint32_t W[NB], wdec[NB];
    int32_t nlo[TX_BLOCK], nhi[TX_BLOCK];
    uint32_t f0_q8;                /* F0 = pi kappa / (2 S), Hz Q8: f_mn = F0 (m^2 / 2.4 + 2.4 n^2) */
    uint32_t h_um, fc_hz, crash, sig_q8, fk;
    int32_t level;
    uint8_t p[NP], nm, limit, quiet, dirty;
} plate_t;

static void set_param(plate_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    if (id == P_SIZE || id == P_THICK) {
        uint32_t S_cm2 = tx_knob_exp(s->p[P_SIZE], 500u, 348778);      /* 0.05 .. 2 m^2 (5.32 oct) */
        s->h_um = tx_knob_exp(s->p[P_THICK], 300u, 217706);             /* 0.3 .. 3 mm (3.32 oct) */
        s->f0_q8 = 6141u * s->h_um / S_cm2;                  /* pi 1527 h / (2 S) = 23.99 h_um / S_cm2 Hz */
        s->dirty = 1;
    }
    if (id == P_MALLET) s->fc_hz = tx_knob_exp(v, 83u, 283660);        /* f_c 83 .. 1667 Hz (4.32 oct) */
    if (id == P_CRASH) s->crash = v ? tx_exp2((10 << 16) + (int32_t)(v * 1310720u / 127u)) : 0;   /* C 2^10 .. 2^30 */
    if (id == P_DAMP) { s->sig_q8 = tx_knob_exp(v, 128u, 387110); s->dirty = 1; }   /* 0.5 .. 30 / s */
}

/* sin(pi f / fs) x 2 in Q16, from f in Hz Q8 (series to x^5: < 3e-6 below fs / 6) */
static int32_t coef_q16(uint32_t f_q8)
{
    int64_t x = (int64_t)f_q8 * 299, x2 = (x * x) >> 30, s;  /* pi f / fs, Q30 (2^30 pi / (44100 256) = 298.8) */
    s = x - ((((x * x2) >> 30) * 178956971) >> 30) + ((((((x * x2) >> 30) * x2) >> 30) * 8947849) >> 30);
    return (int32_t)(s >> 13);
}

/* the exact modes: the lowest nm above 30 Hz (and under 7 kHz) */
static void modes(plate_t *s)
{
    uint32_t fl[TX_PLATE_MODES];
    unsigned m, n, k = 0, i, b;
    for (m = 1; m <= 24u; m++)
        for (n = 1; n <= 12u; n++) {
            uint32_t f = s->f0_q8 * (m * m * 25u + n * n * 144u) / 60u;   /* Hz Q8 */
            if (f < (30u << 8) || f > (7000u << 8)) continue;
            if (k == s->limit && f >= fl[k - 1]) continue;
            i = k < s->limit ? k++ : k - 1u;                 /* insertion: drop the highest when full */
            while (i > 0 && fl[i - 1] > f) { fl[i] = fl[i - 1]; s->mm[i] = s->mm[i - 1]; s->mn[i] = s->mn[i - 1]; i--; }
            fl[i] = f; s->mm[i] = (uint8_t)m; s->mn[i] = (uint8_t)n;
        }
    for (i = 0; i < k; i++) {
        tx_pmode_t *p = &s->m[i];
        uint32_t hz = fl[i] >> 8, best = 0;
        p->hz = (uint16_t)hz;
        p->e0 = p->e = coef_q16(fl[i]);
        p->dec = tx_plate_decay(tx_plate_law_q8(hz) + s->sig_q8);
        for (b = 1; b < NB; b++)                             /* the nearest band (log): f_b^2 <= hz^2 RHO */
            if ((uint64_t)TX_PLATE_BAND_HZ[b] * TX_PLATE_BAND_HZ[b] * 65536u <= (uint64_t)hz * hz * TX_PLATE_RHO_Q16) best = b;
        p->band = (uint8_t)best;
    }
    for (i = k; i < TX_PLATE_MODES; i++) { s->m[i].g = 0; s->m[i].gl = 0; }
    s->nm = (uint8_t)k;
    s->fk = k ? fl[k - 1] >> 8 : 30u;
    for (b = 0; b < NB; b++) {
        uint32_t r = tx_plate_law_q8(TX_PLATE_BAND_HZ[b]) + s->sig_q8;
        s->wdec[b] = tx_plate_decay(r * 2u);                 /* energy */
    }
    s->dirty = 0;
}

static void plate_init(void *st, uint32_t seed)
{
    plate_t *s = (plate_t *)st;
    uint8_t *bp = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) bp[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 12u << 8);                      /* a held key rolls: ~12 soft hits / s */
    tx_cloud_regular(&s->cloud, 96);
    tx_noise_init(&s->hi, seed ^ 0x9E3779B9u);
    tx_noise_band(&s->hi, 7300, 11, TX_NOISE_HIGH);
    /* the bank's input: low-passed noise for bands 0-7, high-passed for 8-15 (700 Hz, 12 dB / oct): a band-pass's
     * 6 dB / oct skirts alone let the loud low bands cover the top (the mallet roll-off and the crash) */
    tx_noise_init(&s->xlo, seed ^ 0x5bd1e995u);
    tx_noise_band(&s->xlo, 700, 11, TX_NOISE_LOW);
    s->rng = seed | 1u;
    for (i = 0; i < NBANK; i++) {
        s->f[i] = tx_svf_coef(TX_PLATE_BAND_HZ[i]);
        s->gin[i] = (int32_t)tx_svf_gin_noise(TX_PLATE_BAND_HZ[i], 50);
    }
    s->limit = 10;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    modes(s);
}

static void plate_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    tx_cloud_key(&((plate_t *)st)->cloud, note, vel, on);
}

static void plate_param(void *st, uint8_t id, uint8_t val)
{
    plate_t *s = (plate_t *)st;
    if (id == TX_PID_PARTICLES) {
        s->limit = (uint8_t)(val < 1u ? 1u : val > TX_PLATE_MODES ? TX_PLATE_MODES : val);
        s->dirty = 1;
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

/* the mallet's energy spectrum at f: 1 / (1 + (f / f_c)^4), Q16 */
static uint32_t mallet(uint32_t hz, uint32_t fc)
{
    uint32_t q = (hz << 12) / fc, q2, q4;                    /* Q12 */
    if (q > (64u << 12)) return 0;
    q2 = (uint32_t)(((uint64_t)q * q) >> 12);
    q4 = (uint32_t)(((uint64_t)q2 * q2) >> 12);              /* Q12 */
    if (q4 > (1u << 28)) return 0;
    return (uint32_t)((1u << 28) / ((1u << 12) + q4));
}

static void strike(plate_t *s, const tx_event_t *e)
{
    uint32_t v = e->vel, px, py = 24248u, ehz, i;           /* y at 0.37 (ours: off the nodal lines) */
    int k = (int)e->note - (int)TX_KEY_F3;
    k = k < 0 ? 0 : k > 26 ? 26 : k;
    px = 32768u - (uint32_t)k * 1059u;                       /* x 0.5 (low keys) .. 0.08 (high keys) */
    if (e->kind == TX_EV_STREAM) v = v * (90u + (tx_rand(&s->cloud.rng) >> 26)) >> 8;   /* the roll: soft */
    ehz = (E_REF / s->h_um) * (v * v * 500u / 16129u);   /* energy per Hz, Q30: ~ v^2 / h */
    for (i = 0; i < s->nm; i++) {
        tx_pmode_t *p = &s->m[i];
        int32_t sx = tx_sin_q15((uint32_t)s->mm[i] * px << 15), sy = tx_sin_q15((uint32_t)s->mn[i] * py << 15);
        int32_t psi = (sx * sy) >> 15;
        uint32_t psi2 = (uint32_t)(psi * psi) >> 15;   /* sin sin, ^2 (Q15) */
        /* E_m = E_hz |F|^2 4 psi^2 / n, 1 / n = 4 F0 / pi Hz; amplitude sqrt(2 E_m) */
        uint64_t em = ((uint64_t)ehz * mallet(p->hz, s->fc_hz)) >> 16;
        em = (em * psi2) >> 13;                              /* x 4 psi^2 */
        em = (em * s->f0_q8 * 5215u) >> 20;                  /* x 4 F0 / pi (5215 / 4096 = 4 / pi, F0 Q8) */
        em *= 2u;
        if (em > 0x7FFFFFFFu) em = 0x7FFFFFFFu;
        tx_pmode_strike(p, (int32_t)tx_isqrt32((uint32_t)em));
    }
    for (i = 0; i < NB; i++) {                               /* the bands above the exact modes */
        uint32_t fb = TX_PLATE_BAND_HZ[i], lo = fb * 55922u >> 16, hi = fb * 76802u >> 16, w;
        uint64_t wb;
        if (hi <= s->fk) continue;
        w = lo >= s->fk ? 65536u : ((hi - s->fk) << 16) / (hi - lo);   /* the share above the last exact mode */
        wb = ((uint64_t)ehz * mallet(fb, s->fc_hz)) >> 16;
        wb = (wb * fb * 20881u) >> 16;                       /* x bandwidth f_b (RHO^0.5 - RHO^-0.5) */
        wb = (wb * w) >> 16;
        wb += s->W[i];
        s->W[i] = wb > (1u << 30) ? (1u << 30) : (uint32_t)wb;
    }
}

/* one white noise (+-2^13) -> the crossover (700 Hz, 12 dB / oct: low -> bands 0-7, high -> bands 8-15, x 2^9: rms
 * 2^22 / sqrt 3 in the pass band) and the top band's high-pass (~7 kHz, gain ramped to gh, Q15, into mix) */
TX_AUDIO_CODE static void noise(plate_t *s, unsigned n, int32_t gh, int32_t *mix)
{
    uint32_t r = s->rng;
    int32_t l1 = s->xlo.low, b1 = s->xlo.band, f1 = s->xlo.f, q1 = s->xlo.q;
    int32_t l2 = s->hi.low, b2 = s->hi.band, f2 = s->hi.f, q2 = s->hi.q;
    int32_t g = s->gh, gs = (gh - s->gh) / (int32_t)(n ? n : 1u);
    unsigned j;
    for (j = 0; j < n; j++) {
        int32_t w, h1, h2;
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        w = (int32_t)r >> 18;
        l1 += (f1 * b1) >> 14;
        h1 = w - l1 - ((q1 * b1) >> 14);
        b1 += (f1 * h1) >> 14;
        l2 += (f2 * b2) >> 14;
        h2 = w - l2 - ((q2 * b2) >> 14);
        b2 += (f2 * h2) >> 14;
        s->nlo[j] = l1 * 512;
        s->nhi[j] = h1 * 512;
        mix[j] += h2 * (g >> 5);                             /* Q13 x Q10 -> Q23 */
        g += gs;
    }
    s->gh = gh;
    if (!gh) l2 = b2 = 0;
    s->rng = r;
    s->xlo.low = l1; s->xlo.band = b1; s->hi.low = l2; s->hi.band = b2;
}

TX_AUDIO_CODE static void plate_render_raw(void *st, int32_t *mix, unsigned n)
{
    plate_t *s = (plate_t *)st;
    unsigned k, i, live = 0;
    uint64_t el = 0;
    if (n > TX_BLOCK) n = TX_BLOCK;
    if (s->dirty) modes(s);
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet) return;                              /* silent and nothing new */
    for (i = 0; i < k; i++) strike(s, &s->ev[i]);
    /* the modes' energy (g^2 / 2): the chaos indicator, the coupling, the glide */
    for (i = 0; i < s->nm; i++) el += (uint32_t)(s->m[i].g * s->m[i].g) >> 1;
    if (el > 0xFFFFFFFFu) el = 0xFFFFFFFFu;
    {
        uint32_t frac = 0, d;
        if (s->crash && el > THR) {                          /* turbulence: the modes feed their bands */
            uint64_t fr = (((el - THR) >> 14) * (uint64_t)s->crash) >> 26;
            frac = fr > 8192u ? 8192u : (uint32_t)fr;        /* Q16, at most 1/8 per block */
        }
        d = (uint32_t)(el >> 14) * 5243u >> 16;              /* hardening: + 8 % x E (full scale), at most 3 % */
        if (d > 1966u) d = 1966u;
        for (i = 0; i < s->nm; i++) {
            tx_pmode_t *p = &s->m[i];
            if (!p->g) continue;
            if (frac) {
                uint32_t e = (uint32_t)(p->g * p->g) >> 1, lost = (uint32_t)(((uint64_t)e * frac) >> 16);
                uint32_t w = s->W[p->band] + lost;
                s->W[p->band] = w > (1u << 30) ? (1u << 30) : w;
                p->g = (int32_t)tx_isqrt32((e - lost) * 2u);
            }
            p->g = (int32_t)(((uint32_t)p->g * p->dec) >> 16);
            if (p->g < 8) p->g = 0;
            p->e = p->e0 + (int32_t)(((int64_t)p->e0 * (int32_t)d) >> 16);
        }
    }
    tx_plate_diffuse(s->W, NB, s->crash);
    for (i = 0; i < NB; i++) {
        uint32_t w = (uint32_t)(((uint64_t)s->W[i] * s->wdec[i]) >> 16);
        s->W[i] = w < W_CUT ? 0 : w;
    }
    tx_pmode_add(s->m, s->nm, s->level, mix, n);
    for (i = 0; i < s->nm; i++) live |= (uint32_t)(s->m[i].g | s->m[i].gl);
    {   /* the bands: gains sqrt(W) (the inputs at rms 2^22 / sqrt 3) */
        int32_t any = 0, gh = (int32_t)((tx_isqrt32(s->W[NB - 1]) * 138400u >> 16) * (uint32_t)s->level >> 15);
        for (i = 0; i < NBANK; i++) {
            uint32_t a = tx_isqrt32(s->W[i]);                /* Q15 rms */
            s->gb[i] = (int32_t)(((a * 14196u) >> 16) * (uint32_t)s->level >> 15);
            any |= s->gb[i];
        }
        if (any || gh || s->gh) {
            noise(s, n, gh, mix);
            live |= 1u;
        }
        if (any || !tx_svf_bank_quiet(s->sv, NBANK)) {
            tx_svf_bank_add_g(s->sv, s->f, 5285, s->gin, s->gb, 8, s->nlo, mix, n);
            tx_svf_bank_add_g(s->sv + 8, s->f + 8, 5285, s->gin + 8, s->gb + 8, 8, s->nhi, mix, n);
            if (!any) for (i = 0; i < NBANK; i++) s->sv[i].low = s->sv[i].band = s->sv[i].ef = 0;
            live |= 1u;
        }
    }
    for (i = 0; i < NB; i++) live |= s->W[i];
    s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !live);
}

/* the engine into its own block, then trimmed into the mix (OUT_TRIM) */
TX_AUDIO_CODE static void plate_render(void *st, int32_t *mix, unsigned n)
{
    int32_t b[TX_BLOCK];
    unsigned j;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) b[j] = 0;
    plate_render_raw(st, b, n);
    tx_trim_add(b, mix, n, OUT_TRIM);
}

static void plate_stats(const void *st, tx_stats_t *o)
{
    const plate_t *s = (const plate_t *)st;
    unsigned i, n = 0;
    for (i = 0; i < s->nm; i++) n += s->m[i].g != 0;
    o->particles = (uint16_t)n;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: every exact mode, a thin plate struck hard and rolled with eight keys, strong crash, free */
static const char SCENE_MAX[] =
    "0 particles 24\n0 param SIZE 60\n0 param THICK 0\n0 param MALLET 127\n0 param CRASH 127\n0 param DAMP 0\n"
    "0 on 53 127\n0 on 56 127\n0 on 59 127\n0 on 62 127\n0 on 65 127\n0 on 68 127\n0 on 71 127\n0 on 74 127\n"
    "3000 off 53\n3000 off 56\n3000 off 59\n3000 off 62\n3000 off 65\n3000 off 68\n3000 off 71\n3000 off 74\n"
    "4000 end\n";

const tx_engine_t tx_eng_plate = {
    "plate", "#23b Thunder sheet / plate", NP, PARAMS, TX_PLATE_MODES, 10, sizeof(plate_t), SCENE_MAX,
    plate_init, plate_key, plate_param, plate_render, plate_stats,
};
