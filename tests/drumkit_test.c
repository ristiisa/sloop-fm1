/* SPDX-License-Identifier: GPL-3.0-only */
/* Synthesised drum kits (drum_synth.c): every kit x every sound is bounded, audible and
 * ends; the level of each kit stays near the sampled kit; the host cost of 6 synth voices.
 * The Peaks models (drum_peaks.c): each answers its pitch, decay and own knobs; one voice of them costs
 * no more than the costliest other synthesised voice.
 * argv[1]: a WAV demo (each kit plays one bar), argv[2]: a per-kit report. */
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <assert.h>
static const uint8_t DS_NOTE[DS_LANES] = {36, 38, 39, 42, 46, 43, 48, 49, 51, 70, 63, 37, 56, 75, 35, 40};   /* one GM note per synth lane */
static uint32_t one_hit(uint32_t kit, uint32_t note, int32_t *peak, uint64_t *energy)
{
    uint32_t j, k, blocks = 0;
    int32_t l[CTL], r[CTL], rv[CTL];
    memset(&drums, 0, sizeof drums);
    drums.set = -2;
    TDRUM->p[P_E0] = (int16_t)kit;
    song.g[G_DRLVL] = 100;
    drum_on(note, 110);
    *peak = 0;
    *energy = 0;
    for (j = 0; j < FS * 6u / CTL; j++) {
        memset(l, 0, sizeof l), memset(r, 0, sizeof r), memset(rv, 0, sizeof rv);
        drums_render(l, r, rv, CTL);
        for (k = 0; k < CTL; k++) {
            int32_t a = l[k] < 0 ? -l[k] : l[k];
            assert(a < 131072);
            if (a > *peak) *peak = a;
            *energy += (uint64_t)a;
        }
        blocks++;
        for (k = 0; k < NDRUM && !drums.v[k].active; k++)
            ;
        if (k == NDRUM)
            break;
    }
    return blocks;
}

/* one sound alone (as lane 0, note 36, velocity 110), mono into buf until it ends; returns its samples */
static dkit_t tkit;
static uint32_t render_snd(const dsnd_t *d, int32_t *buf, uint32_t max)
{
    dsv_t v;
    uint32_t n = 0;
    int alive;
    memset(&tkit, 0, sizeof tkit);
    tkit.s[0] = *d;
    ds_on(&v, &tkit, 36, 110);
    do {
        alive = ds_render(&v, buf + n, CTL);
        n += CTL;
    } while (alive && n + CTL <= max);
    return n;
}
static uint64_t hash_buf(const int32_t *b, uint32_t n)
{
    uint64_t h = 1469598103934665603ull;
    while (n--)
        h = (h ^ (uint32_t)*b++) * 1099511628211ull;
    return h;
}
static uint32_t zero_x(const int32_t *b, uint32_t n)       /* sign changes */
{
    uint32_t i, z = 0;
    for (i = 1; i < n; i++)
        z += (b[i - 1] < 0) != (b[i] < 0);
    return z;
}
static uint64_t tail_energy(const int32_t *b, uint32_t n, uint32_t from)
{
    uint64_t e = 0;
    for (; from < n; from++)
        e += (uint64_t)(b[from] < 0 ? -b[from] : b[from]);
    return e;
}

/* every Peaks model answers its parameters: pitch up an octave (more zero crossings in the first 100 ms;
 * the hi-hat: another sound), a longer decay (it lasts longer, more after 50 ms), and its own knobs */
static int32_t pbuf[2][FS * 6];
static void peaks_params(FILE *rep)
{
    static const char *const MN[4] = {"BD", "SD", "HH", "FM"};
    uint32_t m, kit, lane;
    for (m = DW_PK_BD; m <= DW_PK_FM; m++) {
        const dsnd_t *d0 = 0;
        dsnd_t d;
        uint32_t n0, n1, z0, z1, i;
        uint64_t h0, e0, e1;
        for (kit = 0; kit < DS_NKITS && !d0; kit++)
            for (lane = 0; lane < DS_LANES && !d0; lane++)
                if (DS_KITS[kit].s[lane].wave == m && DS_KITS[kit].s[lane].decay < 200u)
                    d0 = &DS_KITS[kit].s[lane];
        assert(d0);
        n0 = render_snd(d0, pbuf[0], FS * 6u);
        h0 = hash_buf(pbuf[0], n0);
        z0 = zero_x(pbuf[0], FS / 10u);
        e0 = tail_energy(pbuf[0], n0, FS / 20u);
        d = *d0;
        d.pitch += 12;
        n1 = render_snd(&d, pbuf[1], FS * 6u);
        z1 = zero_x(pbuf[1], FS / 10u);
        fprintf(rep, "peaks %s: pitch +12: zero crossings %u -> %u;", MN[m - DW_PK_BD], z0, z1);
        fflush(rep);
        assert(hash_buf(pbuf[1], n1) != h0);
        if (m != DW_PK_HH)
            assert(z1 > z0 + z0 / 4u);
        d = *d0;
        d.decay = (uint8_t)(m >= DW_PK_HH && d.decay > 87u ? 127u : d.decay + 40u);   /* (HH, FM: a DECAY_K index) */
        n1 = render_snd(&d, pbuf[1], FS * 6u);
        e1 = tail_energy(pbuf[1], n1, FS / 20u);
        fprintf(rep, " decay up: %u -> %u ms, tail %llu -> %llu;", n0 * 1000u / FS, n1 * 1000u / FS,
                (unsigned long long)e0, (unsigned long long)e1);
        fflush(rep);
        assert(n1 > n0 && e1 > e0);
        for (i = 0; i < 2u; i++) {                      /* its other knobs: punch, tone; tone, snappy; band, colour; fm, noise */
            static const uint8_t F[4][2] = {{4, 16}, {16, 12}, {16, 18}, {4, 12}};   /* (dsnd_t byte offsets) */
            uint8_t *f;
            d = *d0;
            f = (uint8_t *)&d + F[m - DW_PK_BD][i];
            *f = (uint8_t)(*f >= 60u ? *f - 60u : *f + 60u);
            n1 = render_snd(&d, pbuf[1], FS * 6u);
            fprintf(rep, " field %u: %s", F[m - DW_PK_BD][i], hash_buf(pbuf[1], n1) != h0 ? "changes" : "SAME");
            fflush(rep);
            assert(hash_buf(pbuf[1], n1) != h0);
        }
        fprintf(rep, "\n");
    }
}

/* the host cost of one voice of a sound: ns per sample it sounds (the first 64 blocks of a hit, 30 hits, the
 * best of reps); a Peaks voice must cost no more than the costliest of the others: the costliest of each
 * measured again, in turns */
static double voice_ns(uint32_t kit, uint32_t lane, uint32_t reps)
{
    static int32_t sink;
    double best = 1e9;
    uint32_t r, hit;
    for (r = 0; r < reps; r++) {
        struct timespec t0, t1;
        uint64_t blocks = 0;
        int32_t buf[CTL];
        dsv_t v;
        double ns;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (hit = 0; hit < 30u; hit++) {
            uint32_t b = 0;
            ds_on(&v, &DS_KITS[kit], DS_NOTE[lane], 110);
            while (b < 64u && ds_render(&v, buf, CTL))
                b++;
            blocks += b + 1u;
            sink += buf[3];
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ns = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (double)(blocks * CTL);
        if (ns < best)
            best = ns;
    }
    return best;
}
static void voice_cost(FILE *rep)
{
    static const char *const LN[DS_LANES] = {"KICK", "SNARE", "CLAP", "CHH", "OHH", "TOMLO", "TOMHI", "CRASH", "RIDE",
                                              "SHAKER", "CONGA", "RIM", "COWBELL", "CLAVE", "KICK2", "SNARE2"};
    double top[2][4] = {{0}}, pkm[4] = {0}, worst[2] = {0, 0};
    uint32_t kit, lane, r, i, tk[2][4] = {{0}}, tl[2][4] = {{0}}, wi[2] = {0, 0};
    for (kit = 0; kit < DS_NKITS; kit++)                /* the four costliest of each */
        for (lane = 0; lane < DS_LANES; lane++) {
            const dsnd_t *d = &DS_KITS[kit].s[lane];
            uint32_t pk = d->wave >= DW_PK_BD, m = 0;
            double ns = voice_ns(kit, lane, 5);
            for (i = 1; i < 4u; i++)
                if (top[pk][i] < top[pk][m]) m = i;
            if (ns > top[pk][m]) {
                top[pk][m] = ns;
                tk[pk][m] = kit;
                tl[pk][m] = lane;
            }
            if (pk && ns > pkm[d->wave - DW_PK_BD])
                pkm[d->wave - DW_PK_BD] = ns;
        }
    fprintf(rep, "cost of one voice (host, ns / sample): Peaks models BD %.2f SD %.2f HH %.2f FM %.2f\n", pkm[0], pkm[1],
            pkm[2], pkm[3]);
    for (i = 0; i < 8u; i++)
        top[i / 4u][i % 4u] = 1e9;
    for (r = 0; r < 5u; r++)                            /* measured again, in turns: the best of 15 */
        for (i = 0; i < 8u; i++) {
            double ns = voice_ns(tk[i / 4u][i % 4u], tl[i / 4u][i % 4u], 3);
            if (ns < top[i / 4u][i % 4u]) top[i / 4u][i % 4u] = ns;
        }
    for (i = 0; i < 8u; i++)
        if (top[i / 4u][i % 4u] > worst[i / 4u]) {
            worst[i / 4u] = top[i / 4u][i % 4u];
            wi[i / 4u] = i % 4u;
        }
    fprintf(rep, "cost of one voice: the costliest of the others %.2f ns (%s %s), of the Peaks models %.2f ns (%s %s)\n",
            worst[0], DS_KITS[tk[0][wi[0]]].name, LN[tl[0][wi[0]]], worst[1], DS_KITS[tk[1][wi[1]]].name, LN[tl[1][wi[1]]]);
    fflush(rep);
    assert(worst[1] <= worst[0]);
}

int main(int argc, char **argv)
{
    uint32_t kit, lane, j, k;
    int32_t peak, ref_peak = 0;
    uint64_t e;
    FILE *rep = argc > 2 ? fopen(argv[2], "w") : stdout;
    host_tracks_init();
    one_hit(0, 36, &ref_peak, &e);                       /* the sampled kick */
    for (kit = DRUM_SAMPLED; kit < DRUM_KITS; kit++) {
        int32_t kpk = 0;
        if (!kit_synth(kit))
            continue;                                     /* (the user kits: tests/userkit_test.c) */
        fprintf(rep, "%-8s %-12s", DRUM_KIT_NAMES[kit], DRUM_KIT_STYLES[kit]);
        for (lane = 0; lane < DS_LANES; lane++) {
            uint32_t b = one_hit(kit, DS_NOTE[lane], &peak, &e);
            fprintf(rep, " %5d/%4ums", peak, b * CTL * 1000u / FS);
            fflush(rep);
            assert(b < FS * 6u / CTL);                    /* every sound ends */
            assert(peak > 600);                           /* and is heard */
            if (peak > kpk) kpk = peak;
        }
        fprintf(rep, "\n");
        assert(kpk < ref_peak * 3 && kpk > ref_peak / 4);  /* near the sampled kit */
    }
    peaks_params(rep);
    voice_cost(rep);
    {   /* cost: 6 synth voices (open hat, crash, ride, kick, snare, clap) vs 6 sampled ones */
        int32_t l[CTL], r[CTL], rv[CTL];
        struct timespec t0, t1;
        double ns_s, ns_y;
        for (k = 0; k < 2u; k++) {
            uint32_t rep_n = 2000;
            memset(&drums, 0, sizeof drums);
            drums.set = -2;
            TDRUM->p[P_E0] = (int16_t)(k ? DRUM_SAMPLED + 1u : 0u);
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (j = 0; j < rep_n; j++) {
                if (j % 100u == 0u) {
                    static const uint8_t N[6] = {46, 49, 51, 36, 38, 39};
                    uint32_t q;
                    for (q = 0; q < 6u; q++) drum_on(N[q], 100);
                }
                drums_render(l, r, rv, CTL);
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            *(k ? &ns_y : &ns_s) = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (rep_n * (double)CTL);
        }
        fprintf(rep, "cost: 6 voices sampled %.1f ns/sample, synthesised %.1f ns/sample (host)\n", ns_s, ns_y);
    }
    if (argc > 1) {   /* demo: each kit, one bar: K . H . S . H . K K H . S . H O */
        static const uint8_t P[16][3] = {{36, 42, 0}, {0}, {42, 0}, {0}, {38, 42, 0}, {0}, {42, 0}, {70, 0},
                                         {36, 42, 0}, {36, 0}, {42, 0}, {37, 0}, {38, 39, 0}, {0}, {42, 63, 0}, {46, 0}};
        uint32_t step = FS * 60u / 120u / 4u, total = DS_NKITS * 16u * step;
        FILE *f = fopen(argv[1], "wb");
        int32_t l[CTL], r[CTL], rv[CTL];
        wav_hdr(f, total / CTL * CTL);
        memset(&drums, 0, sizeof drums);
        drums.set = -2;
        for (kit = DRUM_SAMPLED; kit < DRUM_KITS; kit++) {
            if (!kit_synth(kit))
                continue;
            TDRUM->p[P_E0] = (int16_t)kit;
            for (j = 0; j < 16u; j++) {
                uint32_t q, s;
                for (q = 0; q < 3u && P[j][q]; q++) drum_on(P[j][q], q ? 90 : 115);
                for (s = 0; s < step / CTL; s++) {
                    memset(l, 0, sizeof l), memset(r, 0, sizeof r), memset(rv, 0, sizeof rv);
                    drums_render(l, r, rv, CTL);
                    for (k = 0; k < CTL; k++) wav_put(f, clamp(l[k], -32767, 32767), clamp(r[k], -32767, 32767));
                }
            }
        }
        fclose(f);
    }
    printf("drum kits: %u synthesised x %u sounds bounded, audible, finite; levels near the sampled kit PASS\n",
           DS_NKITS, DS_LANES);
    return 0;
}
