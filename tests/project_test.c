/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Host test of the project formats (firmware/src/project.c, -DPROJ_HOST part). Format 8 ("FUN8",
 * SLOOP 2.5: 10-byte steps with levels and ratchets, the drum track's 16 lanes, the arp rhythm, ROT /
 * SYNC / RHYM / DEJA, SHIFT / CYC, TURN, a condition per step, the parameter locks, checked on load) is written; format 7
 * ("FUN7", SLOOP 2.5 before TURN), format 6 ("FUN6", SLOOP 2.5 before SHIFT), format 5 ("FUN5", SLOOP 2.4), format 4 ("FUN4", SLOOP 2.0..2.3), format 3 ("FUN3", SLOOP 1.x), format 2 ("FUN2", 53 parameters per track) and format 1 ("FUN1"), built
 * byte for byte as the firmware stored them, convert: every old value at its parameter, the parameters
 * added since at their defaults, the swings onto the MPC scale (x 0.8), synth steps as they were, the
 * drum track's notes onto its lanes (accent: hard), globals, selection, the engine bytes (kept; the
 * drum track's 0); damaged ones are refused. Run by tests/run_tests.sh (needs build/gen). */
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
        ok &= n->p[P_E0 + k] == oldv(t, 45u + k);
    return ok;
}

int main(void)
{
    static project_v3_t v3;
    static project_v2_t v2;
    static project_v1_t v1;
    static project_t q, q2;
    static union {
        project_t v8;
        project_v7_t v7;
        project_v6_t v6;
        project_v5_t v5;
        project_v4_t v4;
        project_v3_t v3;
        project_v2_t v2;
        project_v1_t v1;
    } buf;
    uint32_t i, t;
    int bad = 0, ok;

    bad += check("layout: P_CHORD, the arp rhythm, ROT..DEJA, SHIFT, CYC, TURN, P_E0 (61); P_COUNT = format 7's + 1",
                 P_CHORD + 1 == P_AACC && P_ARAT + 1 == P_AROT && P_ADEJA + 1 == P_ASHIFT && P_ACYC + 1 == P_TURN &&
                 P_TURN + 1 == P_E0 && P_E0 == 61 && P_COUNT == 69 && P_COUNT == PROJ_NP_V7 + 1u &&
                 P_COUNT == PROJ_NP_V6 + 3u && P_COUNT == PROJ_NP_V5 + 7u && P_COUNT == PROJ_NP_V4 + 11u && P_SLDEPTH + 1 == P_CHORD);
    bad += check("format 6: format 5's + 32 (ROT..DEJA) + a condition per step and track + the locks, then the sum",
                 sizeof(project_v6_t) == sizeof(project_v5_t) + 32u + NTRK * NSTEP + 4u * PLK_MAX &&
                 __builtin_offsetof(project_v6_t, cond) + NTRK * NSTEP == __builtin_offsetof(project_v6_t, lk) &&
                 __builtin_offsetof(project_v6_t, lk) + 4u * PLK_MAX + 4u == sizeof(project_v6_t));
    bad += check("format 7: format 6's + 16 (SHIFT, CYC), the conditions and the locks, then the sum",
                 sizeof(project_v7_t) == sizeof(project_v6_t) + 16u &&
                 __builtin_offsetof(project_v7_t, cond) + NTRK * NSTEP == __builtin_offsetof(project_v7_t, lk) &&
                 __builtin_offsetof(project_v7_t, lk) + 4u * PLK_MAX + 4u == sizeof(project_v7_t));
    bad += check("format 8: format 7's + 8 (TURN), the conditions and the locks, then the sum",
                 sizeof(project_t) == sizeof(project_v7_t) + 8u &&
                 __builtin_offsetof(project_t, cond) + NTRK * NSTEP == __builtin_offsetof(project_t, lk) &&
                 __builtin_offsetof(project_t, lk) + 4u * PLK_MAX + 4u == sizeof(project_t));
    /* (.noinit holds 200 bytes besides the slots: fm1_crash, felucca_dbg, bootguard, panel, settings) */
    bad += check("format 8 fits one flash object; 4 slots fit .noinit", sizeof(project_t) <= 3840u &&
                 4u * sizeof(project_t) < 0x3D50u - 512u);
    bad += check("format 6 = format 5 (3144 bytes) + 32 + 256 conditions + 56 locks (3656); 7: 3672; 8: 3680",
                 sizeof(project_v5_t) == 3144u && sizeof(project_v6_t) == 3656u && PLK_MAX == 56u &&
                 __builtin_offsetof(project_v6_t, lk) == 3656u - 4u - 4u * PLK_MAX && sizeof(project_v7_t) == 3672u &&
                 __builtin_offsetof(project_v7_t, lk) == 3672u - 4u - 4u * PLK_MAX && sizeof(project_t) == 3680u &&
                 __builtin_offsetof(project_t, lk) == 3680u - 4u - 4u * PLK_MAX);

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
    bad += check("FUN3 -> FUN8: converted, valid format 8 slot", ok && proj_ok(&q) && q.magic == PROJ_MAGIC);
    ok = q.sel == 3 && q.g[G_SWING] == 40;
    for (i = 0; i < PROJ_NG_V3; i++)
        ok &= i == G_SWING || q.g[i] == (int16_t)(300 + i);
    for (i = PROJ_NG_V3; i < G_COUNT; i++)
        ok &= q.g[i] == GP[i].def;
    bad += check("FUN3 -> FUN8: globals (swing 50 -> 40: the MPC scale), the new ones default", ok);
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
            ok &= n->p[P_E0 + k] == oldv(t, PROJ_NP_V3 - 8u + k);
    }
    bad += check("FUN3 -> FUN8: parameters (P_E0.. moved), steps, drum notes -> lanes", ok);

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
    bad += check("FUN2 -> FUN8: converted, valid format 8 slot", ok && proj_ok(&q) && q.magic == PROJ_MAGIC);
    ok = q.sel == 2;
    for (i = 0; i < PROJ_NG_V2; i++)
        ok &= i == G_SWING || q.g[i] == (int16_t)(500 + i);
    bad += check("FUN2 -> FUN8: globals and selected track", ok);
    ok = 1;
    for (t = 0; t < NTRK; t++)
        ok &= track_ok_v2(&q.t[t], &v2.t[t], t);
    bad += check("FUN2 -> FUN8: every parameter mapped, SLICER OFF, CHORD OFF (4 tracks)", ok);
    bad += check("FUN2 -> FUN8: engine bytes kept (WHEEL 7, ANALOG 0, TRIO 6), drum 0",
                 q.t[0].engine == 7 && q.t[1].engine == 0 && q.t[2].engine == 6 && q.t[3].engine == 0 &&
                 str_eq(ENGINES[7]->name, "WHEEL") && str_eq(ENGINES[6]->name, "TRIO") && NENGINES > 8);

    /* format 4 (SLOOP 2.0..2.3): by id up to P_CHORD, the arp rhythm its defaults, P_E0.. moved */
    {
        static project_v4_t v4;
        uint32_t k;
        memset(&v4, 0, sizeof v4);
        v4.magic = PROJ_MAGIC_V4;
        v4.size = sizeof v4;
        for (i = 0; i < G_COUNT; i++)
            v4.g[i] = (int16_t)(200 + i);
        v4.sel = 1;
        for (t = 0; t < NTRK; t++) {
            for (k = 0; k < PROJ_NP_V4; k++)
                v4.t[t].p[k] = oldv(t, k);
            v4.t[t].engine = OLD_ENG[t];
            v4.t[t].preset = (uint8_t)(t + 2u);
            v4.t[t].step[k = t + 3u].lvl = 0x5A;
            v4.t[t].step[k].rat = 0x12;
        }
        v4.sum = proj_hash(&v4, sizeof v4 - 4u);
        memcpy(&buf, &v4, sizeof v4);
        ok = proj_import(&q, &buf, (int)sizeof v4) && proj_ok(&q) && q.magic == PROJ_MAGIC && q.sel == 1 &&
             !memcmp(q.g, v4.g, sizeof q.g);
        for (t = 0; t < NTRK; t++) {
            for (k = 0; k <= P_CHORD; k++)
                ok &= q.t[t].p[k] == oldv(t, k);
            for (k = P_AACC; k < P_E0; k++)
                ok &= q.t[t].p[k] == TP[k].def;
            ok &= q.t[t].p[P_AACC] == 0 && q.t[t].p[P_AHITS] == 16 && q.t[t].p[P_ASTEPS] == 16 && q.t[t].p[P_ARAT] == 0;
            for (k = 0; k < 8u; k++)
                ok &= q.t[t].p[P_E0 + k] == oldv(t, PROJ_NP_V4 - 8u + k);
            ok &= q.t[t].engine == OLD_ENG[t] && q.t[t].preset == t + 2u && !memcmp(q.t[t].step, v4.t[t].step, sizeof q.t[t].step);
        }
        bad += check("FUN4 -> FUN8: parameters by id, the arp rhythm default, P_E0.. moved, steps", ok);
        v4.t[2].p[7]++;
        memcpy(&buf, &v4, sizeof v4);
        bad += check("FUN4 with a bad checksum: refused", !proj_import(&q2, &buf, (int)sizeof v4));
    }

    /* format 5 (SLOOP 2.4): by id up to P_ARAT, ROT / SYNC / RHYM / DEJA their defaults, P_E0.. moved, every condition ALWAYS */
    {
        static project_v5_t v5;
        uint32_t k;
        memset(&v5, 0, sizeof v5);
        v5.magic = PROJ_MAGIC_V5;
        v5.size = sizeof v5;
        for (i = 0; i < G_COUNT; i++)
            v5.g[i] = (int16_t)(400 + i);
        v5.sel = 2;
        for (t = 0; t < NTRK; t++) {
            for (k = 0; k < PROJ_NP_V5; k++)
                v5.t[t].p[k] = oldv(t, k);
            v5.t[t].engine = OLD_ENG[t];
            v5.t[t].preset = (uint8_t)(t + 4u);
            v5.t[t].step[k = t + 7u].lvl = 0xA5;
            v5.t[t].step[k].rat = 0x21;
        }
        dstep_set(&v5.t[TRK_DRUM].dstep[2], 11, LV_HARD, 3);
        v5.sum = proj_hash(&v5, sizeof v5 - 4u);
        bad += check("FUN5 image is 3144 bytes (as stored)", sizeof v5 == 3144u);
        memcpy(&buf, &v5, sizeof v5);
        ok = proj_import(&q, &buf, (int)sizeof v5) && proj_ok(&q) && q.magic == PROJ_MAGIC && q.sel == 2 &&
             !memcmp(q.g, v5.g, sizeof q.g);
        for (t = 0; t < NTRK; t++) {
            for (k = 0; k <= P_ARAT; k++)
                ok &= q.t[t].p[k] == oldv(t, k);
            ok &= q.t[t].p[P_AROT] == 0 && q.t[t].p[P_ASYNC] == 0 && q.t[t].p[P_ARHYM] == 0 && q.t[t].p[P_ADEJA] == 0 &&
                  q.t[t].p[P_ASHIFT] == 0 && q.t[t].p[P_ACYC] == 4 && q.t[t].p[P_TURN] == 0;
            for (k = 0; k < 8u; k++)
                ok &= q.t[t].p[P_E0 + k] == oldv(t, PROJ_NP_V5 - 8u + k);
            ok &= q.t[t].engine == OLD_ENG[t] && q.t[t].preset == t + 4u && !memcmp(q.t[t].step, v5.t[t].step, sizeof q.t[t].step);
            for (k = 0; k < NSTEP; k++)
                ok &= q.cond[t][k] == CN_ALWAYS;
        }
        for (k = 0; k < PLK_MAX; k++)
            ok &= !q.lk[k].id;
        bad += check("FUN5 -> FUN8: parameters by id, ROT..TURN default, P_E0.. moved, steps, lanes, conditions ALWAYS, no locks", ok);
        v5.t[1].p[50]++;
        memcpy(&buf, &v5, sizeof v5);
        bad += check("FUN5 with a bad checksum: refused", !proj_import(&q2, &buf, (int)sizeof v5));
        v5.t[1].p[50]--;
        v5.sum = proj_hash(&v5, sizeof v5 - 4u);
        memcpy(&buf, &v5, sizeof v5);
        bad += check("FUN5 with a wrong length: refused", !proj_import(&q2, &buf, (int)sizeof v5 - 2));
    }

    /* format 6 (SLOOP 2.5 before SHIFT): by id up to P_ADEJA, SHIFT / CYC their defaults, P_E0.. moved;
     * the conditions and the locks as they were, a lock of P_E0..P_E7 (id + 1) moved with it */
    {
        static project_v6_t v6;
        uint32_t k;
        int16_t v = 0;
        memset(&v6, 0, sizeof v6);
        v6.magic = PROJ_MAGIC_V6;
        v6.size = sizeof v6;
        for (i = 0; i < G_COUNT; i++)
            v6.g[i] = (int16_t)(600 + i);
        v6.sel = 1;
        for (t = 0; t < NTRK; t++) {
            for (k = 0; k < PROJ_NP_V6; k++)
                v6.t[t].p[k] = oldv(t, k);
            v6.t[t].engine = OLD_ENG[t];
            v6.t[t].preset = (uint8_t)(t + 3u);
            v6.t[t].step[k = t + 9u].lvl = 0x3C;
            v6.t[t].step[k].rat = 0x14;
            v6.cond[t][t + 1u] = (uint8_t)(CN_1_2 + t);
        }
        dstep_set(&v6.t[TRK_DRUM].dstep[4], 9, LV_GHOST, 1);
        v6.lk[0] = (plk_t){0 << 6 | 4, 58 + 1 + 1, ENGINES[OLD_ENG[0]]->edit[1].def};   /* EDIT 2 (P_E1 was 59) */
        v6.lk[1] = (plk_t){1 << 6 | 6, 58 + 7 + 1, -3};                                /* EDIT 8 (P_E7 was 65) */
        v6.lk[2] = (plk_t){2 << 6 | 63, P_ED_FLT + 1, 21};
        v6.lk[9] = (plk_t){TRK_DRUM << 6 | 4, P_SLDEPTH + 1, 64};
        v6.sum = proj_hash(&v6, sizeof v6 - 4u);
        bad += check("FUN6 image is 3656 bytes (as stored)", sizeof v6 == 3656u);
        memcpy(&buf, &v6, sizeof v6);
        ok = proj_import(&q, &buf, (int)sizeof v6) && proj_ok(&q) && q.magic == PROJ_MAGIC && q.sel == 1 &&
             !memcmp(q.g, v6.g, sizeof q.g) && !memcmp(q.cond, v6.cond, sizeof q.cond);
        for (t = 0; t < NTRK; t++) {
            for (k = 0; k <= P_ADEJA; k++)
                ok &= q.t[t].p[k] == oldv(t, k);
            ok &= q.t[t].p[P_ASHIFT] == 0 && q.t[t].p[P_ACYC] == 4 && q.t[t].p[P_TURN] == 0;
            for (k = 0; k < 8u; k++)
                ok &= q.t[t].p[P_E0 + k] == oldv(t, PROJ_NP_V6 - 8u + k);
            ok &= q.t[t].engine == OLD_ENG[t] && q.t[t].preset == t + 3u && !memcmp(q.t[t].step, v6.t[t].step, sizeof q.t[t].step);
        }
        bad += check("FUN6 -> FUN8: parameters by id, SHIFT / CYC / TURN default, P_E0.. moved, steps, lanes, conditions", ok);
        ok = q.lk[0].id == P_E1 + 1 && q.lk[0].ts == (0 << 6 | 4) && q.lk[1].id == P_E7 + 1 && q.lk[1].v == -3 &&
             q.lk[2].id == P_ED_FLT + 1 && q.lk[2].v == 21 && q.lk[9].id == P_SLDEPTH + 1 && !q.lk[3].id;
        host_tracks_init();
        proj_apply(&q, 1);
        ok &= plk_get(&trk[0], 4, P_E1, &v) && v == ENGINES[OLD_ENG[0]]->edit[1].def && plk_count(&trk[1], 6) == 1u &&
              plk_get(TDRUM, 4, P_SLDEPTH, &v) && v == 64 && plk_count(&trk[0], 4) == 1u;
        bad += check("FUN6 -> FUN8: the locks kept, those of P_E0..P_E7 moved with it (+3)", ok);
        v6.lk[1].v++;
        memcpy(&buf, &v6, sizeof v6);
        bad += check("FUN6 with a bad checksum: refused", !proj_import(&q2, &buf, (int)sizeof v6));
        v6.lk[1].v--;
        v6.sum = proj_hash(&v6, sizeof v6 - 4u);
        memcpy(&buf, &v6, sizeof v6);
        bad += check("FUN6 with a wrong length: refused", !proj_import(&q2, &buf, (int)sizeof v6 - 2));
    }

    /* format 7 (SLOOP 2.5 before TURN): by id up to P_ACYC, TURN its default (0), P_E0.. moved; the
     * conditions and the locks as they were, a lock of P_E0..P_E7 (id + 1) moved with it */
    {
        static project_v7_t v7;
        uint32_t k;
        int16_t v = 0;
        memset(&v7, 0, sizeof v7);
        v7.magic = PROJ_MAGIC_V7;
        v7.size = sizeof v7;
        for (i = 0; i < G_COUNT; i++)
            v7.g[i] = (int16_t)(700 + i);
        v7.sel = 2;
        for (t = 0; t < NTRK; t++) {
            for (k = 0; k < PROJ_NP_V7; k++)
                v7.t[t].p[k] = oldv(t, k);
            v7.t[t].engine = OLD_ENG[t];
            v7.t[t].preset = (uint8_t)(t + 5u);
            v7.t[t].step[k = t + 11u].lvl = 0x1E;
            v7.t[t].step[k].rat = 0x09;
            v7.cond[t][t + 2u] = (uint8_t)(CN_P25 + t);
        }
        dstep_set(&v7.t[TRK_DRUM].dstep[6], 12, LV_SOFT, 2);
        v7.lk[0] = (plk_t){0 << 6 | 5, 60 + 0 + 1, ENGINES[OLD_ENG[0]]->edit[0].def};   /* EDIT 1 (P_E0 was 60) */
        v7.lk[3] = (plk_t){2 << 6 | 7, 60 + 7 + 1, 5};                                 /* EDIT 8 (P_E7 was 67) */
        v7.lk[4] = (plk_t){1 << 6 | 62, P_DLY + 1, 99};
        v7.lk[11] = (plk_t){TRK_DRUM << 6 | 6, P_SLRATE + 1, 2};
        v7.sum = proj_hash(&v7, sizeof v7 - 4u);
        bad += check("FUN7 image is 3672 bytes (as stored)", sizeof v7 == 3672u);
        memcpy(&buf, &v7, sizeof v7);
        ok = proj_import(&q, &buf, (int)sizeof v7) && proj_ok(&q) && q.magic == PROJ_MAGIC && q.sel == 2 &&
             !memcmp(q.g, v7.g, sizeof q.g) && !memcmp(q.cond, v7.cond, sizeof q.cond);
        for (t = 0; t < NTRK; t++) {
            for (k = 0; k <= P_ACYC; k++)
                ok &= q.t[t].p[k] == oldv(t, k);
            ok &= q.t[t].p[P_TURN] == 0;
            for (k = 0; k < 8u; k++)
                ok &= q.t[t].p[P_E0 + k] == oldv(t, PROJ_NP_V7 - 8u + k);
            ok &= q.t[t].engine == OLD_ENG[t] && q.t[t].preset == t + 5u && !memcmp(q.t[t].step, v7.t[t].step, sizeof q.t[t].step);
        }
        bad += check("FUN7 -> FUN8: parameters by id, TURN 0, P_E0.. moved, steps, lanes, levels, conditions", ok);
        ok = q.lk[0].id == P_E0 + 1 && q.lk[0].ts == (0 << 6 | 5) && q.lk[3].id == P_E7 + 1 && q.lk[3].v == 5 &&
             q.lk[4].id == P_DLY + 1 && q.lk[4].v == 99 && q.lk[11].id == P_SLRATE + 1 && !q.lk[1].id;
        host_tracks_init();
        proj_apply(&q, 1);
        ok &= plk_get(&trk[0], 5, P_E0, &v) && v == ENGINES[OLD_ENG[0]]->edit[0].def && plk_get(&trk[2], 7, P_E7, &v) &&
              v == 5 && plk_get(&trk[1], 62, P_DLY, &v) && v == 99 && plk_get(TDRUM, 6, P_SLRATE, &v) && v == 2 &&
              !plk_get(&trk[2], 7, P_E6, &v) && trk[1].p[P_TURN] == 0;
        bad += check("FUN7 -> FUN8: the locks kept, those of P_E0..P_E7 moved with it (+1)", ok);
        v7.t[3].p[20]++;
        memcpy(&buf, &v7, sizeof v7);
        bad += check("FUN7 with a bad checksum: refused", !proj_import(&q2, &buf, (int)sizeof v7));
        v7.t[3].p[20]--;
        v7.sum = proj_hash(&v7, sizeof v7 - 4u);
        memcpy(&buf, &v7, sizeof v7);
        bad += check("FUN7 with a wrong length: refused", !proj_import(&q2, &buf, (int)sizeof v7 - 2));
    }

    /* a FUN8 round trip: stored as is (an engine added since: 8) */
    q.t[1].engine = 8;
    q.t[0].step[3].lvl = 0x9C;
    q.t[0].step[3].rat = 0x27;
    dstep_set(&q.t[TRK_DRUM].dstep[5], 13, LV_GHOST, 2);
    q.t[2].p[P_ARHYM] = 9;
    q.t[2].p[P_ADEJA] = 100;
    q.t[2].p[P_ASHIFT] = -3;
    q.t[2].p[P_ACYC] = 7;
    q.t[1].p[P_TURN] = 35;
    q.t[TRK_DRUM].p[P_TURN] = 100;
    q.sum = proj_sum(&q);
    memcpy(&buf, &q, sizeof q);
    bad += check("FUN8 -> FUN8: as stored (levels, ratchets, lanes, engine 8, RHYM, DEJA, SHIFT, CYC, TURN)",
                 proj_import(&q2, &buf, (int)sizeof q) && !memcmp(&q, &q2, sizeof q) && q2.t[1].engine == 8 &&
                 q2.t[2].p[P_ASHIFT] == -3 && q2.t[2].p[P_ACYC] == 7 && q2.t[1].p[P_TURN] == 35 &&
                 q2.t[TRK_DRUM].p[P_TURN] == 100);
    q.cond[0][3] = CN_1_4;
    q.cond[TRK_DRUM][5] = CN_P25;
    q.cond[2][63] = CN_NFIRST;
    q.sum = proj_sum(&q);
    memcpy(&buf, &q, sizeof q);
    bad += check("FUN8 -> FUN8: as stored (levels, ratchets, lanes, engine 8, conditions)",
                 proj_import(&q2, &buf, (int)sizeof q) && !memcmp(&q, &q2, sizeof q) && q2.t[1].engine == 8 &&
                 q2.cond[0][3] == CN_1_4 && q2.cond[TRK_DRUM][5] == CN_P25 && q2.cond[2][63] == CN_NFIRST);
    q.lk[0] = (plk_t){0 << 6 | 3, P_ED_FLT + 1, 40};
    q.lk[7] = (plk_t){TRK_DRUM << 6 | 5, P_SLCR + 1, 2};
    q.sum = proj_sum(&q);
    memcpy(&buf, &q, sizeof q);
    bad += check("FUN8 -> FUN8: as stored (levels, ratchets, lanes, engine 8, locks)",
                 proj_import(&q2, &buf, (int)sizeof q) && !memcmp(&q, &q2, sizeof q) && q2.t[1].engine == 8 &&
                 q2.lk[7].v == 2);

    /* damaged / wrong size */
    v2.t[1].p[3]++;
    memcpy(&buf, &v2, sizeof v2);
    bad += check("FUN2 with a bad checksum: refused", !proj_import(&q2, &buf, (int)sizeof v2));
    v2.t[1].p[3]--;
    memcpy(&buf, &v2, sizeof v2);
    bad += check("FUN2 with a wrong length: refused", !proj_import(&q2, &buf, (int)sizeof v2 - 2));
    memcpy(&buf, &q, sizeof q);
    buf.v8.magic = PROJ_MAGIC_V3;
    bad += check("FUN8 size with a FUN3 magic: refused", !proj_import(&q2, &buf, (int)sizeof q));
    memcpy(&buf, &q, sizeof q);
    buf.v8.magic = PROJ_MAGIC_V6;
    bad += check("FUN8 size with a FUN6 magic: refused", !proj_import(&q2, &buf, (int)sizeof q));
    memcpy(&buf, &q, sizeof q);
    buf.v8.magic = PROJ_MAGIC_V7;
    bad += check("FUN8 size with a FUN7 magic: refused", !proj_import(&q2, &buf, (int)sizeof q));
    memcpy(&buf, &q, sizeof q);
    buf.v8.lk[0].v++;
    bad += check("FUN8 with a lock changed (bad checksum): refused", !proj_import(&q2, &buf, (int)sizeof q));
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
              q.t[t].p[P_E0] == ENGINES[trk_def_engine(t)]->edit[0].def &&
              (t == TRK_DRUM ? dstep_mask(&q.t[t].dstep[0]) == 0u : q.t[t].step[0].time == ST_REST);
    bad += check("FUN1 -> FUN8: track 1 mapped, tracks 2..4 defaults", ok);

    /* capture / apply: the working project round trip */
    host_tracks_init();
    for (t = 0; t < NTRK; t++)
        trk[t].p[P_SLEN] = (int16_t)(5 + t);
    trk[1].step[2].n = 2, trk[1].step[2].note[0] = 60, trk[1].step[2].note[1] = 64, trk[1].step[2].time = ST_NOTE;
    trk[1].step[2].lvl = 0x0D;
    dstep_set(&TDRUM->dstep[9], 4, LV_SOFT, 1);
    trk[1].cond[2] = CN_FILL;
    TDRUM->cond[9] = CN_2_3;
    song.g[G_DUST] = 33;
    proj_capture(&q);
    host_tracks_init();
    proj_apply(&q, 1);
    ok = trk[1].cond[2] == CN_FILL && TDRUM->cond[9] == CN_2_3 && trk[0].cond[2] == CN_ALWAYS;
    q.cond[3][1] = 200;                                /* (out of range: ALWAYS) */
    proj_apply(&q, 1);
    ok &= TDRUM->cond[1] == CN_ALWAYS;
    bad += check("the working project: conditions captured / applied (bad ones ALWAYS)", ok);
    ok = trk[2].p[P_SLEN] == 7 && trk[1].step[2].n == 2 && trk[1].step[2].lvl == 0x0D && song.g[G_DUST] == 33 &&
         dstep_has(&TDRUM->dstep[9], 4) && dstep_lvl(&TDRUM->dstep[9], 4) == LV_SOFT && dstep_rat(&TDRUM->dstep[9], 4) == 1u;
    bad += check("the working project: capture -> apply round trip (levels, lanes, DUST)", ok);

    /* the locks: captured and applied; on load each one checked */
    host_tracks_init();
    host_preset(&trk[0], 0, 0);
    ok = plk_set(&trk[0], 4, P_ED_FLT, 30) && plk_set(&trk[0], 4, P_E1, 20) && plk_set(&trk[1], 0, P_REV, 100) &&
         plk_set(TDRUM, 2, P_SLDEPTH, 64);
    trk[0].p[P_ED_FLT] = -10;
    proj_capture(&q);
    host_tracks_init();
    proj_apply(&q, 1);
    {
        int16_t v = 0, w = 0, x = 0, y = 0;
        ok &= plk_get(&trk[0], 4, P_ED_FLT, &v) && v == 30 && plk_get(&trk[0], 4, P_E1, &w) && w == 20 &&
              plk_get(&trk[1], 0, P_REV, &x) && x == 100 && plk_get(TDRUM, 2, P_SLDEPTH, &y) && y == 64 &&
              trk[0].p[P_ED_FLT] == -10 && plk_count(&trk[0], 4) == 2u;
    }
    bad += check("locks: capture -> apply round trip (the track's own value kept apart)", ok);
    memset(q.lk, 0, sizeof q.lk);
    q.lk[0] = (plk_t){0 << 6 | 1, P_ED_FLT + 1, 500};       /* out of its range: clamped */
    q.lk[1] = (plk_t){0 << 6 | 1, P_ED_FLT + 1, 5};         /* the same lock again: dropped */
    q.lk[2] = (plk_t){0 << 6 | 1, P_SLEN + 1, 3};           /* a pattern parameter: dropped */
    q.lk[3] = (plk_t){TRK_DRUM << 6 | 1, P_ATK + 1, 3};     /* the drum track: only its SLICER */
    q.lk[4] = (plk_t){1 << 6 | 2, P_COUNT + 1, 3};          /* no such parameter */
    q.lk[5] = (plk_t){2 << 6 | 63, P_PAN + 1, -80};         /* the last step, clamped */
    for (i = 0; i < PLK_STEP + 2u; i++)                     /* 10 on one step: 8 kept */
        q.lk[10 + i] = (plk_t){1 << 6 | 7, (uint8_t)(P_ATK + i + 1u), 1};
    proj_apply(&q, 1);
    {
        int16_t v = 0, w = 0;
        uint32_t n = 0;
        for (i = 0; i < PLK_MAX; i++)
            n += plk[i].id != 0;
        ok = plk_get(&trk[0], 1, P_ED_FLT, &v) && v == 63 && !plk_get(&trk[0], 1, P_SLEN, &w) &&
             plk_count(TDRUM, 1) == 0u && plk_count(&trk[1], 2) == 0u && plk_get(&trk[2], 63, P_PAN, &w) && w == -64 &&
             plk_count(&trk[1], 7) == PLK_STEP && n == 2u + PLK_STEP;
    }
    bad += check("locks on load: range, duplicates, what cannot lock, PLK_STEP a step: checked", ok);

    printf("%s\n", bad ? "PROJECT FORMAT TEST FAILED" : "project format test passed");
    return bad != 0;
}
