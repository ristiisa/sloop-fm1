/* SPDX-License-Identifier: GPL-3.0-only */
/* DICE (EDIT + PRESETS; seq.c includes it after mutate): a new pattern for the selected track, in a style,
 * a roll a detent; turned back, the rolls before (made again from their seeds), and at last the pattern
 * there was before the first one.
 *   drums  a groove of the style from its bar (HOUSE, TRAP, BOOM BAP, BREAK, AMAPIANO, DNB) with chance and
 *          euclidean variations (RANDOM: euclidean throughout), levels and ratchets; the bar again up to
 *          LEN, a little different each time, a fill at the end of 2 / 4 bars, a crash on the first
 *   synth  a bass line, a melody or (CHORD on, POLY) the chords of the scale, over a progression, in the
 *          style's rhythm: the notes in the scale, in the register of the notes the track had (else of
 *          its sound: a bass around C2, the rest around C4), ties, slides, levels, a few ratchets
 *   ACID   synth: a 303 line (dice_acid); drums: HOUSE as acid house (open hats off the beat, 16th hats)
 * A roll writes every step within LEN (no condition, no lock left there), the steps past it stay. It
 * depends only on its seed, its style and the track's LEN, key, chord and voice mode (the register is
 * taken once, at the first roll): the rolls kept turn back exactly while those stay. */
enum { DS_KIT, DS_HOUSE, DS_TRAP, DS_BOOMBAP, DS_BREAK, DS_AMAPIANO, DS_DNB, DS_ACID, DS_RANDOM, DS_COUNT };
static const char *const N_DICE[DS_COUNT] = {"KIT", "HOUSE", "TRAP", "BOOM BAP", "BREAK", "AMAPIANO", "DNB", "ACID", "RANDOM"};
#define DICE_DEPTH 16u                   /* rolls kept to turn back */
static struct {
    uint8_t n, trk;                      /* rolls kept, their track */
    uint8_t bass, lo, hi;                /* synth: a bass line, the register (from the first roll) */
    uint8_t acid[3];                     /* the last ACID line's density, accent and slide (%) */
    uint32_t sum, ctx;                   /* the pattern after the last roll, the parameters it was rolled with */
    uint32_t seed[DICE_DEPTH];
    uint8_t style[DICE_DEPTH];
} dice;
static step_t dice_was[NSTEP] __attribute__((section(".pool")));   /* the pattern before the first roll */
static uint8_t dice_wcond[NSTEP] __attribute__((section(".pool")));
static int8_t dice_wmicro[NSTEP] __attribute__((section(".pool")));   /* .. its nudges and locks */
static plock_t dice_wlk[NLOCK] __attribute__((section(".pool")));

/* DS_KIT: the style of the drum kit (by its name; any other: RANDOM) */
static const struct { const char *kit; uint8_t style; } DICE_KIT[] = {
    {"808", DS_TRAP}, {"TRAP", DS_TRAP}, {"DRILL", DS_TRAP}, {"PHONK", DS_TRAP}, {"DUBSTEP", DS_TRAP},
    {"909", DS_HOUSE}, {"606", DS_HOUSE}, {"80S", DS_HOUSE}, {"HOUSE", DS_HOUSE}, {"D.HOUSE", DS_HOUSE},
    {"TECHNO", DS_HOUSE}, {"MINIMAL", DS_HOUSE}, {"DISCO", DS_HOUSE}, {"GARAGE", DS_HOUSE}, {"SYNTHWV", DS_HOUSE},
    {"BOOMBAP", DS_BOOMBAP}, {"LO-FI", DS_BOOMBAP}, {"DEEP", DS_BOOMBAP}, {"DUST", DS_BOOMBAP}, {"JAZZ", DS_BOOMBAP},
    {"ACOUSTIC", DS_BREAK}, {"TIGHT", DS_BREAK}, {"BRIGHT", DS_BREAK}, {"VINTAGE", DS_BREAK}, {"ELECTRO", DS_BREAK},
    {"JUNGLE", DS_DNB}, {"AMAPIANO", DS_AMAPIANO}, {"AFRO", DS_AMAPIANO},
};
static uint32_t dice_kit_style(void)
{
    uint32_t i;
    for (i = 0; i < sizeof DICE_KIT / sizeof DICE_KIT[0]; i++)
        if (str_eq(DRUM_KIT_NAMES[drum_kit()], DICE_KIT[i].kit))
            return DICE_KIT[i].style;
    return DS_RANDOM;
}

/* the roll's own random numbers, from its seed (a roll made again is the same) */
static uint32_t dice_rs;
static __attribute__((noinline)) uint32_t drnd(uint32_t n)   /* 0 .. n - 1 (out of line, as dchance and dh: called all over) */
{
    dice_rs ^= dice_rs << 13;
    dice_rs ^= dice_rs >> 17;
    dice_rs ^= dice_rs << 5;
    return dice_rs % n;
}
static __attribute__((noinline)) int dchance(uint32_t pct) { return drnd(100) < pct; }
static int euclid(uint32_t hits, uint32_t steps, uint32_t rot, uint32_t i)   /* hits spread over steps, turned */
{
    return (i + rot) % steps * hits % steps < hits;
}
static uint32_t dice_level(uint32_t lv)          /* a level one softer or louder */
{
    uint32_t r = lv == LV_GHOST ? 0u : lv == LV_SOFT ? 1u : lv == LV_NORM ? 2u : 3u;
    return MUT_LV[r == 0u ? 1u : r == 3u ? 2u : drnd(2) ? r + 1u : r - 1u];
}

/* ------------------------------------------------------------- drums --- */
static dstep_t dice_bar[16], dice_cur[16];       /* the style's bar, the bar being written */
static const uint8_t DICE_PERC[5] = {LANE_RIM, LANE_CONGA, LANE_BELL, LANE_TOM_HI, LANE_SHAKER};

static __attribute__((noinline)) void dh(uint32_t i, uint32_t l, uint32_t lv, uint32_t rat) { dstep_set(&dice_bar[i & 15u], l, lv, rat); }
static void dh_mask(uint32_t m, uint32_t l, uint32_t lv)   /* lane l on the steps of m (bit i: step i) */
{
    uint32_t i;
    for (i = 0; i < 16u; i++)
        if ((m >> i) & 1u)
            dh(i, l, lv, 0);
}
static void dh_hats(uint32_t l, uint32_t every)   /* a hat each 8th / 16th: the beats NORM, the rest softer */
{
    uint32_t i;
    for (i = 0; i < 16u; i += every)
        dh(i, l, i % 4u == 0u ? LV_NORM : (i & 1u) ? (dchance(50) ? LV_GHOST : LV_SOFT) : dchance(30) ? LV_NORM : LV_SOFT, 0);
}
static void dh_perc(uint32_t l, uint32_t lv)     /* 3, 5 or 7 hits of 16 (turned), off the beats */
{
    uint32_t i, k = 3u + 2u * drnd(3), r = drnd(16);
    for (i = 0; i < 16u; i++)
        if (i % 4u && euclid(k, 16, r, i))
            dh(i, l, dchance(30) ? LV_GHOST : lv, 0);
}

static void dice_groove(uint32_t style)          /* the style's bar */
{
    static const uint8_t TRAP_K[6] = {3, 6, 7, 10, 11, 14}, AMA_K[4] = {3, 6, 11, 14};
    static const uint8_t BB_G[6] = {3, 7, 9, 11, 14, 15}, DNB_G[5] = {3, 6, 9, 14, 15};
    static const uint16_t BB_K[6] = {0x0401, 0x0481, 0x0409, 0x0501, 0x2401, 0x8441};
    static const uint16_t BRK_K[4] = {0x0C05, 0x0449, 0x0441, 0x0485};   /* amen, funky, think, ... */
    static const uint16_t BRK_G[4] = {0x8280, 0x4280, 0x2200, 0x8240};   /* .. their ghost snares */
    static const uint16_t DNB_K[5] = {0x0401, 0x0481, 0x2401, 0x0405, 0x0C01};
    uint32_t i, k, l, r;
    memset(dice_bar, 0, sizeof dice_bar);
    switch (style) {
    case DS_HOUSE:                                /* four on the floor, the clap on 2 and 4, open hats off the beat */
    case DS_ACID:                                 /* (acid house: always the open hats, the 16th hats between) */
        l = dchance(75) || style == DS_ACID ? LANE_OPEN : LANE_RIDE;
        for (i = 0; i < 16u; i += 4u) {
            dh(i, LANE_KICK, i ? LV_NORM : LV_HARD, 0);
            dh(i + 2u, l, LV_NORM, 0);
        }
        dh(4, LANE_CLAP, LV_NORM, 0);
        dh(12, LANE_CLAP, LV_NORM, 0);
        if (dchance(35)) {
            dh(4, LANE_SNARE, LV_SOFT, 0);
            dh(12, LANE_SNARE, LV_SOFT, 0);
        }
        k = style == DS_ACID ? 0u : drnd(3);
        for (i = 0; i < 16u && k < 2u; i++)       /* closed hats between: the 16ths, or the odd ones */
            if (k ? i & 1u : i % 4u != 2u)
                dh(i, LANE_HAT, i % 4u == 0u ? LV_SOFT : dchance(50) ? LV_GHOST : LV_SOFT, 0);
        if (dchance(75))
            dh_perc(DICE_PERC[drnd(5)], LV_SOFT);
        if (dchance(25))
            for (i = 1; i < 16u; i += 2u)
                dh(i, LANE_SHAKER, LV_GHOST, 0);
        break;
    case DS_TRAP:                                 /* a sparse kick, the snare / clap on 3, hats with rolls */
        dh(0, LANE_KICK, LV_HARD, 0);
        if (dchance(40))
            dh(0, LANE_KICK2, LV_NORM, 0);        /* (the long 808 of the trap kits) */
        for (i = 0, k = 0; i < 6u; i++)
            if (dchance(35)) {
                dh(TRAP_K[i], LANE_KICK, dchance(30) ? LV_SOFT : LV_NORM, 0);
                k++;
            }
        if (!k)
            dh(TRAP_K[drnd(6)], LANE_KICK, LV_NORM, 0);
        dh(8, LANE_CLAP, LV_HARD, 0);
        if (dchance(60))
            dh(8, LANE_SNARE, LV_NORM, 0);
        dh_hats(LANE_HAT, dchance(50) ? 1u : 2u);
        for (k = 1u + drnd(3); k; k--) {          /* rolls: a hat x2 .. x4, soft */
            i = 1u + drnd(15);
            l = dchance(50) ? LV_SOFT : LV_GHOST;
            dh(i, LANE_HAT, l, 1u + drnd(3));
        }
        if (dchance(40))
            dh(dchance(50) ? 6u : 14u, LANE_OPEN, LV_SOFT, 0);
        if (dchance(30))
            dh(dchance(50) ? 7u : 15u, LANE_SNARE, LV_GHOST, 0);
        break;
    case DS_BOOMBAP:                              /* a lazy kick, the snare on 2 and 4, ghost snares, 8th hats */
        dh_mask(BB_K[drnd(6)], LANE_KICK, LV_NORM);
        dh(0, LANE_KICK, LV_HARD, 0);
        for (i = 0; i < 6u; i++)
            if (dchance(22))
                dh(BB_G[i], LANE_SNARE, LV_GHOST, 0);
        dh(4, LANE_SNARE, LV_NORM, 0);
        dh(12, LANE_SNARE, LV_HARD, 0);
        dh_hats(dchance(80) ? LANE_HAT : LANE_RIDE, 2u);
        if (dchance(35))
            dh(dchance(50) ? 7u : 15u, LANE_HAT, LV_GHOST, 0);
        if (dchance(30))
            dh(14, LANE_OPEN, LV_SOFT, 0);
        if (dchance(30))
            dh_perc(dchance(50) ? LANE_RIM : LANE_SHAKER, LV_SOFT);
        break;
    case DS_BREAK:                                /* a breakbeat: one of the classic kicks, its ghost snares */
        k = drnd(4);
        dh_mask(BRK_K[k], LANE_KICK, dchance(40) ? LV_SOFT : LV_NORM);
        dh(0, LANE_KICK, LV_HARD, 0);
        dh_mask(BRK_G[k], LANE_SNARE, LV_GHOST);
        if (dchance(50))
            dh(1u + 2u * drnd(8), LANE_SNARE, LV_GHOST, 0);
        if (dchance(30))
            dh(1u + 2u * drnd(8), LANE_KICK, LV_SOFT, 0);
        dh(4, LANE_SNARE, LV_NORM, 0);
        dh(12, LANE_SNARE, LV_HARD, 0);
        l = dchance(30) ? LANE_RIDE : LANE_HAT;
        dh_hats(l, 2u);
        for (i = 1; i < 16u; i += 2u)
            if (dchance(30))
                dh(i, l, LV_GHOST, 0);
        if (dchance(50))
            dh(dchance(50) ? 14u : 6u, LANE_OPEN, LV_SOFT, 0);
        if (dchance(30))
            dh_perc(DICE_PERC[drnd(4)], LV_SOFT);
        break;
    case DS_AMAPIANO:                             /* shaker 16ths, a syncopated kick, the rim around it */
        for (i = 0; i < 16u; i++)
            dh(i, LANE_SHAKER, i % 4u == 2u ? LV_NORM : i & 1u ? LV_GHOST : LV_SOFT, 0);
        dh(0, LANE_KICK, LV_HARD, 0);
        dh(8, LANE_KICK, LV_NORM, 0);
        if (dchance(60)) {
            dh(4, LANE_KICK, LV_NORM, 0);
            dh(12, LANE_KICK, LV_NORM, 0);
        }
        dh(AMA_K[drnd(4)], LANE_KICK, LV_SOFT, 0);
        r = drnd(8);
        for (i = 0; i < 16u; i++)                 /* the tresillo, turned */
            if (i % 4u && euclid(3, 8, r, i % 8u))
                dh(i, LANE_RIM, dchance(40) ? LV_SOFT : LV_NORM, 0);
        dh(12, LANE_CLAP, LV_NORM, 0);
        if (dchance(60))
            dh(4, LANE_CLAP, LV_SOFT, 0);
        if (dchance(50))
            dh_perc(LANE_CONGA, LV_SOFT);
        break;
    case DS_DNB:                                  /* two-step: the kick on 1 and the "and" of 3, the snare on 2 and 4 */
        dh_mask(DNB_K[drnd(5)], LANE_KICK, LV_NORM);
        dh(0, LANE_KICK, LV_HARD, 0);
        for (i = 0; i < 5u; i++)
            if (dchance(25))
                dh(DNB_G[i], LANE_SNARE, LV_GHOST, 0);
        dh(4, LANE_SNARE, LV_HARD, 0);
        dh(12, LANE_SNARE, LV_HARD, 0);
        k = drnd(3);
        dh_hats(k == 2u ? LANE_RIDE : LANE_HAT, k == 1u ? 1u : 2u);
        if (dchance(30))
            for (i = 1; i < 16u; i += 2u)
                dh(i, LANE_SHAKER, LV_GHOST, 0);
        if (dchance(30))
            dh(dchance(50) ? 6u : 14u, LANE_OPEN, LV_SOFT, 0);
        break;
    default: {                                    /* RANDOM: euclidean kick, hats, percussion */
        static const uint8_t HATS[4] = {LANE_HAT, LANE_RIDE, LANE_SHAKER, LANE_PEDAL};
        static const uint8_t PERC[7] = {LANE_RIM, LANE_CONGA, LANE_BELL, LANE_TOM_LO, LANE_TOM_HI, LANE_SNARE2, LANE_OPEN};
        k = 3u + drnd(4);
        r = drnd(16);
        for (i = 0; i < 16u; i++)
            if (euclid(k, 16, r, i))
                dh(i, LANE_KICK, dchance(25) ? LV_SOFT : LV_NORM, 0);
        dh(0, LANE_KICK, LV_HARD, 0);
        l = dchance(50) ? LANE_SNARE : LANE_CLAP;
        k = drnd(3);
        if (k == 0u) {
            dh(4, l, LV_NORM, 0);
            dh(12, l, LV_NORM, 0);
        } else if (k == 1u) {
            dh(8, l, LV_NORM, 0);
        } else {
            r = drnd(16);
            for (i = 1; i < 16u; i++)
                if (euclid(3, 16, r, i))
                    dh(i, l, LV_NORM, 0);
        }
        l = HATS[drnd(4)];
        k = 5u + drnd(9);
        r = drnd(16);
        for (i = 0; i < 16u; i++)
            if (euclid(k, 16, r, i)) {
                uint32_t lv = i % 4u == 0u ? LV_NORM : dchance(50) ? LV_SOFT : LV_GHOST;
                dh(i, l, lv, dchance(12) ? 1u + drnd(2) : 0u);
            }
        for (k = drnd(3); k; k--)
            dh_perc(PERC[drnd(7)], LV_SOFT);
        break;
    }
    }
}

/* a later bar: one or two small changes (the kicks on the beats and the snares that are not ghosts stay) */
static void dice_vary(uint32_t style)
{
    static const uint8_t HATS[4] = {LANE_HAT, LANE_SHAKER, LANE_RIDE, LANE_PEDAL};
    uint32_t k = 1u + drnd(2), i, j, l;
    dstep_t *s;
    while (k--) {
        s = &dice_cur[i = drnd(16)];
        switch (drnd(4)) {
        case 0:                                   /* a ghost snare (rim) comes or goes off the beat */
            l = style == DS_HOUSE || style == DS_ACID || style == DS_AMAPIANO ? LANE_RIM : LANE_SNARE;
            if (!(i % 4u))
                break;
            if (!dstep_has(s, l))
                dstep_set(s, l, LV_GHOST, 0);
            else if (dstep_lvl(s, l) == LV_GHOST)
                dstep_clr(s, l);
            break;
        case 1:                                   /* a soft kick off the beat comes or goes */
            if (!(i % 4u))
                break;
            if (dstep_has(s, LANE_KICK))
                dstep_clr(s, LANE_KICK);
            else
                dstep_set(s, LANE_KICK, LV_SOFT, 0);
            break;
        case 2:                                   /* a hat louder / softer, or a ratchet */
            for (j = 0; j < 4u && !dstep_has(s, HATS[j]); j++)
                ;
            if (j == 4u)
                break;
            l = HATS[j];
            if (dchance(60))
                dstep_set(s, l, dice_level(dstep_lvl(s, l)), dstep_rat(s, l));
            else
                dstep_set(s, l, dstep_lvl(s, l), dstep_rat(s, l) ? 0u : 1u);
            break;
        default:                                  /* a percussion hit comes or goes off the beat (not the shaker) */
            l = DICE_PERC[drnd(4)];
            if (!(i % 4u))
                break;
            if (dstep_has(s, l))
                dstep_clr(s, l);
            else
                dstep_set(s, l, LV_SOFT, 0);
            break;
        }
    }
}

static void dice_fill(uint32_t style)            /* the last beat of the bar: snares up, the toms down, a roll */
{
    uint32_t i, l;
    switch (style == DS_TRAP ? 2u : drnd(3)) {
    case 0:
        for (i = 13; i < 16u; i++)
            dstep_set(&dice_cur[i], LANE_SNARE, i == 15u ? LV_NORM : i == 14u ? LV_SOFT : LV_GHOST, i == 15u);
        break;
    case 1:
        dstep_set(&dice_cur[13], LANE_TOM_HI, LV_NORM, 0);
        dstep_set(&dice_cur[14], LANE_TOM_LO, LV_NORM, 0);
        dstep_set(&dice_cur[15], LANE_TOM_LO, LV_SOFT, 1);
        break;
    default:                                      /* ratchets getting faster */
        l = style == DS_TRAP && dchance(50) ? LANE_HAT : LANE_SNARE;
        dstep_set(&dice_cur[14], l, LV_SOFT, 1u + drnd(2));
        dstep_set(&dice_cur[15], l, LV_NORM, 3);
        break;
    }
}

static void dice_drums(track_t *t, uint32_t style, uint32_t len)
{
    uint32_t bars = (len + 15u) / 16u, b, i;
    dice_groove(style);
    for (b = 0; b < bars; b++) {
        memcpy(dice_cur, dice_bar, sizeof dice_cur);
        if (b)
            dice_vary(style);
        if (bars > 1u && (b % 4u == 3u || b + 1u == bars))
            dice_fill(style);
        if (bars > 1u && !b && dchance(40))
            dstep_set(&dice_cur[0], LANE_CRASH, LV_NORM, 0);
        for (i = 0; i < 16u && b * 16u + i < len; i++)
            t->dstep[b * 16u + i] = dice_cur[i];
    }
}

/* ------------------------------------------------------------- synth --- */
/* the rhythm of a bar (bit i: step i): the steps a note always / maybe (pmay %) starts on; the chance it
 * holds on to the next (tie), slides into it, leaps an octave (bass) / a fifth (melody), ratchets */
typedef struct { uint16_t must, may; uint8_t pmay, tie, slide, leap, rat; } dice_rhy_t;
static const dice_rhy_t DICE_RHY[3][6] = {
    {   /* bass: HOUSE off the beat, TRAP the 808 (long, sliding), BOOM BAP, BREAK, AMAPIANO, DNB (long) */
        {0x4444, 0x8989, 20, 10, 0, 30, 0}, {0x0001, 0x4CC8, 40, 95, 40, 20, 5}, {0x0401, 0xE188, 30, 60, 0, 10, 0},
        {0x0001, 0xD6CC, 40, 25, 10, 20, 0}, {0x0041, 0x7408, 45, 60, 35, 15, 0}, {0x0401, 0x40C0, 25, 95, 25, 10, 0},
    }, { /* melody */
        {0x0000, 0x4D4D, 45, 25, 10, 10, 0}, {0x0001, 0x5D5C, 50, 15, 0, 15, 12}, {0x0001, 0x5548, 35, 55, 0, 10, 0},
        {0x0001, 0x6DAC, 45, 20, 10, 15, 5}, {0x0001, 0x4D48, 50, 35, 15, 10, 0}, {0x0101, 0x5050, 30, 90, 20, 10, 0},
    }, { /* chords: stabs, pads */
        {0x0000, 0x4C4C, 50, 0, 0, 0, 0}, {0x0001, 0x0100, 50, 95, 0, 0, 0}, {0x0001, 0x4440, 30, 70, 0, 0, 0},
        {0x0001, 0x1448, 40, 30, 0, 0, 0}, {0x0001, 0x6448, 50, 30, 0, 0, 0}, {0x0001, 0x0100, 60, 95, 0, 0, 0},
    }};
/* progressions: degrees of the scale, a chord a bar (LEN <= 16: half a bar) */
static const int8_t DICE_PROG[8][4] = {{0, 0, 0, 0}, {0, 3, 4, 0}, {0, 5, 3, 4}, {0, 4, 5, 3},
                                       {0, 3, 0, 4}, {0, 5, 6, 4}, {0, 2, 3, 4}, {0, 0, 3, 3}};
static const int8_t DICE_BASS[8] = {0, 0, 0, 0, 4, 4, 2, -1};   /* the bass: degrees from the chord's root */

static uint32_t dice_mask(const track_t *t)      /* the notes it plays: the scale (chords on CHR: minor, as the keys) */
{
    return t->p[P_CHORD] && !t->p[P_SCALE] ? SCALE_MASK[2] : scale_mask(t);
}
static int32_t dice_walk(const track_t *t, int32_t n, int32_t d)   /* d degrees of the scale up / down from n */
{
    uint32_t mask = dice_mask(t), guard;
    for (; d; d += d > 0 ? -1 : 1) {
        guard = 12;
        do
            n += d > 0 ? 1 : -1;
        while (--guard && !((mask >> (uint32_t)((n - t->p[P_ROOT] + 120) % 12)) & 1u));
    }
    return n;
}
static int32_t dice_fold(int32_t n, int32_t lo, int32_t hi)   /* into lo..hi by octaves (hi - lo >= 11) */
{
    while (n > hi)
        n -= 12;
    while (n < lo)
        n += 12;
    return n;
}

/* the first roll: a bass or not, the register: around the notes the track has, else its sound's (a bass
 * C2 .. E3 or as its TRANSPOSE puts the keys, the rest around C4), with the octave of the keys */
static void dice_role(const track_t *t, int bass)
{
    uint32_t len = trk_len(t), i, k, n = 0;
    int32_t a = 127, b = 0, mid, lo, hi;
    for (i = 0; i < len; i++)
        for (k = 0; ms_note(t, i, k); k++, n++) {
            a = t->step[i].note[k] < a ? t->step[i].note[k] : a;
            b = t->step[i].note[k] > b ? t->step[i].note[k] : b;
        }
    mid = (a + b) / 2;
    dice.bass = (uint8_t)(bass || t->p[P_TRANS] <= -12 || (n && mid < 50));
    if (!n)
        mid = (dice.bass ? (t->p[P_TRANS] <= -12 ? 60 + t->p[P_TRANS] : 36) + 7 : 65 + t->p[P_TRANS]) + 12 * song.octave;
    lo = clamp(mid - (dice.bass ? 8 : 9), 12, 96);
    hi = lo + (dice.bass ? 17 : 19);
    if (n && a < lo && hi - a <= 24)
        lo = a;
    if (n && b > hi && b - lo <= 24)
        hi = b;
    dice.lo = (uint8_t)lo;
    dice.hi = (uint8_t)hi;
}

static void dice_synth(track_t *t, uint32_t style, uint32_t len)
{
    uint32_t role = t->p[P_CHORD] && t->p[P_VOICE] == V_POLY ? 2u : dice.bass ? 0u : 1u;
    uint32_t i, j, b, k, m, seg = len > 16u ? 16u : 8u, prog = drnd(8), bars = (len + 15u) / 16u, first = 1;
    uint32_t lim = len < 16u ? len : 16u, w = (1u << lim) - 1u;
    uint8_t hold[16], lv[16], rt[16], sl[16], oct[16];
    int8_t rel[16], rb[16];
    int32_t lo = dice.lo, hi = role == 2u ? dice.lo + 11 : dice.hi, cur = role == 1u ? 2 * (int32_t)drnd(3) : 0, root0, n;
    dice_rhy_t r;
    if (style == DS_RANDOM) {
        uint32_t h = 3u + drnd(8), q = drnd(16);
        r.must = 1;
        r.may = 0;
        for (i = 0; i < 16u; i++)
            r.may |= (uint16_t)(euclid(h, 16, q, i) << i);
        r.pmay = 75, r.tie = 40, r.slide = 20, r.leap = 20, r.rat = 5;
    } else {
        r = DICE_RHY[role][(style - DS_HOUSE) % 6u];
    }
    /* the bar: the steps notes start on, how long each holds, its level, ratchet, slide, pitch */
    m = r.must;
    for (i = 0; i < 16u; i++)
        if (((r.may >> i) & 1u) && dchance(r.pmay))
            m |= 1u << i;
    for (k = 0; k < 16u && (!(m & w) || !(m & w & ((m & w) - 1u))); k++)
        m |= 1u << (2u * drnd((lim + 1u) / 2u)); /* (two at least within LEN, on the 8ths) */
    for (i = 0; i < 16u; i++) {
        if (!((m >> i) & 1u))
            continue;
        for (j = i + 1u; j < 16u && !((m >> j) & 1u); j++)
            ;
        hold[i] = (uint8_t)(dchance(r.tie) ? j - i : j - i > 1u && dchance(25) ? 2u : 1u);
        lv[i] = (uint8_t)(i % 4u == 0u ? (dchance(15) ? LV_HARD : LV_NORM) :
                          (i & 1u) ? (dchance(40) ? LV_SOFT : dchance(15) ? LV_GHOST : LV_NORM) : dchance(25) ? LV_SOFT : LV_NORM);
        rt[i] = (uint8_t)(hold[i] == 1u && dchance(r.rat) ? 1u + drnd(2) : 0u);
        sl[i] = (uint8_t)(j < 16u && hold[i] == j - i && !rt[i] && dchance(r.slide));
        oct[i] = 0;
        if (role == 0u) {
            rel[i] = (int8_t)(first ? 0 : DICE_BASS[drnd(8)]);
            oct[i] = (uint8_t)(!first && dchance(r.leap));
        } else if (role == 1u) {
            cur += dchance(r.leap) ? (dchance(50) ? 4 : -4) : (int32_t)drnd(5) - 2;
            cur = clamp(cur, -4, 9);
            rel[i] = (int8_t)cur;
        } else {
            rel[i] = 0;
        }
        first = 0;
    }
    /* the notes: the bar again up to LEN, over the chords of the progression, a little different each time */
    root0 = role == 1u ? (lo + hi) / 2 - 6 : lo;
    root0 += ((t->p[P_ROOT] - root0) % 12 + 12) % 12;
    for (i = 0; i < len; i++) {
        memset(&t->step[i], 0, sizeof t->step[i]);
        t->step[i].time = ST_REST;
    }
    for (b = 0; b < bars; b++) {
        memcpy(rb, rel, sizeof rb);
        if (b && role != 2u && dchance(35)) {     /* a note a degree away */
            k = drnd(16);
            if ((m >> k) & 1u)
                rb[k] = (int8_t)(rb[k] + (drnd(2) ? 1 : -1));
        }
        if (b && b + 1u == bars && role != 2u && dchance(50)) {
            for (k = 15; !((m >> k) & 1u); k--)
                ;
            rb[k] = (int8_t)(drnd(2) ? 4 : 1);    /* the last note of the last bar: a turn */
        }
        for (i = 0; i < 16u && b * 16u + i < len; i++) {
            uint32_t idx = b * 16u + i, c;
            step_t *s = &t->step[idx];
            int32_t croot;
            if (!((m >> i) & 1u))
                continue;
            croot = dice_walk(t, root0, DICE_PROG[prog][(idx / seg) % 4u]);
            if (croot > root0 + 7 && croot - 12 >= lo)
                croot -= 12;
            n = dice_fold(dice_walk(t, croot, rb[i]) + 12 * oct[i], lo, hi);
            s->n = 1;
            s->note[0] = (uint8_t)n;
            if (role == 2u)
                s->n = (uint8_t)chord_notes(t, (uint32_t)n, s->note);
            s->time = ST_NOTE;
            s->vel = 100;
            for (c = 0; c < s->n; c++) {
                s->lvl |= (uint8_t)(lv[i] << (2u * c));
                s->rat |= (uint8_t)(rt[i] << (2u * c));
            }
            for (j = 1; j < hold[i] && idx + j < len; j++)
                t->step[idx + j].time = ST_TIE;
            if (sl[i] && idx + hold[i] < len)
                t->step[idx + hold[i] - 1u].flags = SF_SLIDE;
        }
    }
}

/* -------------------------------------------------------------- acid --- */
/* ACID: a 303 line. Ported from the TB-3PO generator of X0X (fm1-x0x, firmware/src/seq/tb3po.c) by Charles
 * Vestal, GPL-3.0-only: "A port of schwung-tb3po's generator (itself a port of the Phazerville Hemisphere
 * Suite TB_3PO applet, (c) djphazer and contributors, GPL-3.0)"; the applet Copyright (c) 2020 Logarhythm
 * (MIT). A step a note at the density (on the beat often the root), a degree of the scale (CHR: minor) in one
 * or two octaves up from the root nearest the register, an accent (HARD) or else a slide at their chance; a
 * slide into a rest goes. Here also: density, accent and slide rolled around TB-3PO's 70 / 40 / 25 %, a rest
 * after a note now and then a tie (the slide moves on to it), the notes not accented NORM or SOFT, no slide
 * out of LEN. */
static void dice_acid(track_t *t, uint32_t len)
{
    uint32_t mask = t->p[P_SCALE] ? scale_mask(t) : SCALE_MASK[2], degs = 0, deg, acc, iv, i, any = 0;
    uint32_t oct = dchance(80) ? 2u : 1u;
    int32_t base = dice.lo + ((t->p[P_ROOT] - dice.lo + 6) % 12 + 12) % 12 - 6;
    step_t *s;
    dice.acid[0] = (uint8_t)(55u + drnd(31));
    dice.acid[1] = (uint8_t)(25u + drnd(31));
    dice.acid[2] = (uint8_t)(15u + drnd(21));
    for (iv = 0; iv < 12u; iv++)
        degs += (mask >> iv) & 1u;
    for (i = 0; i < len; i++) {
        s = &t->step[i];
        memset(s, 0, sizeof *s);
        s->time = ST_REST;
        if (!dchance(dice.acid[0])) {
            if (i && s[-1].time != ST_REST && dchance(25)) {
                s->time = ST_TIE;
                s->flags = s[-1].flags;
                s[-1].flags = 0;
            }
            continue;
        }
        deg = i % 4u == 0u && dchance(35) ? 0u : drnd(degs);
        for (iv = 0; !((mask >> iv) & 1u) || deg--; iv++)   /* (the deg-th note of the scale) */
            ;
        s->n = 1;
        s->note[0] = (uint8_t)(base + (int32_t)iv + 12 * (int32_t)drnd(oct));
        s->time = ST_NOTE;
        s->vel = 100;
        acc = dchance(dice.acid[1]);
        s->flags = (uint8_t)(dchance(dice.acid[2]) ? SF_SLIDE : 0u);
        s->lvl = (uint8_t)(acc && !s->flags ? LV_HARD : (i & 1u) && dchance(30) ? LV_SOFT : LV_NORM);
        any = 1;
    }
    for (i = 0; i < len; i++)
        if (i + 1u >= len || t->step[i + 1u].time != ST_NOTE)
            t->step[i].flags = 0;
    if (!any) {
        s = &t->step[0];
        s->n = 1;
        s->note[0] = (uint8_t)base;
        s->time = ST_NOTE;
        s->vel = 100;
    }
}

/* ---------------------------------------------------------- the rolls --- */
static uint32_t dice_ctx(const track_t *t)       /* what a roll depends on: LEN, the key, the chords, the voice */
{
    return (uint32_t)(t->p[P_SLEN] & 127) | (uint32_t)(t->p[P_ROOT] & 15) << 7 | (uint32_t)(t->p[P_SCALE] & 31) << 11 |
           (uint32_t)(t->p[P_CHORD] & 7) << 16 | (uint32_t)(t->p[P_VOICE] & 3) << 19;
}
static uint32_t dice_depth(const track_t *t)     /* the rolls there are to turn back */
{
    return dice.n && dice.trk == trk_index(t) && dice.sum == pattern_sum(t) && dice.ctx == dice_ctx(t) ? dice.n : 0u;
}
static void dice_make(track_t *t, uint32_t style, uint32_t seed)
{
    uint32_t len = trk_len(t), i;
    dice_rs = (seed ^ seed >> 16) * 0x85EBCA77u;  /* (mixed: the streams of two rolls never overlap) */
    dice_rs = (dice_rs ^ dice_rs >> 13) * 0xC2B2AE3Du;
    dice_rs = dice_rs ? dice_rs : 0x9E3779B9u;
    t->rh_n = 0;                                  /* (a recorded note held: it ties no further) */
    for (i = 0; i < len; i++) {                   /* (the conditions, nudges and locks of the steps within LEN go) */
        t->cond[i] = CN_ALWAYS;
        lock_strip(t, i);
    }
    if (is_drum(t))
        dice_drums(t, style, len);
    else if (style == DS_ACID)
        dice_acid(t, len);
    else
        dice_synth(t, style, len);
    t->seq_active = 1;
}
/* a new pattern on track t in style (not DS_KIT; bass: its sound is a bass): the rolls there are to turn back */
static uint32_t dice_roll(track_t *t, uint32_t style, int bass)
{
    uint32_t p;
    if (!dice_depth(t)) {                         /* the first roll: the pattern there is, kept */
        memcpy(dice_was, t->step, sizeof dice_was);
        memcpy(dice_wcond, t->cond, sizeof dice_wcond);
        memcpy(dice_wmicro, t->micro, sizeof dice_wmicro);
        memcpy(dice_wlk, t->lock, sizeof dice_wlk);
        dice.n = 0;
        dice.trk = (uint8_t)trk_index(t);
        if (!is_drum(t))
            dice_role(t, bass);
    }
    if (dice.n == DICE_DEPTH) {                   /* full: the oldest roll goes (the pattern before them stays) */
        for (p = 1; p < DICE_DEPTH; p++) {
            dice.seed[p - 1u] = dice.seed[p];
            dice.style[p - 1u] = dice.style[p];
        }
        dice.n--;
    }
    p = dice.n;
    dice.seed[p] = rng();
    dice.style[p] = (uint8_t)style;
    dice_make(t, style, dice.seed[p]);
    dice.ctx = dice_ctx(t);
    dice.sum = pattern_sum(t);
    return ++dice.n;
}
static int dice_back(track_t *t)                  /* the roll before (the first: the pattern there was); 0 = none */
{
    uint32_t p;
    if (!dice_depth(t)) {
        dice.n = 0;
        return 0;
    }
    p = --dice.n;
    if (p) {
        dice_make(t, dice.style[p - 1u], dice.seed[p - 1u]);
    } else {
        memcpy(t->step, dice_was, sizeof dice_was);
        memcpy(t->cond, dice_wcond, sizeof dice_wcond);
        memcpy(t->micro, dice_wmicro, sizeof dice_wmicro);
        memcpy(t->lock, dice_wlk, sizeof dice_wlk);
    }
    dice.sum = pattern_sum(t);
    return 1;
}
