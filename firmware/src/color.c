/* SPDX-License-Identifier: GPL-3.0-only */
/* COLOR: a per-track insert effect on the track's dry mono signal, after DIST and before the SLICER,
 * LEVEL / PAN and the sends (fx.c mix_part); on the drum track before its SLICER (slicer.c slicer_drums).
 *   P_COLOR  OFF / PHASR / WAH / FOLD / RING (OFF: nothing runs)
 *   P_CAMT   PHASR: the notches (0..50 % wet), past 50 % feedback; WAH: the wet mix and the peak;
 *            FOLD: the drive, 1 x .. 16 x; RING: the mix
 *   P_CRATE  PHASR: its LFO, 0.05..40 Hz (as the LFO's RATE); WAH: the sensitivity; FOLD: an LFO moving
 *            the drive by +-50 % (0: none); RING: the carrier, a MIDI note (0: 8 Hz, 60: 262 Hz, 127: 12.5 kHz)
 * PHASR: four first-order allpasses, their corner swept 350 Hz .. 4.2 kHz by a sine LFO, mixed with the dry
 *   signal (two notches), feedback from the last one. WAH: the SVF of dsp.c as a band-pass (its peak at
 *   2 x), the cutoff 220 Hz .. 4.2 kHz from an envelope follower (fast attack, ~45 ms release; the level
 *   of the block before). FOLD: a sine shaper: the signal is a phase, sin() folds it back each time it
 *   passes a quarter turn (round corners, no DC); at 1 x a gentle saturation. RING: times a sine.
 * Fixed point, the coefficients once a block. The effects work on x / 4 held to +-CQ (the products fit
 * 32 bits; a track rarely passes 2^19), mixed back as x + (wet - dry) * mix: the dry part keeps every bit.
 * A type change starts from zero; after CQUIET blocks of silent input the state is cleared (no integer
 * limit cycle left behind) and the track costs a scan of its block. 28 bytes a track. */
#define CQ 131071
#define CQUIET 8
enum { CO_OFF, CO_PHASR, CO_WAH, CO_FOLD, CO_RING };   /* P_COLOR (params.c N_COLOR) */
typedef struct {
    int32_t z[5];                /* PHASR: the first allpass' last input, the four outputs; WAH: z0, z1 the
                                  * SVF, z2 the envelope; FOLD: z0 the phase scale of the last block */
    uint32_t ph;                 /* PHASR / FOLD: the LFO; RING: the carrier */
    uint8_t type, quiet;         /* the type the state belongs to; blocks of silent input in a row */
} col_t;
static col_t col[NTRK];

/* the COLOR of track t on its n samples in b (the caller skips it at OFF) */
static __attribute__((noinline)) void color_track(const track_t *t, int32_t *b, uint32_t n)
{
    col_t *c = &col[t - trk];
    uint32_t type = (uint32_t)t->p[P_COLOR], i, any = 0, ph = c->ph;
    int32_t amt = t->p[P_CAMT], rate = t->p[P_CRATE] & 127, m = amt << 6;   /* m: the mix, Q13 */
    int32_t z0 = c->z[0], z1 = c->z[1], z2 = c->z[2], z3 = c->z[3], z4 = c->z[4];
    for (i = 0; i < n; i++)
        any |= (uint32_t)b[i];
    c->quiet = any ? 0 : (uint8_t)(c->quiet + (c->quiet < CQUIET));
    if (c->quiet >= CQUIET) {                           /* silent input: rung out, rests cleared */
        c->z[0] = c->z[1] = c->z[2] = c->z[3] = c->z[4] = 0;
        return;
    }
    if (c->type != type) {                              /* another effect: from zero */
        c->type = (uint8_t)type;
        z0 = z1 = z2 = z3 = z4 = 0;
        ph = 0;
    }
    switch (type) {
    case CO_PHASR: {
        int32_t idx, k, g, a, fb = amt > 64 ? (amt - 64) * 78 : 0;    /* feedback, Q13: 0..0.6 */
        ph += LFO_INC[rate];
        idx = (75 << 8) + ((osc_sine(ph) * 25) >> 7);   /* the corner: index 50..100 of SVF_G */
        k = idx >> 8;
        g = SVF_G[k] + (((SVF_G[k + 1] - SVF_G[k]) * (idx & 255)) >> 8);
        a = ((g - 4096) << 13) / (g + 4096);            /* (tan - 1) / (tan + 1), Q13 */
        m = (amt < 64 ? amt : 64) << 6;                 /* wet up to 50 %: the deepest notches */
        for (i = 0; i < n; i++) {
            int32_t x = b[i], d = clamp(x >> 2, -CQ, CQ), u, y1, y2, y3, y4;
            u = clamp(d + ((fb * z4 + 4096) >> 13), -CQ, CQ);   /* (rounded: at DC an allpass lifts */
            y1 = clamp(((a * (u - z1) + 4096) >> 13) + z0, -CQ, CQ);   /* its error 1 / (1 + a) x) */
            y2 = clamp(((a * (y1 - z2) + 4096) >> 13) + z1, -CQ, CQ);
            y3 = clamp(((a * (y2 - z3) + 4096) >> 13) + z2, -CQ, CQ);
            y4 = clamp(((a * (y3 - z4) + 4096) >> 13) + z3, -CQ, CQ);
            z0 = u, z1 = y1, z2 = y2, z3 = y3, z4 = y4;
            b[i] = x + (((y4 - d) * m) >> 11);
        }
        break;
    }
    case CO_WAH: {
        tsvf_t cf;
        int32_t reso = 100 + amt / 6, kd = 8192 - reso * 7600 / 127, sum = 0;   /* kd: the SVF's damping (dsp.c) */
        int32_t up = (z2 * (rate + 1)) >> 4;
        tsvf_coef(&cf, (40 << 8) + (up < (60 << 8) ? up : 60 << 8), reso);
        for (i = 0; i < n; i++) {
            int32_t x = b[i], d = clamp(x >> 2, -CQ, CQ), c1 = z0, w;
            tsvf_lp(&cf, d, &z0, &z1);
            w = (((z0 + c1) >> 1) * kd) >> 12;          /* the band-pass (the mean of ic1), unity at its peak */
            w = clamp(w << 1, -CQ, CQ);
            sum += d < 0 ? -d : d;
            b[i] = x + (((w - d) * m) >> 11);
        }
        sum >>= CTL_LOG2;                               /* the block's mean level -> the envelope */
        z2 += sum > z2 ? (sum - z2) >> 1 : -((z2 - sum) >> 6);
        break;
    }
    case CO_FOLD: {
        int32_t dq = 16 + amt * amt / 67, k, dk;        /* the drive, Q4: 1 x .. 16 x */
        if (rate) {
            ph += LFO_INC[rate];
            dq += (dq * osc_sine(ph)) >> 16;
        }
        k = dq << 12;                                   /* phase per x / 4: a quarter turn at x = 65536 / drive */
        if (!z0)
            z0 = k;
        dk = (k - z0) >> CTL_LOG2;                      /* (ramped over the block) */
        for (i = 0; i < n; i++) {
            int32_t x = b[i], d = clamp(x >> 2, -CQ, CQ);
            int32_t w = (sine_i((uint32_t)d * (uint32_t)(z0 + dk * (int32_t)i)) * 10430) >> 15;   /* x 65536 / 2 pi */
            b[i] = x + ((w - d) << 2);
        }
        z0 = k;
        break;
    }
    case CO_RING: {
        uint32_t inc = pitch_inc(rate * 16);
        for (i = 0; i < n; i++) {
            int32_t x = b[i], d = clamp(x >> 2, -CQ, CQ), w = (d * (sine_i(ph) >> 1)) >> 14;
            ph += inc;
            b[i] = x + (((w - d) * m) >> 11);
        }
        break;
    }
    }
    c->z[0] = z0, c->z[1] = z1, c->z[2] = z2, c->z[3] = z3, c->z[4] = z4;
    c->ph = ph;
}
