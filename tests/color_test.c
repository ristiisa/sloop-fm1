/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the COLOR insert (firmware/src/color.c), same sources as the firmware (through hostsim.c).
 *   build/host/color_test [DEMO_DIR]          (run_tests.sh: build/color_demo)
 * 1. OFF: a song with every COLOR off never runs it (no state touched: the block is skipped).
 * 2. PHASR: the gain of steady sines across 100 Hz .. 8 kHz with the LFO held low / high: deep notches,
 *    and they move up with the LFO; in time, a 1 kHz sine swells and dips as the notches pass it.
 * 3. WAH: a sawtooth, loud then quiet: brighter (the band higher) when loud, and with more sensitivity.
 * 4. FOLD: a sine: the harmonics grow with the drive (odd ones: no even harmonics, no DC).
 * 5. RING: a 1 kHz sine, the carrier C4: the sum and difference tones at half the level, the input gone.
 * 6. bounded: loud noise and squares through every type at the extremes: no overflow (the linear ones
 *    scale exactly with the input up to 2^19), nothing past the track's headroom.
 * 7. no stuck state: after the input stops the state rings out and is cleared, the output 0 (no DC);
 *    a note through the mix: every state 0 after the release, the track idle.
 * 8. parameter locks: a step locked to RING / FOLD (synth and drum track) sounds so on that step only,
 *    p[] keeps the track's own value; the drum track shows the page.
 * 9. cost: ns per sample of each type on one track (host), and the 4-track song with COLOR on.
 * Demos (WAV) into DEMO_DIR: the pad and the drums through each type. */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <sys/mman.h>

static int check(const char *what, int ok)
{
    printf("color: %-84s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

static void set_color(track_t *t, int type, int amt, int rate)
{
    t->p[P_COLOR] = (int16_t)type;
    t->p[P_CAMT] = (int16_t)amt;
    t->p[P_CRATE] = (int16_t)rate;
}

/* n samples of in through track 0's COLOR (block by block) into out */
static void run_fx(const int32_t *in, int32_t *out, uint32_t n)
{
    uint32_t f;
    memcpy(out, in, n * sizeof *out);
    for (f = 0; f + CTL <= n; f += CTL)
        color_track(&trk[0], out + f, CTL);
}

static void sine(int32_t *b, uint32_t n, double hz, double amp)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        b[i] = (int32_t)lrint(amp * sin(2 * M_PI * hz * i / FS));
}

static double rms(const int32_t *b, uint32_t n)
{
    double s = 0;
    uint32_t i;
    for (i = 0; i < n; i++)
        s += (double)b[i] * b[i];
    return sqrt(s / n);
}

/* the amplitude of the hz component of b (Hann window) */
static double tone(const int32_t *b, uint32_t n, double hz)
{
    double re = 0, im = 0, ws = 0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * cos(2 * M_PI * i / n);
        re += w * b[i] * cos(2 * M_PI * hz * i / FS);
        im -= w * b[i] * sin(2 * M_PI * hz * i / FS);
        ws += w;
    }
    return 2 * sqrt(re * re + im * im) / ws;
}

static uint32_t lcg = 1u;
static int32_t noise(int32_t amp)
{
    lcg = lcg * 1664525u + 1013904223u;
    return (int32_t)((int64_t)((int32_t)lcg >> 8) * amp >> 23);
}

static void fx_reset(void)
{
    host_tracks_init();
    memset(col, 0, sizeof col);
}

/* ------------------------------------------------------------ the song --- */
static void song_setup(void)
{
    static const uint8_t ACID[16] = {45, 45, 57, 45, 0, 48, 45, 55, 45, 0, 57, 52, 45, 48, 0, 50};
    static const uint8_t AM[4] = {57, 60, 64, 67};
    static const uint8_t LEAD[12] = {76, 0, 0, 79, 0, 0, 81, 0, 79, 0, 76, 0};
    track_t *t1 = &trk[0], *t2 = &trk[1], *t3 = &trk[2], *td = TDRUM;
    uint32_t i;
    fx_reset();
    memset(sl, 0, sizeof sl);
    host_preset(t1, 0, 4);
    host_preset(t2, 0, 10);
    host_preset(t3, 3, 0);
    for (i = 0; i < 16u; i++) {
        uint8_t n = ACID[i];
        put_step(t1, i, n ? 1u : 0u, &n, n ? ST_NOTE : ST_REST, 0);
    }
    t2->p[P_SLEN] = 16;
    for (i = 0; i < 16u; i++)
        put_step(t2, i, i == 0u ? 4u : 0u, AM, i == 0u ? ST_NOTE : i < 14u ? ST_TIE : ST_REST, 0);
    t3->p[P_SLEN] = 12;
    for (i = 0; i < 12u; i++) {
        uint8_t n = LEAD[i];
        put_step(t3, i, n ? 1u : 0u, &n, n ? ST_NOTE : ST_REST, 0);
    }
    for (i = 0; i < 16u; i++) {
        uint8_t n[3];
        uint32_t k = 0;
        if (i % 4u == 0u)
            n[k++] = 36;
        if (i == 4u || i == 12u)
            n[k++] = 38;
        if (i % 2u == 0u)
            n[k++] = 42;
        put_step(td, i, k, n, k ? ST_NOTE : ST_REST, 0);
    }
}

/* ------------------------------------------------------------- 1. OFF --- */
static int test_off(void)
{
    static const col_t Z;
    uint32_t f, k, same = 1;
    int32_t o[2 * CTL];
    song_setup();
    transport_req = 1;
    for (f = 0; f < 3u * FS; f += CTL)
        mix_block(o, CTL);
    transport_req = 2;
    for (k = 0; k < NTRK; k++)
        same &= !memcmp(&col[k], &Z, sizeof Z);
    return check("OFF: a 3 s song with every COLOR off never runs it (no state touched)", same);
}

/* ----------------------------------------------------------- 2. PHASR --- */
/* the steady gain at hz with the LFO held at phase ph */
static double phasr_gain(double hz, uint32_t ph, int amt)
{
    static int32_t in[4096], out[4096];
    fx_reset();
    set_color(&trk[0], CO_PHASR, amt, 0);                /* (RATE 0: 0.05 Hz, still over 0.1 s) */
    col[0].type = CO_PHASR;
    col[0].ph = ph;
    sine(in, 4096, hz, 8000);
    run_fx(in, out, 4096);
    return rms(out + 2048, 2048) / rms(in + 2048, 2048);
}

static int test_phasr(void)
{
    static const uint32_t PH[2] = {0xC0000000u, 0x40000000u};   /* the LFO at its low / high end */
    double notch[2], depth[2];
    char what[200];
    int bad = 0;
    uint32_t p, k;
    for (p = 0; p < 2u; p++) {
        double best = 9, bf = 0;
        for (k = 0; k <= 72u; k++) {                      /* 100 Hz .. 8 kHz, 1/12 octave apart */
            double hz = 100.0 * pow(2.0, k / 12.0), g;
            if (hz > 8000)
                break;
            g = phasr_gain(hz, PH[p], 64);
            if (g < best)
                best = g, bf = hz;
        }
        notch[p] = bf;
        depth[p] = best;
    }
    snprintf(what, sizeof what, "PHASR: the lowest notch %.0f Hz (LFO low) -> %.0f Hz (LFO high), %.1f / %.1f dB deep",
             notch[0], notch[1], 20 * log10(depth[0]), 20 * log10(depth[1]));
    bad += check(what, notch[1] > 1.5 * notch[0] && depth[0] < 0.1 && depth[1] < 0.1);
    {
        double g0 = phasr_gain(notch[0], PH[0], 0), g1 = phasr_gain(notch[0], PH[0], 127);
        snprintf(what, sizeof what, "PHASR: AMT 0 is dry (gain %.3f), AMT 100 %% feeds back (gain at the notch %.2f)", g0, g1);
        bad += check(what, fabs(g0 - 1) < 0.002 && g1 > depth[0] && g1 < 1.5);
    }
    {   /* in time: the LFO at ~1 Hz moves the notches past a 1 kHz sine */
        static int32_t in[3u * 44100u], out[3u * 44100u];
        double mn = 1e9, mx = 0;
        uint32_t f;
        fx_reset();
        set_color(&trk[0], CO_PHASR, 64, 57);
        sine(in, 3u * FS, 1000, 8000);
        run_fx(in, out, 3u * FS);
        for (f = FS / 2u; f + 441u <= 3u * FS; f += 441u) {
            double r = rms(out + f, 441);
            mn = r < mn ? r : mn;
            mx = r > mx ? r : mx;
        }
        snprintf(what, sizeof what, "PHASR: RATE 57 (%.2f Hz): a 1 kHz sine swells and dips %.1f dB as the notches pass",
                 LFO_HZ_X100[57] / 100.0, 20 * log10(mx / mn));
        bad += check(what, mx > 4 * mn);
    }
    return bad;
}

/* ------------------------------------------------------------- 3. WAH --- */
/* brightness: the energy of the first difference over the energy (rises with the band's frequency) */
static double bright(const int32_t *b, uint32_t n)
{
    double d = 0, e = 0;
    uint32_t i;
    for (i = 1; i < n; i++) {
        d += (double)(b[i] - b[i - 1]) * (b[i] - b[i - 1]);
        e += (double)b[i] * b[i];
    }
    return d / (e + 1);
}

static void wah_run(int rate, double *loud, double *quiet, double *loud2)
{
    static int32_t in[3u * 22050u], out[3u * 22050u];
    uint32_t i, h = FS / 2u;
    fx_reset();
    set_color(&trk[0], CO_WAH, 127, rate);
    for (i = 0; i < 3u * h; i++) {                        /* a 110 Hz saw: loud, 18 dB down, loud */
        double a = i >= h && i < 2u * h ? 3000 : 24000;
        in[i] = (int32_t)(a * (2 * fmod(i * 110.0 / FS, 1.0) - 1));
    }
    run_fx(in, out, 3u * h);
    *loud = bright(out + h / 2u, h / 2u);
    *quiet = bright(out + h + h / 2u, h / 2u);
    *loud2 = bright(out + 2u * h + h / 2u, h / 2u);
}

static int test_wah(void)
{
    double l, q, l2, l0, q0, x, lh, qh;
    char what[200];
    int bad = 0;
    wah_run(40, &l, &q, &l2);
    wah_run(0, &l0, &q0, &x);
    wah_run(127, &lh, &qh, &x);
    snprintf(what, sizeof what, "WAH: brighter loud than quiet (%.3f / %.3f / %.3f: loud, quiet, loud again)", l, q, l2);
    bad += check(what, l > 3 * q && l2 > 3 * q);
    snprintf(what, sizeof what, "WAH: the sensitivity: loud %.3f (RATE 0) < %.3f (40) < %.3f (127)", l0, l, lh);
    bad += check(what, l0 < l && l < lh);
    return bad;
}

/* ------------------------------------------------------------ 4. FOLD --- */
static int test_fold(void)
{
    static const int AMT[5] = {0, 32, 64, 96, 127};
    static int32_t in[8000], out[8000];
    double thd[5], even = 0, odd = 0, dc = 0;
    char what[240];
    int bad = 0, up = 1;
    uint32_t a, k, i;
    sine(in, 8000, 220.5, 24000);                        /* a full voice; 40 whole periods */
    for (a = 0; a < 5u; a++) {
        double h = 0, f1;
        fx_reset();
        set_color(&trk[0], CO_FOLD, AMT[a], 0);
        run_fx(in, out, 8000);
        f1 = tone(out, 8000, 220.5);
        for (k = 2; k <= 40u; k++) {
            double t = tone(out, 8000, 220.5 * k);
            h += t * t;
            if (a == 4u)
                *(k & 1u ? &odd : &even) += t * t;
        }
        thd[a] = sqrt(h) / f1;
        up &= !a || thd[a] > thd[a - 1];
        if (a == 4u)
            for (i = 0; i < 8000u; i++)
                dc += out[i] / 8000.0;
    }
    snprintf(what, sizeof what, "FOLD: harmonics / fundamental %.2f %.2f %.2f %.2f %.2f (AMT 0 32 64 96 127): up with the drive",
             thd[0], thd[1], thd[2], thd[3], thd[4]);
    bad += check(what, up && thd[0] < 0.1 && thd[4] > 0.5);
    snprintf(what, sizeof what, "FOLD: odd harmonics only (even %.1f dB below), no DC (%.2f)", 10 * log10(odd / (even + 1e-9)), dc);
    bad += check(what, even < odd * 1e-4 && fabs(dc) < 2);
    {   /* RATE: an LFO moves the drive */
        static int32_t o2[3u * 44100u], i2[3u * 44100u];
        double mn = 9, mx = 0;
        uint32_t f;
        fx_reset();
        set_color(&trk[0], CO_FOLD, 64, 57);
        sine(i2, 3u * FS, 220.5, 24000);
        run_fx(i2, o2, 3u * FS);
        for (f = FS / 2u; f + 4410u <= 3u * FS; f += 4410u) {   /* the harmonics' share of the energy, every 0.1 s */
            double e1 = pow(tone(o2 + f, 4410, 220.5), 2), eh = 0, sh;
            for (k = 3; k < 20u; k += 2u)
                eh += pow(tone(o2 + f, 4410, 220.5 * k), 2);
            sh = eh / (e1 + eh);
            mn = sh < mn ? sh : mn;
            mx = sh > mx ? sh : mx;
        }
        snprintf(what, sizeof what, "FOLD: RATE 57: the drive moves (the harmonics' share of the energy %.0f .. %.0f %%)",
                 100 * mn, 100 * mx);
        bad += check(what, mx > 1.5 * mn);
    }
    return bad;
}

/* ------------------------------------------------------------ 5. RING --- */
static int test_ring(void)
{
    static int32_t in[44100], out[44100];
    double fc = 440.0 * pow(2.0, (60 - 69) / 12.0), lo, hi, mid, lo2, mid2;
    char what[200];
    int bad = 0;
    fx_reset();
    set_color(&trk[0], CO_RING, 127, 60);
    sine(in, FS, 1000, 16000);
    run_fx(in, out, FS);
    lo = tone(out, FS, 1000 - fc), hi = tone(out, FS, 1000 + fc), mid = tone(out, FS, 1000);
    snprintf(what, sizeof what, "RING: 1 kHz x C4 (%.1f Hz): %.0f Hz %.0f, %.0f Hz %.0f (8000 each), 1 kHz %.0f", fc,
             1000 - fc, lo, 1000 + fc, hi, mid);
    bad += check(what, fabs(lo - 8000) < 400 && fabs(hi - 8000) < 400 && mid < 300);
    fx_reset();
    set_color(&trk[0], CO_RING, 64, 60);
    run_fx(in, out, FS);
    lo2 = tone(out, FS, 1000 - fc), mid2 = tone(out, FS, 1000);
    snprintf(what, sizeof what, "RING: AMT 50 %%: the input %.0f, a sideband %.0f (8000, 4000)", mid2, lo2);
    bad += check(what, fabs(mid2 - 8000) < 400 && fabs(lo2 - 4000) < 300);
    return bad;
}

/* --------------------------------------------------------- 6. bounded --- */
static int test_bounded(void)
{
    static int32_t in[44100], out[44100], o2[44100];
    static const int SET[3][2] = {{127, 0}, {127, 127}, {64, 64}};
    int32_t worst = 0, pk_in = 0;
    uint32_t ty, s, i, w;
    char what[200];
    int bad = 0;
    for (w = 0; w < 3u; w++) {                            /* noise at 2^19, a square at 300000, a sine at 2^19 */
        for (i = 0; i < 44100u; i++)
            in[i] = w == 0u ? noise(524000) : w == 1u ? ((i / 50u) & 1u ? 300000 : -300000) :
                    (int32_t)(524000 * sin(2 * M_PI * 3000.0 * i / FS));
        for (i = 0; i < 44100u; i++)
            pk_in = abs(in[i]) > pk_in ? abs(in[i]) : pk_in;
        for (ty = CO_PHASR; ty <= CO_RING; ty++)
            for (s = 0; s < 3u; s++) {
                fx_reset();
                set_color(&trk[0], (int)ty, SET[s][0], SET[s][1]);
                run_fx(in, out, 44100);
                for (i = 0; i < 44100u; i++)
                    worst = abs(out[i]) > worst ? abs(out[i]) : worst;
            }
    }
    snprintf(what, sizeof what, "bounded: noise / squares / sines up to %d through every type at the extremes: |out| <= %d", pk_in,
             worst);
    bad += check(what, worst < 4 * 524288);
    for (s = 0; s < 2u; s++) {                           /* the linear ones scale exactly: no overflow inside */
        static const int L[2][3] = {{CO_PHASR, 64, 8}, {CO_RING, 127, 32}};   /* type, AMT, x */
        double err = 0, pk = 0;
        lcg = 7;
        for (i = 0; i < 44100u; i++)
            in[i] = noise(16000);
        fx_reset();
        set_color(&trk[0], L[s][0], L[s][1], 64);
        run_fx(in, out, 44100);
        for (i = 0; i < 44100u; i++)
            in[i] *= L[s][2];
        fx_reset();
        set_color(&trk[0], L[s][0], L[s][1], 64);
        run_fx(in, o2, 44100);
        for (i = 0; i < 44100u; i++) {
            err = fabs(o2[i] - (double)L[s][2] * out[i]) > err ? fabs(o2[i] - (double)L[s][2] * out[i]) : err;
            pk = fabs((double)o2[i]) > pk ? fabs((double)o2[i]) : pk;
        }
        snprintf(what, sizeof what, "bounded: %s, noise x%d (to %d) comes out x%d (largest error %.2f %% of the peak)",
                 L[s][0] == CO_PHASR ? "PHASR" : "RING", L[s][2], 16000 * L[s][2], L[s][2], 100 * err / pk);
        bad += check(what, err < 0.01 * pk);
    }
    {   /* no clipping inside the headroom: a sine at 500000 through RING keeps its level */
        double r;
        fx_reset();
        set_color(&trk[0], CO_RING, 127, 60);
        sine(in, FS, 1000, 500000);
        run_fx(in, out, FS);
        r = tone(out, FS, 1000 - 261.6256) / 250000;
        snprintf(what, sizeof what, "bounded: RING on a sine of 500000: its sidebands at %.3f of their level (not clipped)", r);
        bad += check(what, r > 0.97 && r < 1.01);
    }
    return bad;
}

/* --------------------------------------------------------- 7. release --- */
static int test_release(void)
{
    static int32_t in[44100], out[44100];
    uint32_t ty, i, f;
    int bad = 0, zero = 1, rest = 1, dcok = 1;
    double dcw = 0, dcr = 0;
    char what[200];
    for (ty = CO_PHASR; ty <= CO_RING; ty++) {           /* a burst, then silence */
        fx_reset();
        set_color(&trk[0], (int)ty, 127, 64);
        for (i = 0; i < 44100u; i++)
            in[i] = i < 8820u ? noise(60000) : 0;
        run_fx(in, out, 44100);
        for (i = 8820u + (CQUIET + 1u) * CTL; i < 44100u; i++)
            zero &= out[i] == 0;
        rest &= !col[0].z[0] && !col[0].z[1] && !col[0].z[2] && !col[0].z[3] && !col[0].z[4] && col[0].quiet == CQUIET;
    }
    bad += check("no stuck state: a burst, then silence: every type at 0 after CQUIET blocks, its state cleared",
                 zero && rest);
    for (ty = CO_PHASR; ty <= CO_RING; ty++) {           /* DC: a sine in, its mean out (Hann weighted: no tone leaks in) */
        double m, r;
        fx_reset();
        set_color(&trk[0], (int)ty, 100, 50);
        sine(in, FS, 294, 24000);
        run_fx(in, out, FS);
        m = tone(out + FS - 33000u, 33000, 0) / 2;
        r = rms(out + FS - 33000u, 33000);
        dcok &= fabs(m) < r * 1e-3;
        if (fabs(m) > dcw)
            dcw = fabs(m), dcr = r;
    }
    snprintf(what, sizeof what, "no DC: a sine through every type: its mean below 0.1 %% of its level (worst %.1f of %.0f)", dcw, dcr);
    bad += check(what, dcok);
    for (ty = CO_PHASR; ty <= CO_RING; ty++) {           /* through the mix: a chord, its release */
        int32_t o[2 * CTL], pk = 0;
        uint32_t idle = 0;
        fx_reset();
        host_preset(&trk[0], 0, 10);
        trk[0].p[P_AMODE] = 0;
        trk[0].p[P_REL] = 40;
        trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = 0;
        set_color(&trk[0], (int)ty, 127, 64);
        trk_note_on(&trk[0], 48, 110);
        trk_note_on(&trk[0], 55, 110);
        for (f = 0; f < FS / 2u; f += CTL)
            mix_block(o, CTL);
        trk_note_off(&trk[0], 48);
        trk_note_off(&trk[0], 55);
        for (f = 0; f < 6u * FS; f += CTL) {
            mix_block(o, CTL);
            if (f >= 5u * FS)
                for (i = 0; i < 2u * CTL; i++)
                    pk = abs(o[i]) > pk ? abs(o[i]) : pk;
        }
        for (i = 0; i < NVOICE; i++)
            idle += trk[0].v[i].active;
        snprintf(what, sizeof what, "no stuck state: %s on a chord, released: voices free %s, state 0 %s, the end silent (%d)",
                 N_COLOR[ty], idle ? "no" : "yes", col[0].z[0] | col[0].z[1] | col[0].z[2] | col[0].z[3] | col[0].z[4] ? "no" : "yes", pk);
        bad += check(what, !idle && !(col[0].z[0] | col[0].z[1] | col[0].z[2] | col[0].z[3] | col[0].z[4]) && pk <= 2);
    }
    return bad;
}

/* ----------------------------------------------------------- 8. locks --- */
/* the song (T1 acid alone, or the drums alone) for 2 s (a bar): per block, the step playing and the output;
 * lock: step LSTEP locked to RING (synth) / FOLD (drums) */
#define LSTEP(drums) ((drums) ? 4u : 2u)
static void lock_run(int drums, int lock, int32_t *out, uint8_t *stp, uint32_t blocks)
{
    track_t *t = drums ? TDRUM : &trk[0];
    uint32_t b, k;
    song_setup();
    for (k = 0; k < NTRK; k++)
        if (&trk[k] != t) {
            if (k == TRK_DRUM)
                song.g[G_DRLVL] = 0;
            else
                trk[k].p[P_LEVEL] = 0;
        }
    if (lock) {
        lock_set(t, LSTEP(drums), P_COLOR, drums ? CO_FOLD : CO_RING);
        lock_set(t, LSTEP(drums), P_CAMT, 127);
        lock_set(t, LSTEP(drums), P_CRATE, 90);
    }
    transport_req = 1;
    for (b = 0; b < blocks; b++) {
        stp[b] = (uint8_t)at_step(t, 0);
        mix_block(out + 2u * CTL * b, CTL);
        if (t->seq_idx != LSTEP(drums) && (t->p[P_COLOR] || t->p[P_CAMT] != 64))
            stp[b] = 0xFF;                                /* (a lock left in p[] past its step: fails below) */
    }
    transport_req = 2;
}

/* lock_run in a child: both runs start from the same state (the FX tails, the limiter of the tests before) */
static void lock_fork(int drums, int lock, int32_t *out, uint8_t *stp, uint32_t blocks)
{
    pid_t pid;
    fflush(stdout);
    if (!(pid = fork())) {
        lock_run(drums, lock, out, stp, blocks);
        _exit(0);
    }
    waitpid(pid, 0, 0);
}

static int test_locks(void)
{
    enum { NB = 2u * 44100u / CTL };
    int32_t *a = mmap(0, 2u * (2u * CTL * NB * 4u + NB), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    int32_t *b = a + 2u * CTL * NB;
    uint8_t *sa = (uint8_t *)(b + 2u * CTL * NB), *sb = sa + NB;
    int bad = 0, d;
    char what[200];
    bad += check("lockable: COLOR on the synth and the drum tracks (sound parameters)",
                 p_lockable(P_COLOR) && p_lockable(P_CAMT) && p_lockable(P_CRATE) && lock_set(TDRUM, 9, P_COLOR, 1) &&
                 lock_set(TDRUM, 9, P_CRATE, 1) && lock_set(&trk[0], 9, P_CAMT, 1));
    locks_clear(TDRUM);
    locks_clear(&trk[0]);
    for (d = 0; d < 2; d++) {
        uint32_t k, same_before = 1, n4 = 0, diff4 = 0, kept = 1;
        double e = 0, de = 0;
        lock_fork(d, 0, a, sa, NB);
        lock_fork(d, 1, b, sb, NB);
        for (k = 0; k < NB; k++) {
            uint32_t i;
            double ek = 0, dk = 0;
            kept &= sa[k] != 0xFFu && sb[k] != 0xFFu;
            for (i = 0; i < 2u * CTL; i++) {
                double x = a[2u * CTL * k + i], y = b[2u * CTL * k + i];
                ek += x * x;
                dk += (x - y) * (x - y);
            }
            if (k < NB / 2u && sb[k] < LSTEP(d))            /* before the locked step */
                same_before &= !memcmp(a + 2u * CTL * k, b + 2u * CTL * k, 2u * CTL * sizeof *a);
            if (sb[k] == LSTEP(d) && k > 0u && sb[k - 1u] == LSTEP(d)) {   /* (the block it starts in: half of each) */
                n4++;
                e += ek;
                de += dk;
                diff4 += dk > 0;
            }
        }
        snprintf(what, sizeof what, "locks, %s: step %u locked to %s: as before until it (%s), different on it (%u of %u blocks, "
                 "%.0f %% of its energy), p[] back after it", d ? "drums" : "synth", LSTEP(d) + 1u, d ? "FOLD" : "RING",
                 same_before ? "bit exact" : "NO", diff4, n4, 100 * sqrt(de / (e + 1)));
        bad += check(what, same_before && n4 > 8u && 2u * diff4 >= n4 && de > 0.1 * e && kept);
    }
    {
        uint32_t i, shown = 0;
        for (i = 0; i < NPAGES; i++)
            if (PAGES[i].id[0] == P_COLOR)
                shown = PAGES[i].fam == FAM_FX && page_for_drum(&PAGES[i]) && i > 0 && PAGES[i - 1].graph == GR_SLCR;
        bad += check("the COLOR page: in the FX family after the SLICER, on the drum track too", shown);
    }
    return bad;
}

/* ------------------------------------------------------------ 9. cost --- */
static int test_cost(void)
{
    static int32_t in[CTL], buf[CTL];
    static const char *const NM[5] = {"OFF", "PHASR", "WAH", "FOLD", "RING"};
    double ns[5], song_off, song_on;
    uint32_t ty, r, k, f;
    int32_t o[2 * CTL];
    char what[240];
    for (k = 0; k < CTL; k++)
        in[k] = noise(30000);
    for (ty = CO_OFF; ty <= CO_RING; ty++) {
        uint64_t t0;
        const uint32_t reps = 200000u;
        fx_reset();
        set_color(&trk[0], (int)ty, 100, 64);
        t0 = now_ns();
        for (r = 0; r < reps; r++) {
            memcpy(buf, in, sizeof buf);
            buf[r & (CTL - 1u)] ^= (int32_t)r;              /* (never all silent) */
            if (trk[0].p[P_COLOR])                          /* as mix_part */
                color_track(&trk[0], buf, CTL);
        }
        ns[ty] = (double)(now_ns() - t0) / ((double)reps * CTL);
    }
    for (k = 0; k < 2u; k++) {
        uint64_t t0;
        song_setup();
        if (k)
            for (r = 0; r < NTRK; r++)
                set_color(&trk[r], (int)(CO_PHASR + r % 4u), 100, 64);
        transport_req = 1;
        for (f = 0; f < FS; f += CTL)
            mix_block(o, CTL);
        t0 = now_ns();
        for (f = 0; f < 4u * FS; f += CTL)
            mix_block(o, CTL);
        *(k ? &song_on : &song_off) = (double)(now_ns() - t0) / (4.0 * FS);
        transport_req = 2;
    }
    for (ty = CO_OFF; ty <= CO_RING; ty++)
        printf("color: cost: %-5s %6.2f ns / sample (host, one track%s)\n", NM[ty], ns[ty] - (ty ? ns[0] : 0),
               ty ? "" : ": the test loop alone, mix_part skips it");
    snprintf(what, sizeof what, "cost: the song %.1f ns / sample, with PHASR WAH FOLD RING on the 4 tracks %.1f (+%.1f)",
             song_off, song_on, song_on - song_off);
    return check(what, ns[0] < 1.0);
}

/* ------------------------------------------------------------ demos --- */
static void demos(const char *dir)
{
    static const struct { const char *name; int trk, type, amt, rate; } D[] = {
        {"pad_dry.wav", 1, 0, 0, 0}, {"pad_phasr.wav", 1, CO_PHASR, 100, 50}, {"pad_ring.wav", 1, CO_RING, 90, 64},
        {"acid_wah.wav", 0, CO_WAH, 120, 50}, {"acid_fold.wav", 0, CO_FOLD, 90, 0}, {"lead_fold_lfo.wav", 2, CO_FOLD, 70, 60},
        {"drums_dry.wav", 3, 0, 0, 0}, {"drums_phasr.wav", 3, CO_PHASR, 127, 40}, {"drums_ring.wav", 3, CO_RING, 80, 70},
    };
    uint32_t d, k, f;
    for (d = 0; d < sizeof D / sizeof D[0]; d++) {
        char path[512];
        FILE *w;
        snprintf(path, sizeof path, "%s/%s", dir, D[d].name);
        if (!(w = fopen(path, "wb")))
            continue;
        song_setup();
        for (k = 0; k < NTRK; k++)
            if ((int)k != D[d].trk) {
                if (k == TRK_DRUM)
                    song.g[G_DRLVL] = 0;
                else
                    trk[k].p[P_LEVEL] = 0;
            }
        set_color(&trk[D[d].trk], D[d].type, D[d].amt, D[d].rate);
        transport_req = 1;
        wav_hdr(w, 8u * FS / CTL * CTL);
        for (f = 0; f + CTL <= 8u * FS; f += CTL) {
            int32_t o[2 * CTL];
            uint32_t i;
            mix_block(o, CTL);
            for (i = 0; i < CTL; i++)
                wav_put(w, o[2 * i], o[2 * i + 1]);
        }
        transport_req = 2;
        fclose(w);
    }
    printf("color: demos in %s (pad, acid, lead, drums: dry and through the types)\n", dir);
}

int main(int argc, char **argv)
{
    int bad = 0;
    bad += test_off();
    bad += test_phasr();
    bad += test_wah();
    bad += test_fold();
    bad += test_ring();
    bad += test_bounded();
    bad += test_release();
    bad += test_locks();
    bad += test_cost();
    if (argc > 1)
        demos(argv[1]);
    printf("%s\n", bad ? "COLOR TEST FAILED" : "color test passed");
    return bad != 0;
}
