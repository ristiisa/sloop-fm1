/* SPDX-License-Identifier: MIT
 * Copyright 2013 Emilie Gillet.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
 * associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
 * OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. */
/* Peaks drum models by Emilie Gillet (Mutable Instruments Peaks, peaks/drums/: bass_drum, snare_drum,
 * high_hat, fm_drum, svf, excitation; MIT), ported to C for SLOOP's synthesised kits (drum_synth.c
 * includes this file; its dsv_t holds the state, pkv_t). Changes: 44.1 kHz instead of 48 kHz (the
 * pulse decays, delays and filter coefficients recomputed for it, no lookup tables: the SVF's f from
 * PITCH_INC and the sine, its damping by two square roots), the per-voice parameters of a dsnd_t, the
 * slow envelopes at the control rate (CTL) and ramped, velocity and level at the output, the hi-hat's
 * colour high-pass as two one-poles (the same critically damped filter, a third of the work), a voice
 * ends at -54 dB (the integer SVF's small limit cycles would ring on). The fields of a Peaks sound
 * (tools/gen_drumkits.py BD, SD, HH, FM):
 *   BD  pitch: the resonator; bend: PUNCH; decay: DECAY; fcut: the TONE low-pass coefficient / 256
 *   SD  pitch: the body; fcut: TONE; nlev: SNAPPY; decay: DECAY (knobs 0..255 = Peaks' 0..65535)
 *   HH  pitch: the first of the six squares; decay: DECAY_K index of the VCA; fcut / hpf: the notes of
 *       the noise band-pass (run twice a sample) and the colour high-pass; t2lev: band-pass resonance / 128
 *   FM  pitch: the sine; bend: FM AMOUNT; nlev: NOISE (128 = none, below: overdrive); decay / btime:
 *       DECAY_K indices of the AM and FM envelopes; t2: the attack sweep's depth / 8 */
#define PK_D 44                  /* 1 ms: the second pulses */
#define PK_ATK 176               /* 4 ms: BD's resonator 17 semitones up */

static uint32_t pk_sqrt(uint32_t x)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > x)
        b >>= 2;
    while (b) {
        if (x >= r + b) {
            x -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}
/* SVF damping for a resonance 0..32767: 2 (1 - (r / 32896) ^ 1/4), Q15 (Peaks lut_svf_damp) */
static int32_t pk_damp(int32_t r)
{
    int32_t q = (int32_t)pk_sqrt(pk_sqrt((uint32_t)r * 32640u) << 15);
    return q ? 65536 - 2 * q : 65534;
}
/* SVF f = 2 sin(pi fc / fs), Q15, fc at most fs / 8 (sh = 1: run twice a sample) */
static int32_t pk_f(int32_t p16, uint32_t sh)
{
    uint32_t inc = ds_inc(p16) >> sh;
    return 2 * sine_i((inc > 1u << 29 ? 1u << 29 : inc) >> 1);
}
/* x * f >> 15 for an x far beyond 16 bits (the pulses) */
static inline int32_t pk_mul(int32_t x, int32_t f) { return (x >> 15) * f + (((x & 32767) * f) >> 15); }
/* one step of the SVF (svf.h): returns the high-pass; v->bp, v->lp the others. pk_svfx: for an input far
 * beyond 16 bits (the pulses) */
static inline int32_t pk_svf(pksvf_t *v, int32_t in, int32_t f, int32_t damp)
{
    int32_t hp;
    v->lp = clamp(v->lp + ((f * v->bp) >> 15), -32767, 32767);
    hp = in - ((v->bp * damp) >> 15) - v->lp;
    v->bp = clamp(v->bp + ((f * hp) >> 15), -32767, 32767);
    return hp;
}
static inline int32_t pk_svfx(pksvf_t *v, int32_t in, int32_t f, int32_t damp)
{
    int32_t hp;
    v->lp = clamp(v->lp + ((f * v->bp) >> 15), -32767, 32767);
    hp = in - ((v->bp * damp) >> 15) - v->lp;
    v->bp = clamp(v->bp + pk_mul(hp, f), -32767, 32767);
    return hp;
}
/* Peaks' envelope curve 1 - (1 - e^-4x) / (1 - e^-4) from e^-4x (Q16), Q16 */
static uint32_t pk_env(uint32_t e) { return e > 1200u ? ((e - 1200u) * 66759u) >> 16 : 0u; }

/* FM: the envelopes at the end of the block (aux: 20 ms), the pitch and the level they give */
static void pk_fm_control(dsv_t *s)
{
    pkv_t *P = &s->pk;
    P->fm.eaux = (P->fm.eaux * 56683u) >> 16;
    P->fm.efm = (P->fm.efm * P->fm.kfm) >> 16;
    P->fm.eam = (P->fm.eam * s->ka) >> 16;
    s->inc_to = ds_inc((s->base16 * 8 + (int32_t)((pk_env(P->fm.efm) * (uint32_t)P->fm.fma) >> 16) +
                        (int32_t)((pk_env(P->fm.eaux) * (uint32_t)P->fm.aux) >> 15)) >> 3);
    s->amp_to = (int32_t)(pk_env(P->fm.eam) >> 1);
}

static void pk_on(dsv_t *s, uint32_t note)
{
    static const int16_t HH[6] = {0, 109, -74, 32, 87, 184};   /* 540 800 414 607 740 1050 Hz */
    static const uint16_t SDK[4] = {63343, 63884, 64431, 64981};   /* snappy: 4092..4095 / 4096 a sample at 48 kHz, a block here */
    const dsnd_t *d = s->d;
    pkv_t *P = &s->pk;
    int32_t p16 = s->base16;
    uint32_t i, k;
    s->rnd = (int32_t)(0x9E3779B9u ^ (note * 2654435761u) ^ rng());
    switch (d->wave) {
    case DW_PK_BD:
        k = 65535u - d->decay * 257u;
        k = (((k * k) >> 16) * k) >> 18;
        P->sv[0].f = pk_f(p16, 0);
        P->sv[0].damp = pk_damp(32640 - (int32_t)k);
        P->x.p1 = pk_f(p16 + 17 * 16, 0);               /* the attack */
        k = d->bend * 257u;
        k = (k * k) >> 16;
        P->x.p2 = (int32_t)((((k * k) >> 24) * 279u) >> 8);   /* punch (x 48 / 44.1: the same Hz) */
        P->x.lpk = d->fcut << 8;
        P->x.e1 = 275251;                               /* 12 x 32768 x 0.7 */
        break;
    case DW_PK_SD:
        k = d->decay * 257u;
        P->sv[0].f = pk_f(p16, 0);
        P->sv[1].f = pk_f(p16 + 12 * 16, 0);
        P->x.sn.f = pk_f(p16 + 48 * 16, 0);
        if (P->x.sn.f > 21845)
            P->x.sn.f = 21845;                          /* 4.9 kHz: f x hp within 32 bits */
        P->sv[0].damp = pk_damp(29000 + (int32_t)(k >> 5));
        P->sv[1].damp = pk_damp(26500 + (int32_t)(k >> 5));
        P->x.sn.damp = pk_damp(2000);
        s->kn = SDK[k >> 14];
        P->x.p1 = 22000 - (d->fcut * 257 >> 2);         /* body gains */
        P->x.p2 = 22000 + (d->fcut * 257 >> 2);
        i = d->nlev * 257u >> 1;
        s->nz = 512 + (int32_t)(i > 28672u ? 28672u : i);
        s->nz_to = (int32_t)(((uint32_t)s->nz * s->kn) >> 16);
        P->x.e1 = 491520;                               /* 15 x 32768 */
        break;
    case DW_PK_HH:
        for (i = 0; i < 6u; i++)
            P->hh.inc[i] = ds_inc(p16 + HH[i]);
        P->sv[0].f = pk_f(d->fcut * 16, 1);
        P->sv[0].damp = pk_damp((d->t2lev < 218u ? d->t2lev : 218) << 7);
        {   /* the colour high-pass: Peaks runs an SVF twice a sample, damping 2; here two one-poles at 44.1 kHz,
             * a = g / (1 + g), g = tan(pi fc / fs) (fc at most fs / 4) */
            uint32_t inc = ds_inc(d->hpf * 16) >> 1;
            int32_t sn = sine_i(inc > 1u << 29 ? 1u << 29 : inc), cs = sine_i((inc > 1u << 29 ? 1u << 29 : inc) + (1u << 30));
            P->sv[1].f = (sn << 15) / (sn + cs);
        }
        s->ka = ds_k32(d->decay);
        s->amp = 30720;                                 /* 15 x 32768 >> 4 */
        s->amp_to = (int32_t)((30720u * s->ka) >> 16);
        break;
    default:                                            /* FM */
        k = d->nlev * 257u;
        P->fm.fma = (int32_t)((((d->bend * 257u) >> 2) * 3u) >> 2);
        P->fm.aux = d->t2 << 3;
        P->fm.noise = k >= 32768u ? (int32_t)((((k - 32768u) * (k - 32768u)) >> 17) * 5u) : 0;
        P->fm.od = k <= 32767u ? (int32_t)(((32767u - k) * (32767u - k)) >> 14) : 0;
        P->fm.eam = P->fm.efm = P->fm.eaux = 65536;
        s->ka = ds_k32(d->decay);
        P->fm.kfm = ds_k32(d->btime);
        s->ph = (0x3FFFu * (uint32_t)P->fm.fma) >> 16;
        s->amp = 32767;
        s->inc = ds_inc((p16 * 8 + P->fm.fma + 2 * P->fm.aux) >> 3);
        pk_fm_control(s);
        break;
    }
}

/* n (<= CTL) samples of a Peaks voice into out[]; returns 0 when it ended */
static __attribute__((noinline)) int pk_render(dsv_t *s, int32_t *out, uint32_t n)
{
    pkv_t *P = &s->pk;
    pksvf_t *v0 = &P->sv[0], *v1 = &P->sv[1];
    uint32_t i, t = s->t;
    int32_t g = s->gain, lg = s->lg, x, alive, mx = 0;      /* mx: the block's |x|, ORed */
    switch (s->d->wave) {
    case DW_PK_BD: {                                    /* (the states in locals: out[] could alias them) */
        pksvf_t r = *v0;
        int32_t punch = P->x.p2, lpk = P->x.lpk, lp = P->x.lp;
        for (i = 0; i < n; i++, t++) {
            int32_t ex = 0, f = r.f, damp = r.damp;
            if (t < PK_ATK) {                           /* the pulses (bass_drum.cc), the resonator up */
                if (t == PK_D)
                    P->x.e2 = 13763;                    /* 19662 x 0.7, down */
                ex = P->x.e1 - P->x.e2 + (t <= PK_D ? 16384 : 0);
                P->x.e1 = (P->x.e1 * 3280) >> 12;
                P->x.e2 = (P->x.e2 * 2995) >> 12;
                f = P->x.p1;
            }
            if (punch) {                                /* punch: the louder, the higher and the less damped */
                int32_t ps = r.lp > 4096 ? r.lp : 2048;
                f += ((ps >> 4) * punch) >> 9;
                damp += (ps - 2048) >> 3;
            }
            pk_svfx(&r, ex, f, damp);
            x = (ex >> 4) + r.bp;
            lp += (((x - lp) >> 2) * lpk) >> 13;
            x = clamp(lp, -32767, 32767);
            mx |= x ^ (x >> 31);
            out[i] = ((mulq15(x, g) >> 2) * lg) >> 8;
        }
        v0->lp = r.lp, v0->bp = r.bp, P->x.lp = lp;
        alive = t < PK_ATK || mx >> 6;                 /* -54 dB (the SVF's small limit cycles never end) */
        break;
    }
    case DW_PK_SD: {
        int32_t z = s->nz, dz = (s->nz_to - s->nz) >> CTL_LOG2, g1 = P->x.p1, g2 = P->x.p2, rnd = s->rnd;
        pksvf_t b1 = *v0, b2 = *v1, sn = P->x.sn;
        for (i = 0; i < n; i++, t++) {
            int32_t e1 = 0, e2 = 0, nz;
            if (t < 2u * PK_D) {                        /* the pulses (snare_drum.cc) */
                if (t == PK_D) {
                    P->x.e2 = 32768;
                    P->x.e3 = 13107;
                }
                e1 = P->x.e1 - P->x.e2 + (t < PK_D ? 2621 : 0);
                e2 = P->x.e3 + (t < PK_D ? 13107 : 0);
                P->x.e1 = (P->x.e1 * 1408) >> 12;
                P->x.e2 = (P->x.e2 * 2995) >> 12;
                P->x.e3 = (P->x.e3 * 1077) >> 12;
            }
            pk_svfx(&b1, e1, b1.f, b1.damp);
            pk_svf(&b2, e2, b2.f, b2.damp);
            pk_svf(&sn, (int32_t)noise32(&rnd) >> 16, sn.f, sn.damp);
            nz = (z * sn.bp) >> 15;
            x = (((b1.bp + (e1 >> 4)) * g1) >> 15) + (((b2.bp + (e2 >> 4)) * g2) >> 15) + nz;
            x = clamp(x, -32767, 32767);
            mx |= x ^ (x >> 31);
            out[i] = ((mulq15(x, g) >> 2) * lg) >> 8;
            z += dz;
        }
        *v0 = b1, *v1 = b2, P->x.sn = sn, s->rnd = rnd;
        s->nz = s->nz_to;
        s->nz_to = (int32_t)(((uint32_t)s->nz * s->kn) >> 16);
        alive = s->nz > 8 || mx >> 6;
        break;
    }
    case DW_PK_HH: {                                    /* (the states in locals: out[] could alias them) */
        int32_t a = s->amp, da = (s->amp_to - s->amp) >> CTL_LOG2, sq, h;
        int32_t f0 = v0->f, d0 = v0->damp, l0 = v0->lp, b0 = v0->bp, f1 = v1->f, l1 = v1->lp, b1 = v1->bp;
        uint32_t p0 = P->hh.ph[0], p1 = P->hh.ph[1], p2 = P->hh.ph[2], p3 = P->hh.ph[3], p4 = P->hh.ph[4], p5 = P->hh.ph[5];
        const uint32_t *inc = P->hh.inc;
        for (i = 0; i < n; i++) {
            p0 += inc[0];                               /* six squares (high_hat.cc) */
            p1 += inc[1];
            p2 += inc[2];
            p3 += inc[3];
            p4 += inc[4];
            p5 += inc[5];
            sq = (int32_t)(((p0 >> 31) + (p1 >> 31) + (p2 >> 31) + (p3 >> 31) + (p4 >> 31) + (p5 >> 31)) << 12);
            l0 = clamp(l0 + ((f0 * b0) >> 15), -32767, 32767);   /* the band-pass, twice a sample: stable up there */
            h = sq - ((b0 * d0) >> 15) - l0;
            b0 += (f0 * h) >> 15;                       /* (within 16 bits up to resonance 218 / 256: no clip) */
            x = b0;
            l0 = clamp(l0 + ((f0 * b0) >> 15), -32767, 32767);
            h = sq - ((b0 * d0) >> 15) - l0;
            b0 += (f0 * h) >> 15;
            x = clamp(x + b0, 0, 32767);                /* the 808 VCA: the positive half only */
            x = (a * x) >> 14;
            if (x > 32767)
                x = 32767;
            a += da;
            l1 += ((x - l1) * f1) >> 15;                /* the colour high-pass (critically damped: two one-poles) */
            x -= l1;
            b1 += ((x - b1) * f1) >> 15;
            x = clamp((x - b1) * 4, -32767, 32767);
            out[i] = ((mulq15(x, g) >> 2) * lg) >> 8;
        }
        v0->lp = l0, v0->bp = b0, v1->lp = l1, v1->bp = b1;
        P->hh.ph[0] = p0, P->hh.ph[1] = p1, P->hh.ph[2] = p2, P->hh.ph[3] = p3, P->hh.ph[4] = p4, P->hh.ph[5] = p5;
        s->amp = s->amp_to;
        s->amp_to = (int32_t)(((uint32_t)s->amp * s->ka) >> 16);
        alive = s->amp > 4;
        break;
    }
    default: {                                          /* FM (fm_drum.cc) */
        int32_t a = s->amp, da = (s->amp_to - s->amp) >> CTL_LOG2;
        int32_t inc = (int32_t)s->inc, di = (int32_t)(s->inc_to - s->inc) >> CTL_LOG2;
        int32_t nzl = P->fm.noise, od = P->fm.od, prev = P->fm.prev, rnd = s->rnd;
        uint32_t ph = s->ph;
        for (i = 0; i < n; i++) {
            int32_t fb = (inc >> 16) * prev;            /* its own output bends the pitch */
            ph += (uint32_t)(inc + (fb >> 1) - (fb >> 5));
            x = sine_i(ph);
            if (nzl)
                x = (x * (65535 - nzl) + ((int32_t)noise32(&rnd) >> 16) * nzl) >> 16;
            x = (x * a) >> 15;
            if (od)
                x = (x * (65535 - od) + softclip(x * 5) * od) >> 16;
            prev = x;
            out[i] = ((mulq15(x, g) >> 2) * lg) >> 8;
            a += da;
            inc += di;
        }
        P->fm.prev = prev, s->ph = ph, s->rnd = rnd;
        s->amp = s->amp_to;
        s->inc = s->inc_to;
        pk_fm_control(s);
        alive = s->amp > 0;
        break;
    }
    }
    s->t += n;
    return alive;
}
