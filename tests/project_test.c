/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Host test of the project formats (firmware/src/project.c, -DPROJ_HOST part). Format 5 ("FUN5",
 * SLOOP 2.4: format 4 + per-step nudges, parameter locks and fill conditions) is written; format 4 ("FUN4", SLOOP 2.0:
 * 10-byte steps with levels and ratchets, the drum track's 16 lanes, P_CHORD), format 3 ("FUN3", SLOOP
 * 1.x), format 2 ("FUN2", 53 parameters per track) and format 1 ("FUN1"), built byte for byte as the
 * firmware stored them, convert: every old value at its parameter, the parameters added since at their
 * defaults, the swings onto the MPC scale (x 0.8), synth steps as they were, the drum track's notes onto
 * its lanes (accent: hard), globals, selection, the engine bytes (kept; the drum track's 0), no nudge, no
 * lock, no fill condition; damaged ones are refused. SLOOP 2.5 writes format 5 as SLOOP 2.4 does (its
 * parameters, P_E0_V24) and keeps its own parameters, globals and step conditions in the extension record
 * (PX_BYTES, in flash beside the project: storage.c st_save_x, here on a simulated NOR): round trips with
 * and without it, a SLOOP 2.4 project (no extension: defaults, its fill bits as conditions), the locks of
 * 2.5's parameters past 2.4's ids, the backup objects. Run by tests/run_tests.sh (needs build/gen). */
#define main hostsim_main
#include "hostsim.c"
#undef main
#define PROJ_HOST 1
static uint32_t trk_def_engine(uint32_t i)       /* ui.c TRK_DEF: ANALOG, DIGITAL, LOFI */
{
    static const uint8_t E[NPART] = {0, 1, 3};
    return i < NPART ? E[i] : 0u;
}
#include "../firmware/src/project.c"

static uint8_t nor[0x100000];                    /* the flash, simulated (storage.c) */
static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static int st_erase(uint32_t off) { memset(nor + off, 0xFF, 4096); return 0; }
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    for (i = 0; i < n; i++)
        nor[off + i] &= s[i];
    return 0;
}
#include "../firmware/src/storage.c"

static int check(const char *what, int ok)
{
    printf("%-66s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

/* the value parameter k (old id) of track t had in the old project */
static int16_t oldv(uint32_t t, uint32_t k) { return (int16_t)(t * 100u + k * 3u + 1u); }

static const uint8_t OLD_ENG[NTRK] = {7, 0, 6, 8};   /* WHEEL, ANALOG, TRIO; the drum track: 8 (none) */
static void fill_old_steps(step8_t *st, uint32_t t)
{
    uint32_t k;
    for (k = 0; k < NSTEP; k++) {
        step8_t *s = &st[k];
        s->note[0] = (uint8_t)(36u + (k + t) % 40u);
        s->note[1] = (uint8_t)(38u + k % 5u);
        s->n = (uint8_t)(k % 3u);
        s->time = (uint8_t)(k % 3u);
        s->flags = (uint8_t)(k & 3u);
        s->vel = (uint8_t)(64u + t);
    }
}
static void fill_v2_track(proj_trk_v2_t *d, uint32_t t)
{
    uint32_t k;
    for (k = 0; k < PROJ_NP_V2; k++)
        d->p[k] = oldv(t, k);
    d->engine = OLD_ENG[t];
    d->preset = (uint8_t)(t + 5u);
    fill_old_steps(d->step, t);
}
static void fill_v3_track(proj_trk_v3_t *d, uint32_t t)
{
    uint32_t k;
    for (k = 0; k < PROJ_NP_V3; k++)
        d->p[k] = oldv(t, k);
    d->p[P_SSWING] = 50;                           /* (swings: within 0..100) */
    d->p[P_ASWING] = 100;
    d->engine = OLD_ENG[t];
    d->preset = (uint8_t)(t + 5u);
    fill_old_steps(d->step, t);
}

/* the steps of a converted track against the old ones: synth as they were, drums onto lanes */
static int steps_ok(const proj_trk_t *n, const step8_t *o, int drum)
{
    uint32_t k, i;
    for (k = 0; k < NSTEP; k++) {
        if (drum) {
            const dstep_t *d = &n->dstep[k];
            uint32_t want = 0;
            if (o[k].time == ST_NOTE)
                for (i = 0; i < o[k].n; i++)
                    want |= 1u << lane_of_note(o[k].note[i]);
            if (dstep_mask(d) != want)
                return 0;
            for (i = 0; i < DRUM_LANES; i++)
                if ((want >> i) & 1u && dstep_lvl(d, i) != ((o[k].flags & SF_ACCENT) ? LV_HARD : vel_lvl(o[k].vel)))
                    return 0;
        } else {
            const step_t *s = &n->step[k];
            if (memcmp(s->note, o[k].note, 4) || s->n != o[k].n || s->time != o[k].time || s->flags != o[k].flags ||
                s->vel != o[k].vel || s->lvl || s->rat)
                return 0;
        }
    }
    return 1;
}

/* track t converted from format 2 / 1 has the old values where they belong */
static int track_ok_v2(const proj_trk_t *n, const proj_trk_v2_t *o, uint32_t t)
{
    uint32_t k;
    int ok = (t == TRK_DRUM ? n->engine == 0 && n->preset == 0 : n->engine == o->engine && n->preset == o->preset) &&
             steps_ok(n, o->step, t == TRK_DRUM);
    for (k = 0; k <= P_DETUNE; k++)
        if (k != P_SSWING && k != P_ASWING)
            ok &= n->p[k] == oldv(t, k);
    ok &= n->p[P_SLCR] == 0 && n->p[P_SLPAT] == TP[P_SLPAT].def && n->p[P_SLRATE] == TP[P_SLRATE].def &&
          n->p[P_SLDEPTH] == TP[P_SLDEPTH].def && n->p[P_CHORD] == 0;
    for (k = 0; k < 8u; k++)
        ok &= n->p[P_E0_V24 + k] == oldv(t, 45u + k);
    return ok;
}

int main(void)
{
    static project_v3_t v3;
    static project_v2_t v2;
    static project_v1_t v1;
    static project_v4_t v4;
    static project_t q, q2;
    static union {
        project_t v5;
        project_v4_t v4;
        project_v3_t v3;
        project_v2_t v2;
        project_v1_t v1;
    } buf;
    static uint8_t px[PX_BYTES], px2[PX_BYTES], kb[PX_KEYED];
    uint32_t i, t;
    int bad = 0, ok;

    bad += check("layout: P_CHORD, P_TFLT, P_STRUM, P_VLEAD (SLOOP 2.4: P_E0 53), P_COUNT 2.4 = format 4's + 3",
                 P_CHORD + 1 == P_TFLT && P_TFLT + 1 == P_STRUM && P_STRUM + 1 == P_VLEAD && P_VLEAD + 1 == P_E0_V24 &&
                 P_E0_V24 == 53 && P_NP_V24 == PROJ_NP_V4 + 3u && PROJ_NP_V4 == PROJ_NP_V3 + 1u && P_SLDEPTH + 1 == P_CHORD);
    bad += check("layout: SLOOP 2.5's arp rhythm, ROT..DEJA, SHIFT, CYC, TURN, COLOR, then P_E0 (67), P_COUNT 75",
                 P_VLEAD + 1 == P_AACC && P_ARAT + 1 == P_AROT && P_ADEJA + 1 == P_ASHIFT && P_ACYC + 1 == P_TURN &&
                 P_TURN + 1 == P_COLOR && P_COLOR + 3 == P_E0 && P_E0 == 67 && P_COUNT == 75 && P_NX == 14u);
    bad += check("layout: EVOL, BACK, AFILL, PROG after NEW (2.4's G_COUNT 32)", G_NEWPRJ + 1 == G_EVOL && G_EVOL + 1 == G_EVBK &&
                 G_EVBK + 1 == G_AFILL && G_AFILL + 1 == G_PROG && G_PROG + 1 == G_COUNT && G_NG_V24 == 32u && G_NX == 4u);
    bad += check("format 5 fits one flash object; 4 slots fit .noinit (with panel, settings, dbg, bootguard)",
                 sizeof(project_t) <= 3840u && 4u * sizeof(project_t) <= 0x3D50u - 256u && sizeof(project_t) == 3840u);   /* (full: a parameter more needs a new layout) */
    bad += check("the extension record: 4 + 56 parameters + 4 globals + 256 conditions x 5 bits = 224 (a header page)",
                 PX_BYTES == 224u && PX_BYTES == ST_X_MAX && PX_KEYED == 228u);

    /* format 3 (SLOOP 1.x) */
    memset(&v3, 0, sizeof v3);
    v3.magic = PROJ_MAGIC_V3;
    v3.size = sizeof v3;
    for (i = 0; i < PROJ_NG_V3; i++)
        v3.g[i] = (int16_t)(300 + i);
    v3.g[G_SWING] = 50;
    v3.sel = 3;
    for (t = 0; t < NTRK; t++)
        fill_v3_track(&v3.t[t], t);
    v3.sum = proj_hash(&v3, sizeof v3 - 4u);
    memcpy(&buf, &v3, sizeof v3);
    ok = proj_import(&q, &buf, (int)sizeof v3);
    bad += check("FUN3 -> FUN5: converted, valid format 5 slot", ok && proj_ok(&q) && q.magic == PROJ_MAGIC);
    ok = q.sel == 3 && q.g[G_SWING] == 40;
    for (i = 0; i < PROJ_NG_V3; i++)
        ok &= i == G_SWING || q.g[i] == (int16_t)(300 + i);
    for (i = PROJ_NG_V3; i < PROJ_NG_V5; i++)
        ok &= q.g[i] == GP[i].def;
    bad += check("FUN3 -> FUN5: globals (swing 50 -> 40: the MPC scale), the new ones default", ok);
    ok = 1;
    for (t = 0; t < NTRK; t++) {
        const proj_trk_t *n = &q.t[t];
        uint32_t k;
        ok &= (t == TRK_DRUM ? n->engine == 0 : n->engine == OLD_ENG[t]) && steps_ok(n, v3.t[t].step, t == TRK_DRUM);
        for (k = 0; k < PROJ_NP_V3 - 8u; k++)
            if (k != P_SSWING && k != P_ASWING)
                ok &= n->p[k] == oldv(t, k);
        ok &= n->p[P_SSWING] == 40 && n->p[P_ASWING] == 80 && n->p[P_CHORD] == 0;
        for (k = 0; k < 8u; k++)
            ok &= n->p[P_E0_V24 + k] == oldv(t, PROJ_NP_V3 - 8u + k);
        for (k = 0; k < NLOCK; k++)
            ok &= n->lock[k].step == LOCK_FREE;
        for (k = 0; k < NSTEP; k++)
            ok &= n->micro[k] == 0;
        for (k = 0; k < NSTEP / 4u; k++)
            ok &= n->fill[k] == 0;
    }
    bad += check("FUN3 -> FUN5: parameters (P_E0.. moved), steps, drum notes -> lanes, no lock, no fill", ok);

    /* format 2, as written before the SLICER */
    memset(&v2, 0, sizeof v2);
    v2.magic = PROJ_MAGIC_V2;
    v2.size = sizeof v2;
    for (i = 0; i < PROJ_NG_V2; i++)
        v2.g[i] = (int16_t)(500 + i);
    v2.sel = 2;
    for (t = 0; t < NTRK; t++)
        fill_v2_track(&v2.t[t], t);
    v2.sum = proj_hash(&v2, sizeof v2 - 4u);
    bad += check("FUN2 image is 2552 bytes (as stored)", sizeof v2 == 2552u);
    memcpy(&buf, &v2, sizeof v2);
    ok = proj_import(&q, &buf, (int)sizeof v2);
    bad += check("FUN2 -> FUN5: converted, valid format 5 slot", ok && proj_ok(&q) && q.magic == PROJ_MAGIC);
    ok = q.sel == 2;
    for (i = 0; i < PROJ_NG_V2; i++)
        ok &= i == G_SWING || q.g[i] == (int16_t)(500 + i);
    bad += check("FUN2 -> FUN5: globals and selected track", ok);
    ok = 1;
    for (t = 0; t < NTRK; t++)
        ok &= track_ok_v2(&q.t[t], &v2.t[t], t);
    bad += check("FUN2 -> FUN5: every parameter mapped, SLICER OFF, CHORD OFF (4 tracks)", ok);
    bad += check("FUN2 -> FUN5: engine bytes kept (WHEEL 7, ANALOG 0, TRIO 6), drum 0",
                 q.t[0].engine == 7 && q.t[1].engine == 0 && q.t[2].engine == 6 && q.t[3].engine == 0 &&
                 str_eq(ENGINES[7]->name, "WHEEL") && str_eq(ENGINES[6]->name, "TRIO") && NENGINES > 8);

    /* a FUN4 image (SLOOP 2.0 .. 2.3, as stored) -> FUN5: the same, zero nudges, every lock free */
    memset(&v4, 0, sizeof v4);
    v4.magic = PROJ_MAGIC_V4;
    v4.size = sizeof v4;
    memcpy(v4.g, q.g, sizeof v4.g);
    v4.sel = 1;
    for (t = 0; t < NTRK; t++) {
        for (i = 0; i < PROJ_NP_V4; i++)               /* (format 4's ids: P_E0 was P_TFLT) */
            v4.t[t].p[i] = i < P_TFLT ? q.t[t].p[i] : q.t[t].p[P_E0_V24 + i - P_TFLT];
        v4.t[t].engine = q.t[t].engine;
        v4.t[t].preset = q.t[t].preset;
        memcpy(v4.t[t].step, q.t[t].step, sizeof v4.t[t].step);
    }
    v4.t[1].engine = 8;
    v4.t[0].step[3].lvl = 0x9C;
    v4.t[0].step[3].rat = 0x27;
    dstep_set(&v4.t[TRK_DRUM].dstep[5], 13, LV_GHOST, 2);
    v4.sum = proj_hash(&v4, sizeof v4 - 4u);
    bad += check("FUN4 image is 3112 bytes (as stored)", sizeof v4 == 3112u);
    memcpy(&buf, &v4, sizeof v4);
    ok = proj_import(&q2, &buf, (int)sizeof v4) && proj_ok(&q2) && q2.magic == PROJ_MAGIC && q2.sel == 1 && q2.t[1].engine == 8 &&
         q2.t[0].step[3].lvl == 0x9C && q2.t[0].step[3].rat == 0x27 && dstep_lvl(&q2.t[TRK_DRUM].dstep[5], 13) == LV_GHOST &&
         dstep_rat(&q2.t[TRK_DRUM].dstep[5], 13) == 2u && !memcmp(q2.g, v4.g, sizeof q2.g);
    for (t = 0; t < NTRK; t++) {
        ok &= !memcmp(q2.t[t].p, v4.t[t].p, P_TFLT * 2u) && !memcmp(q2.t[t].p + P_E0_V24, v4.t[t].p + P_TFLT, 16u) &&
              q2.t[t].p[P_TFLT] == 0 && q2.t[t].p[P_STRUM] == 0 && q2.t[t].p[P_VLEAD] == 0 &&
              !memcmp(q2.t[t].step, v4.t[t].step, sizeof q2.t[t].step);
        for (i = 0; i < NLOCK; i++)
            ok &= q2.t[t].lock[i].step == LOCK_FREE;
        for (i = 0; i < NSTEP; i++)
            ok &= q2.t[t].micro[i] == 0;
        for (i = 0; i < NSTEP / 4u; i++)
            ok &= q2.t[t].fill[i] == 0;
    }
    bad += check("FUN4 -> FUN5: as stored (levels, ratchets, lanes, engine 8; P_E0.. moved, FILTER off), zero nudges, locks free, no fill condition", ok);
    v4.t[2].step[1].vel ^= 1u;
    memcpy(&buf, &v4, sizeof v4);
    bad += check("FUN4 with a bad checksum: refused", !proj_import(&q2, &buf, (int)sizeof v4));
    v4.t[2].step[1].vel ^= 1u;

    /* a FUN5 round trip: stored as is (nudges, locks, an engine added since: 8) */
    q.t[1].engine = 8;
    q.t[0].step[3].lvl = 0x9C;
    q.t[0].step[3].rat = 0x27;
    dstep_set(&q.t[TRK_DRUM].dstep[5], 13, LV_GHOST, 2);
    q.t[0].micro[3] = -32;
    q.t[0].micro[4] = 31;
    q.t[TRK_DRUM].micro[9] = -7;
    q.t[0].lock[0].step = 3, q.t[0].lock[0].param = P_E0_V24, q.t[0].lock[0].val = 2;
    q.t[0].lock[1].step = 3, q.t[0].lock[1].param = P_DIST, q.t[0].lock[1].val = 100;
    q.t[2].lock[23].step = 63, q.t[2].lock[23].param = P_LEVEL, q.t[2].lock[23].val = 50;
    q.t[2].p[P_TFLT] = -30;                        /* a track FILTER (2.4) */
    q.t[0].fill[0] = 0x09;                         /* steps 1 and 2: FILL ONLY, NO FILL */
    q.t[TRK_DRUM].fill[15] = 0x40;                 /* step 64: FILL ONLY */
    q.sum = proj_sum(&q);
    memcpy(&buf, &q, sizeof q);
    bad += check("FUN5 -> FUN5: as stored (levels, ratchets, lanes, nudges, locks, fill conditions, engine 8)",
                 proj_import(&q2, &buf, (int)sizeof q) && !memcmp(&q, &q2, sizeof q) && q2.t[1].engine == 8 &&
                 q2.t[0].micro[3] == -32 && q2.t[0].lock[1].val == 100 && q2.t[2].lock[23].step == 63 &&
                 q2.t[0].fill[0] == 0x09 && q2.t[TRK_DRUM].fill[15] == 0x40 && q2.t[2].p[P_TFLT] == -30);

    /* damaged / wrong size */
    v2.t[1].p[3]++;
    memcpy(&buf, &v2, sizeof v2);
    bad += check("FUN2 with a bad checksum: refused", !proj_import(&q2, &buf, (int)sizeof v2));
    v2.t[1].p[3]--;
    memcpy(&buf, &v2, sizeof v2);
    bad += check("FUN2 with a wrong length: refused", !proj_import(&q2, &buf, (int)sizeof v2 - 2));
    memcpy(&buf, &q, sizeof q);
    buf.v4.magic = PROJ_MAGIC_V3;
    bad += check("FUN5 size with a FUN3 magic: refused", !proj_import(&q2, &buf, (int)sizeof q));
    memcpy(&buf, &q, sizeof q);
    buf.v5.magic = PROJ_MAGIC_V4;
    bad += check("FUN5 size with a FUN4 magic: refused", !proj_import(&q2, &buf, (int)sizeof q));
    memcpy(&buf, &v3, sizeof v3);
    buf.v3.t[2].step[7].vel ^= 1u;
    bad += check("FUN3 with a bad checksum: refused", !proj_import(&q2, &buf, (int)sizeof v3));

    /* format 1: one instrument -> track 1, the others their defaults */
    memset(&v1, 0, sizeof v1);
    v1.magic = PROJ_MAGIC_V1;
    v1.size = sizeof v1;
    for (i = 0; i < PROJ_NG_V2; i++)
        v1.g[i] = (int16_t)(700 + i);
    fill_v2_track(&v1.t, 0);
    v1.sum = proj_hash(&v1, sizeof v1 - 4u);
    memcpy(&buf, &v1, sizeof v1);
    ok = proj_import(&q, &buf, (int)sizeof v1) && proj_ok(&q) && track_ok_v2(&q.t[0], &v1.t, 0) && q.g[5] == 705;
    for (t = 1; t < NTRK; t++)
        ok &= q.t[t].preset == 0xFF && q.t[t].p[P_SLCR] == 0 && q.t[t].p[P_LEVEL] == TP[P_LEVEL].def &&
              q.t[t].p[P_E0_V24] == ENGINES[trk_def_engine(t)]->edit[0].def && q.t[t].lock[0].step == LOCK_FREE &&
              (t == TRK_DRUM ? dstep_mask(&q.t[t].dstep[0]) == 0u : q.t[t].step[0].time == ST_REST);
    bad += check("FUN1 -> FUN5: track 1 mapped, tracks 2..4 defaults", ok);

    /* capture / apply: the working project round trip (nudges and locks too) */
    host_tracks_init();
    for (t = 0; t < NTRK; t++)
        trk[t].p[P_SLEN] = (int16_t)(5 + t);
    trk[1].step[2].n = 2, trk[1].step[2].note[0] = 60, trk[1].step[2].note[1] = 64, trk[1].step[2].time = ST_NOTE;
    trk[1].step[2].lvl = 0x0D;
    dstep_set(&TDRUM->dstep[9], 4, LV_SOFT, 1);
    song.g[G_DUST] = 33;
    trk[1].micro[2] = -20;
    TDRUM->micro[9] = 12;
    lock_set(&trk[1], 2, P_ED_FLT, -30);
    lock_set(&trk[1], 2, P_E1, 5);
    lock_set(TDRUM, 9, P_DIST, 64);
    step_fill_set(&trk[1], 2, FC_FILL);
    step_fill_set(TDRUM, 9, FC_NOFILL);
    step_fill_set(TDRUM, 63, FC_FILL);
    proj_capture(&q, px);
    host_tracks_init();
    proj_apply(&q, px, 1);
    ok = trk[2].p[P_SLEN] == 7 && trk[1].step[2].n == 2 && trk[1].step[2].lvl == 0x0D && song.g[G_DUST] == 33 &&
         dstep_has(&TDRUM->dstep[9], 4) && dstep_lvl(&TDRUM->dstep[9], 4) == LV_SOFT && dstep_rat(&TDRUM->dstep[9], 4) == 1u &&
         trk[1].micro[2] == -20 && TDRUM->micro[9] == 12 && trk[1].micro[3] == 0 &&
         lock_find(&trk[1], 2, P_ED_FLT, 0) >= 0 && trk[1].lock[lock_find(&trk[1], 2, P_ED_FLT, 0)].val == -30 &&
         lock_find(&trk[1], 2, P_E1, 0) >= 0 && lock_find(TDRUM, 9, P_DIST, 0) >= 0 && lock_find(TDRUM, 9, P_E0, 0) < 0 &&
         step_fill(&trk[1], 2) == FC_FILL && step_fill(&trk[1], 3) == FC_NORM && step_fill(TDRUM, 9) == FC_NOFILL &&
         step_fill(TDRUM, 63) == FC_FILL && step_fill(TDRUM, 8) == FC_NORM;
    bad += check("the working project: capture -> apply round trip (levels, lanes, DUST, nudges, locks, fill conditions)", ok);
    /* a damaged image: a nudge out of range, a lock on a parameter that cannot lock, on a step past the end,
     * with a value past the range: clamped, freed, freed, clamped */
    q.t[1].micro[7] = 100;
    q.t[1].micro[8] = -100;
    q.t[1].lock[5].step = 4, q.t[1].lock[5].param = P_SLEN, q.t[1].lock[5].val = 8;
    q.t[1].lock[6].step = 64, q.t[1].lock[6].param = P_E0_V24, q.t[1].lock[6].val = 1;
    q.t[1].lock[7].step = 4, q.t[1].lock[7].param = P_LEVEL, q.t[1].lock[7].val = 999;
    q.t[1].lock[8].step = 4, q.t[1].lock[8].param = 200, q.t[1].lock[8].val = 1;
    q.t[1].fill[1] = 0xF9;                         /* steps 5, 6: fill only, no fill; 7, 8: 3 (-> normal) */
    proj_apply(&q, 0, 1);                          /* (no extension: the fill bits are the conditions) */
    ok = trk[1].micro[7] == MICRO_MAX && trk[1].micro[8] == MICRO_MIN && trk[1].lock[5].step == LOCK_FREE &&
         trk[1].lock[6].step == LOCK_FREE && trk[1].lock[8].step == LOCK_FREE && trk[1].lock[7].step == 4 &&
         trk[1].lock[7].val == 127 && lock_find(&trk[1], 2, P_ED_FLT, 0) >= 0 &&
         step_fill(&trk[1], 4) == FC_FILL && step_fill(&trk[1], 5) == FC_NOFILL && trk[1].cond[6] == CN_ALWAYS &&
         trk[1].cond[7] == CN_ALWAYS;
    bad += check("apply: a nudge past the range is clamped, a lock on LEN / step 64 / param 200 is freed, LEVEL 999 -> 127, condition 3 -> normal", ok);


    /* ---- SLOOP 2.5: the extension record */
    ok = 1;
    for (i = P_E0_V24; i < P_E0; i++)
        ok &= TP[i].min >= -128 && TP[i].max <= 127;
    for (i = G_NG_V24; i < G_COUNT; i++)
        ok &= GP[i].min >= -128 && GP[i].max <= 127;
    bad += check("extension: SLOOP 2.5's parameters and globals fit a byte each", ok);
    {   /* the conditions, 5 bits each: every value at every place, the neighbours untouched */
        static uint8_t c[PX_COND];
        uint32_t k;
        ok = 1;
        memset(c, 0, sizeof c);
        for (k = 0; k < NTRK * NSTEP; k++)
            px_cond_set(c, k, (k * 7u + 3u) % CN_COUNT);
        for (k = 0; k < NTRK * NSTEP; k++)
            ok &= px_cond(c, k) == (k * 7u + 3u) % CN_COUNT;
        px_cond_set(c, 100, 31);
        ok &= px_cond(c, 100) == 31 && px_cond(c, 99) == (99u * 7u + 3u) % CN_COUNT && px_cond(c, 101) == (101u * 7u + 3u) % CN_COUNT;
        bad += check("extension: conditions packed 5 bits each (every value, every place, neighbours kept)", ok);
    }

    /* the working project with SLOOP 2.5's parameters, globals, every kind of condition, locks of COLOR / SLICER */
    host_tracks_init();
    host_preset(&trk[0], 0, 0);
    trk[0].p[P_AACC] = 4, trk[0].p[P_ASHIFT] = -7, trk[0].p[P_ACYC] = 8, trk[0].p[P_ADEJA] = 127;
    trk[1].p[P_TURN] = 35, trk[2].p[P_COLOR] = 4, trk[2].p[P_CAMT] = 99, trk[2].p[P_CRATE] = 57;
    TDRUM->p[P_COLOR] = 1, TDRUM->p[P_TURN] = 100;
    song.g[G_EVOL] = 2, song.g[G_EVBK] = 3, song.g[G_AFILL] = 6, song.g[G_PROG] = 9;
    for (t = 0; t < NTRK; t++)
        for (i = 0; i < NSTEP; i++)
            trk[t].cond[i] = (uint8_t)((t * 5u + i) % CN_COUNT);
    ok = lock_set(&trk[2], 9, P_COLOR, 3) && lock_set(&trk[2], 9, P_CRATE, 70) && lock_set(&trk[2], 9, P_E7, 5) &&
         lock_set(TDRUM, 2, P_SLDEPTH, 64) && lock_set(TDRUM, 3, P_CAMT, 11) && lock_set(TDRUM, 3, P_SLCR, 2) &&
         lock_set(&trk[0], 4, P_ED_FLT, 30);
    bad += check("locks: COLOR (TYPE, AMT, RATE) and the drum track's SLICER and COLOR lock; TURN, the arp do not",
                 ok && p_lockable(P_COLOR) && p_lockable(P_CAMT) && p_lockable(P_CRATE) && p_lockable(P_SLCR) &&
                 !p_lockable(P_TURN) && !p_lockable(P_AACC) && !lock_set(&trk[0], 4, P_TURN, 5));
    proj_capture(&q, px);
    {   /* format 5 as SLOOP 2.4 has it: its parameters at its ids, the locks of 2.5's past them, FILL / !FILL as fill bits */
        int kc = lock_find(&trk[2], 9, P_COLOR, 0), ke = lock_find(&trk[2], 9, P_E7, 0), kd = lock_find(TDRUM, 2, P_SLDEPTH, 0);
        ok = proj_ok(&q) && kc >= 0 && ke >= 0 && kd >= 0 && q.t[2].p[P_E0_V24 + 7] == trk[2].p[P_E7] &&
             q.t[2].p[P_VLEAD] == trk[2].p[P_VLEAD] && q.t[2].lock[kc].param == P_NP_V24 + (P_COLOR - P_E0_V24) &&
             q.t[2].lock[ke].param == P_E0_V24 + 7u && q.t[TRK_DRUM].lock[kd].param == P_SLDEPTH;
        for (t = 0; t < NTRK; t++)
            for (i = 0; i < NSTEP; i++) {
                uint32_t c = (t * 5u + i) % CN_COUNT, f = (q.t[t].fill[i / 4u] >> (2u * (i % 4u))) & 3u;
                ok &= f == (c == CN_FILL ? FC_FILL : c == CN_NFILL ? FC_NOFILL : FC_NORM);
            }
        bad += check("capture: format 5 as SLOOP 2.4's (its ids; 2.5's locks past them; FILL, !FILL as fill bits)", ok);
    }
    host_tracks_init();
    song.g[G_EVOL] = song.g[G_EVBK] = song.g[G_AFILL] = song.g[G_PROG] = 0;
    proj_apply(&q, px, 1);
    ok = trk[0].p[P_AACC] == 4 && trk[0].p[P_ASHIFT] == -7 && trk[0].p[P_ACYC] == 8 && trk[0].p[P_ADEJA] == 127 &&
         trk[1].p[P_TURN] == 35 && trk[2].p[P_COLOR] == 4 && trk[2].p[P_CAMT] == 99 && trk[2].p[P_CRATE] == 57 &&
         TDRUM->p[P_COLOR] == 1 && TDRUM->p[P_TURN] == 100 && song.g[G_EVOL] == 2 && song.g[G_EVBK] == 3 &&
         song.g[G_AFILL] == 6 && song.g[G_PROG] == 9;
    for (t = 0; t < NTRK; t++)
        for (i = 0; i < NSTEP; i++)
            ok &= trk[t].cond[i] == (t * 5u + i) % CN_COUNT;
    {
        int k = lock_find(&trk[2], 9, P_COLOR, 0);
        ok &= k >= 0 && trk[2].lock[k].val == 3 && lock_find(&trk[2], 9, P_CRATE, 0) >= 0 && lock_find(&trk[2], 9, P_E7, 0) >= 0 &&
              lock_find(TDRUM, 2, P_SLDEPTH, 0) >= 0 && lock_find(TDRUM, 3, P_CAMT, 0) >= 0 && lock_find(TDRUM, 3, P_SLCR, 0) >= 0;
    }
    bad += check("extension: capture -> apply round trip (2.5's parameters, globals, every condition, COLOR / SLICER locks)", ok);

    /* the same project as SLOOP 2.4 sees it: no extension */
    host_tracks_init();
    proj_apply(&q, 0, 1);
    ok = trk[0].p[P_AACC] == TP[P_AACC].def && trk[2].p[P_COLOR] == 0 && trk[1].p[P_TURN] == 0 && song.g[G_PROG] == GP[G_PROG].def &&
         song.g[G_EVOL] == GP[G_EVOL].def && trk[2].p[P_E7] == q.t[2].p[P_E0_V24 + 7];
    for (t = 0; t < NTRK; t++)
        for (i = 0; i < NSTEP; i++) {
            uint32_t c = (t * 5u + i) % CN_COUNT;
            ok &= trk[t].cond[i] == (c == CN_FILL || c == CN_NFILL ? c : CN_ALWAYS);
        }
    ok &= lock_find(&trk[2], 9, P_E7, 0) >= 0 && lock_find(&trk[2], 9, P_COLOR, 0) >= 0;   /* (the locks are the project's) */
    bad += check("no extension (as SLOOP 2.4 saves): 2.5's parameters default, conditions from the fill bits", ok);
    memcpy(px2, px, PX_BYTES);
    px2[0] = PX_VER + 1u;                          /* another version: as none */
    host_tracks_init();
    proj_apply(&q, px2, 1);
    ok = trk[2].p[P_COLOR] == 0 && trk[0].p[P_AACC] == TP[P_AACC].def;
    memcpy(px2, px, PX_BYTES);
    px2[4u + 2u * P_NX + (P_COLOR - P_E0_V24)] = 99;   /* out of range: clamped */
    px_cond_set(px2 + 4u + NTRK * P_NX + G_NX, 5, 30);  /* no such condition: ALWAYS */
    proj_apply(&q, px2, 1);
    ok &= trk[2].p[P_COLOR] == TP[P_COLOR].max && trk[0].cond[5] == CN_ALWAYS;
    bad += check("extension of another version: defaults; a value past its range clamped, a bad condition ALWAYS", ok);

    /* a project SLOOP 2.4 made: its fill bits are our conditions (FILL, !FILL), its lock ids its own */
    {
        static project_t u;
        memcpy(&u, &q, sizeof u);
        memset(u.t[0].fill, 0, sizeof u.t[0].fill);
        u.t[0].fill[0] = 0x09;                     /* steps 1, 2: FILL ONLY, NO FILL */
        for (i = 0; i < NLOCK; i++)
            u.t[0].lock[i].step = LOCK_FREE;
        u.t[0].lock[0].step = 5, u.t[0].lock[0].param = P_E0_V24, u.t[0].lock[0].val = 1;   /* EDIT 1 (2.4's id 53) */
        u.t[0].lock[1].step = 6, u.t[0].lock[1].param = P_NP_V24 + 9u, u.t[0].lock[1].val = 1;   /* (2.5's CYC: cannot lock) */
        u.sum = proj_sum(&u);
        host_tracks_init();
        ok = proj_ok(&u);
        proj_apply(&u, 0, 1);
        ok &= trk[0].cond[0] == CN_FILL && trk[0].cond[1] == CN_NFILL && trk[0].cond[2] == CN_ALWAYS &&
              lock_find(&trk[0], 5, P_E0, 0) >= 0 && trk[0].lock[1].step == LOCK_FREE;
        bad += check("a SLOOP 2.4 project: FILL / NO FILL -> conditions, its lock ids mapped (P_E0 53 -> 67)", ok);
    }

    /* in flash: the record in the project's header page; SLOOP 2.4 saving over it leaves none */
    memset(nor, 0xFF, sizeof nor);
    {
        static union { project_t v5; project_v4_t v4; } lb;
        static uint8_t lx[ST_X_MAX];
        uint32_t xn = 0;
        st_hdr_t h;
        int n;
        ok = st_save_x(OBJ_PROJECT0 + 1, &q, sizeof q, px, PX_BYTES) == 0;
        n = st_load_x(OBJ_PROJECT0 + 1, &lb, sizeof lb, lx, sizeof lx, &xn);
        ok &= n == (int)sizeof q && xn == PX_BYTES && !memcmp(&lb.v5, &q, sizeof q) && !memcmp(lx, px, PX_BYTES);
        ok &= st_load(OBJ_PROJECT0 + 1, &lb, sizeof lb) == (int)sizeof q && !memcmp(&lb.v5, &q, sizeof q);   /* (2.4 reads it) */
        bad += check("flash: project + extension saved and loaded together (2.4's st_load reads the project)", ok);
        ok = st_save(OBJ_PROJECT0 + 1, &q, sizeof q) == 0 &&                /* SLOOP 2.4 saves the slot */
             st_load_x(OBJ_PROJECT0 + 1, &lb, sizeof lb, lx, sizeof lx, &xn) == (int)sizeof q && xn == 0u;
        ok &= st_save_x(OBJ_PROJECT0 + 1, &q, sizeof q, px, PX_BYTES) == 0 &&
              st_load_x(OBJ_PROJECT0 + 1, &lb, sizeof lb, lx, sizeof lx, &xn) == (int)sizeof q && xn == PX_BYTES;
        n = st_current(OBJ_PROJECT0 + 1, &h);
        nor[st_sector(OBJ_PROJECT0 + 1, (uint32_t)n) + ST_X_OFF + 7] ^= 0x10;   /* its record damaged: none */
        ok &= st_load_x(OBJ_PROJECT0 + 1, &lb, sizeof lb, lx, sizeof lx, &xn) == (int)sizeof q && xn == 0u;
        bad += check("flash: saved over by SLOOP 2.4: no extension; a damaged one: none (the project still loads)", ok);
    }

    /* backup objects (editor.c 9..13): the record with its project's sum; restored onto that project only */
    ok = px_keyed(&q, px, kb) == PX_KEYED && px_unkey(&q, kb, PX_KEYED) == kb + 4 && !memcmp(kb + 4, px, PX_BYTES);
    q.t[0].p[P_LEVEL]++;                           /* another project now in the slot */
    q.sum = proj_sum(&q);
    ok &= !px_unkey(&q, kb, PX_KEYED) && !px_unkey(&q, kb, PX_KEYED - 1u);
    memcpy(px2, px, PX_BYTES);
    px2[0] = 0;
    ok &= !px_keyed(&q, px2, kb);                  /* (no record: no object) */
    bad += check("backup: the extension object carries its project's sum; another project refuses it", ok);

    printf("%s\n", bad ? "PROJECT FORMAT TEST FAILED" : "project format test passed");
    return bad != 0;
}
