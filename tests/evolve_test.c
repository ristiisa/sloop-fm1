/* SPDX-License-Identifier: GPL-3.0-only */
/* EVOLVE (GLO > JAM: EVOL, BACK; seq.c evolve_*) on the real code: the bars of the transport clock read by
 * evolve_tick (as the UI does each frame), the real mutate passes, the real UI (the harness of
 * ui_pages_test.c) for the page, the gestures and the audio between the frames:
 *   OFF     nothing changes, no undo, no random number drawn
 *   EVOL    1 / 2 / 4 / 8 bars: a pass on each evolving track exactly on those bars (none between, none
 *           stopped), 1..4 steps a pass; muted, soloed-away, recording and empty tracks untouched
 *   BACK    4 / 8 / 16 bars: exactly the pattern of the first pass on its bars (EVOL as long or longer: one
 *           pass from it); a hand edit (an undo step, DICE, GRIDS, the editor) is a new starting point; a new
 *           run (STOP / PLAY, EVOL off / on) a new snapshot; a track TURN rewrites still comes back
 *   undo    the first pass takes the undo step (the selected track first), kept over STOP / PLAY; EDIT +
 *           OCT- after STOP: the pattern from before EVOLVE exactly; MUTATE's turn-back does not take passes
 *   long    thousands of bars: mutate's invariants (scale, notes, LEN, ties, span, kicks on the beats,
 *           sounds, bits), conditions and locks kept
 * Exit status: the number of failed checks. */
#define UI_TEST_MAIN ui_main
#include "ui_pages_test.c"

static int bad;
static void ck(int ok, const char *what)
{
    printf("evolve: %-88s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

static step_t init[NTRK][NSTEP], prev[NTRK][NSTEP];
static uint8_t init_cond[NTRK][NSTEP];
static plock_t init_lk[NTRK][NLOCK];

static void save(step_t (*s)[NSTEP])
{
    uint32_t i;
    for (i = 0; i < NTRK; i++)
        memcpy(s[i], trk[i].step, sizeof s[i]);
}
static void load(step_t (*s)[NSTEP])
{
    uint32_t i;
    for (i = 0; i < NTRK; i++)
        memcpy(trk[i].step, s[i], sizeof s[i]);
}
static uint32_t diff(const step_t *a, const step_t *b)   /* steps that differ */
{
    uint32_t i, n = 0;
    for (i = 0; i < NSTEP; i++)
        n += memcmp(&a[i], &b[i], sizeof a[i]) != 0;
    return n;
}
static int same(uint32_t i, step_t (*s)[NSTEP]) { return !diff(trk[i].step, s[i]); }

static void note(track_t *t, uint32_t i, uint32_t n, const uint8_t *notes, uint32_t time, uint32_t lvl, uint32_t rat)
{
    put_step(t, i, n, notes, time, 0);
    t->step[i].lvl = (uint8_t)lvl;
    t->step[i].rat = (uint8_t)rat;
}
static int in_scale(const track_t *t, int32_t n)
{
    return (scale_mask(t) >> (uint32_t)((n - t->p[P_ROOT] + 120) % 12)) & 1u;
}
static uint32_t span(const track_t *t, const step_t *st)
{
    uint32_t i, k, lo = 127, hi = 0;
    for (i = 0; i < trk_len(t); i++)
        for (k = 0; st[i].time == ST_NOTE && k < st[i].n; k++) {
            lo = st[i].note[k] < lo ? st[i].note[k] : lo;
            hi = st[i].note[k] > hi ? st[i].note[k] : hi;
        }
    return hi >= lo ? hi - lo : 0u;
}
/* mutate's invariants (tests/mutate_test.c): a synth part against the pattern it started from */
static const char *synth_bad(const track_t *t, const step_t *a)
{
    uint32_t len = trk_len(t), i, j, k, s0 = span(t, a);
    for (i = len; i < NSTEP; i++)
        if (memcmp(&t->step[i], &a[i], sizeof a[i]))
            return "a step past LEN changed";
    for (i = 0; i < len; i++) {
        const step_t *s = &t->step[i];
        uint32_t most = a[i].time == ST_NOTE && a[i].n > 1u ? a[i].n : 1u;
        if (s->time > ST_REST)
            return "a step time out of range";
        if (s->time == ST_TIE) {
            for (j = 1; j < len && t->step[(i + len - j) % len].time == ST_TIE; j++)
                ;
            if (t->step[(i + len - j) % len].time != ST_NOTE || !t->step[(i + len - j) % len].n)
                return "a TIE after no note";
            continue;
        }
        if (s->time != ST_NOTE)
            continue;
        if (s->n > 4u || s->n > most)
            return "more notes in a step than it had (a new one: 1)";
        if (s->n < 4u && ((s->lvl | s->rat) >> (2u * s->n)))
            return "level / ratchet bits past the step's notes";
        for (k = 0; k < s->n; k++) {
            if (!in_scale(t, s->note[k]))
                return "a note out of the scale";
            for (j = 0; j < k; j++)
                if (s->note[j] == s->note[k])
                    return "the same note twice in a step";
        }
    }
    if (span(t, t->step) > (s0 > 12u ? s0 : 12u))
        return "the notes spread over more than an octave (or their span)";
    return 0;
}
static const char *drum_bad(const track_t *t, const step_t *a)
{
    const dstep_t *init = (const dstep_t *)a;
    uint32_t len = trk_len(t), i, l, used0 = 0, used = 0;
    for (i = len; i < NSTEP; i++)
        if (memcmp(&t->dstep[i], &init[i], sizeof init[i]))
            return "a step past LEN changed";
    for (i = 0; i < len; i++) {
        const dstep_t *s = &t->dstep[i];
        used0 |= dstep_mask(&init[i]);
        used |= dstep_mask(s);
        for (l = 0; l < DRUM_LANES; l++) {
            if (!dstep_has(s, l) && (dstep_lvl(s, l) || dstep_rat(s, l)))
                return "level / ratchet bits on a lane without a hit";
            if ((l == LANE_KICK || l == LANE_KICK2) && i % 4u == 0u && dstep_has(&init[i], l) != dstep_has(s, l))
                return "a kick on the beat came or went";
            if (dstep_rat(s, l) && !((MUT_HATS >> l) & 1u))
                return "a ratchet on a lane that is not a hat";
        }
    }
    if (used & ~used0)
        return "a sound the pattern did not use";
    return 0;
}
static const char *all_bad(void)                    /* every track against init, conditions and locks */
{
    uint32_t i;
    const char *why;
    for (i = 0; i < NTRK; i++)
        if ((why = i == TRK_DRUM ? drum_bad(&trk[i], init[i]) : synth_bad(&trk[i], init[i])) != 0)
            return why;
    for (i = 0; i < NTRK; i++)
        if (memcmp(trk[i].cond, init_cond[i], NSTEP))
            return "a condition changed";
    for (i = 0; i < NTRK; i++)
        if (memcmp(trk[i].lock, init_lk[i], sizeof init_lk[i]))
            return "a lock changed";
    return 0;
}

static void groove(track_t *t, uint32_t len)        /* (mutate_test.c) */
{
    uint32_t i;
    t->p[P_SLEN] = (int16_t)len;
    steps_clear(t);
    for (i = 0; i < len; i++) {
        if (i % 4u == 0u || i == 10u)
            dstep_set(&t->dstep[i], LANE_KICK, LV_NORM, 0);
        if (i % 8u == 4u)
            dstep_set(&t->dstep[i], LANE_SNARE, LV_NORM, 0);
        if (i % 2u == 0u && i != 14u)
            dstep_set(&t->dstep[i], LANE_HAT, i % 4u ? LV_SOFT : LV_NORM, 0);
    }
    dstep_set(&t->dstep[14], LANE_OPEN, LV_NORM, 0);
    dstep_set(&t->dstep[12], LANE_CLAP, LV_HARD, 0);
    dstep_set(&t->dstep[40], LANE_BELL, LV_NORM, 0);        /* past LEN: never touched */
}
/* track 1: D minor (a tie, a chord with levels / ratchets, a slide, a condition, a lock); track 2: C major
 * (a bass line, 8 steps); track 3: empty; the drum track: the groove */
static void patterns(void)
{
    static const uint8_t D3[1] = {50}, F3[1] = {53}, DFA[3] = {62, 65, 69}, A3[1] = {57}, C4[1] = {60}, E3[1] = {52},
                         G3[1] = {55}, X[1] = {99}, C3[1] = {48}, G2[1] = {43}, E2[1] = {40};
    track_t *t = &trk[0], *u = &trk[1];
    uint32_t i;
    for (i = 0; i < NTRK; i++) {
        steps_clear(&trk[i]);
        trk[i].p[P_MUTE] = 0;
        trk[i].p[P_TURN] = 0;
        trk[i].p[P_SLEN] = 16;
        trk[i].p[P_CHORD] = 0;
    }
    song.solo = 0;
    song.rec = 0;
    t->p[P_ROOT] = 2;
    t->p[P_SCALE] = 2;
    t->p[P_VOICE] = V_POLY;
    note(t, 0, 1, D3, ST_NOTE, 0, 0);
    note(t, 1, 0, D3, ST_TIE, 0, 0);
    note(t, 3, 1, F3, ST_NOTE, LV_SOFT, 0);
    note(t, 4, 3, DFA, ST_NOTE, LV_GHOST | LV_HARD << 2, 1u << 4);
    note(t, 6, 1, A3, ST_NOTE, 0, 0);
    t->step[6].flags = SF_SLIDE;
    note(t, 7, 1, C4, ST_NOTE, 0, 0);
    note(t, 8, 0, C4, ST_TIE, 0, 0);
    note(t, 9, 0, C4, ST_TIE, 0, 0);
    note(t, 11, 1, E3, ST_NOTE, 0, 2);
    note(t, 12, 1, G3, ST_NOTE, LV_HARD, 0);
    note(t, 20, 1, X, ST_NOTE, 0, 0);
    t->cond[11] = CN_P50;
    lock_set(t, 12, P_ED_FLT, 30);
    u->p[P_ROOT] = 0;
    u->p[P_SCALE] = 1;
    u->p[P_VOICE] = V_MONO;
    u->p[P_SLEN] = 8;
    note(u, 0, 1, C3, ST_NOTE, 0, 0);
    note(u, 2, 1, G2, ST_NOTE, LV_SOFT, 0);
    note(u, 3, 1, C3, ST_NOTE, 0, 0);
    note(u, 5, 1, E2, ST_NOTE, 0, 0);
    note(u, 6, 1, G2, ST_NOTE, LV_HARD, 0);
    groove(TDRUM, 16);
    TDRUM->cond[6] = CN_1_2;
    lock_set(TDRUM, 4, P_SLDEPTH, 50);
    save(init);
    for (i = 0; i < NTRK; i++)
        memcpy(init_cond[i], trk[i].cond, NSTEP);
    for (i = 0; i < NTRK; i++)
        memcpy(init_lk[i], trk[i].lock, sizeof init_lk[i]);
}

/* the transport, as the audio ISR leaves it for the UI: PLAY (seq_reset_tracks: bar 0), then beat q */
static uint32_t beat;
static void play(void)
{
    song.playing = 1;
    ev_reset++;
    clk_beat = beat = 0;
    evolve_tick();
}
static void stop(void)
{
    song.playing = 0;
    evolve_tick();
}
static void beat_to(uint32_t q)
{
    clk_beat = beat = q;
    evolve_tick();
}
static void evol(uint32_t e, uint32_t k)
{
    song.g[G_EVOL] = (int16_t)e;
    song.g[G_EVBK] = (int16_t)k;
}

/* a run of `bars` bars with EVOL e, BACK k from init; tracks in `on` evolve, the others stay; one frame each
 * beat. 0 = as it should, else why not. *pz: passes of track 2 (C major) that changed nothing */
static const char *run(uint32_t e, uint32_t k, uint32_t bars, uint32_t on, uint32_t *pz)
{
    uint32_t n = 1u << (e - 1u), m = k ? 2u << k : 0u, q, i, b, c;
    const char *why;
    load(init);
    evol(e, k);
    play();
    for (q = 1; q <= bars * 4u; q++) {
        int back, pass;
        save(prev);
        beat_to(q);
        b = q / 4u;
        back = !(q % 4u) && m && !(b % m);
        pass = !(q % 4u) && !(b % n) && (!back || n >= m);
        for (i = 0; i < NTRK; i++) {
            if (!((on >> i) & 1u)) {
                if (!same(i, init))
                    return "a track that does not evolve changed";
                continue;
            }
            if (back && !pass) {
                if (!same(i, init))
                    return "BACK: not the pattern of the first pass";
            } else if (back) {
                if ((c = diff(trk[i].step, init[i])) > MUT_STEPS || (!c && i != 1u))
                    return "BACK, a pass due: not one pass from the first pattern";
                *pz += !c;
            } else if (pass) {
                if ((c = diff(trk[i].step, prev[i])) > MUT_STEPS || (!c && i != 1u))
                    return "a pass due: not 1..4 steps changed";
                *pz += !c;
            } else if (!same(i, prev)) {
                return "changed on a beat (or bar) with nothing due";
            }
        }
        if ((why = all_bad()) != 0)
            return why;
    }
    stop();
    return 0;
}

int main(int argc, char **argv)
{
    static step_t e1[NTRK][NSTEP], snapt[NTRK][NSTEP];
    static const char *const EV[5] = {"OFF", "1", "2", "4", "8"}, *const BK[4] = {"NEVER", "4", "8", "16"};
    track_t *t = &trk[0];
    const char *why;
    uint32_t i, e, k, z, ok, r0;
    char what[128];

    outdir = argc > 1 ? argv[1] : "build/host";
    memset(trk, 0, sizeof trk);
    memset(&song, 0, sizeof song);
    host_tracks_init();
    rng_state = 0xE701u;
    patterns();

    /* ---- the parameters */
    ck(GP[G_EVOL].def == 0 && GP[G_EVOL].max == 4 && GP[G_EVBK].def == 0 && GP[G_EVBK].max == 3 &&
       !strcmp(GP[G_EVOL].label, "EVOL") && !strcmp(GP[G_EVBK].label, "BACK"), "EVOL OFF / 1 2 4 8 bars, BACK NEVER / 4 8 16 bars, both off by default");

    /* ---- OFF: nothing */
    evol(0, 3);
    undo.valid = 0;
    r0 = rng_state;
    play();
    for (i = 1; i <= 64u * 4u; i++)
        beat_to(i);
    stop();
    ok = rng_state == r0 && !undo.valid;
    for (i = 0; i < NTRK; i++)
        ok &= same(i, init);
    ck(ok, "EVOL OFF (BACK 16): 64 bars, every pattern as it was, no undo step, no random number drawn");

    /* ---- every EVOL x BACK: the passes and the returns exactly on their bars */
    for (e = 1; e <= 4u; e++)
        for (k = 0; k <= 3u; k++) {
            z = 0;
            why = run(e, k, 64, 0xBu, &z);
            snprintf(what, sizeof what, "EVOL %s, BACK %s, 64 bars: passes / returns on their bars only, invariants", EV[e], BK[k]);
            ck(!why && z < 4u, why ? why : what);
        }

    /* ---- only while playing */
    load(init);
    evol(1, 1);
    play();
    beat_to(5);
    save(prev);
    stop();
    for (i = 6; i < 80u; i++)
        beat_to(i);
    ok = 1;
    for (i = 0; i < NTRK; i++)
        ok &= same(i, prev);
    ck(ok && !same(0, init), "stopped: the bars go by (clock moved), nothing evolves, nothing comes back");

    /* ---- who evolves: muted, soloed away, recording, empty: untouched */
    trk[1].p[P_MUTE] = 1;
    z = 0;
    why = run(1, 0, 24, 0x9u, &z);
    ck(!why, why ? why : "track 2 muted: never changed (the others evolve, track 3 empty: nothing)");
    trk[1].p[P_MUTE] = 0;
    song.solo = 1u;
    why = run(2, 2, 32, 0x1u, &z);
    ck(!why, why ? why : "track 1 soloed: only it evolves (and comes back)");
    song.solo = 0;
    song.rec = 1u << TRK_DRUM;
    why = run(1, 1, 24, 0x3u, &z);
    ck(!why, why ? why : "the drum track recording: not changed, not put back");
    song.rec = 0;
    load(init);
    evol(1, 1);
    play();
    beat_to(8);                                          /* (two passes) */
    save(prev);
    song.rec = 1u;                                       /* track 1 records from here */
    beat_to(16);                                         /* bar 4: BACK */
    ck(same(0, prev) && same(1, init) && same(3, init), "BACK while track 1 records: it stays as recorded, the others come back");
    song.rec = 0;
    stop();

    /* ---- a hand edit: a new starting point */
    load(init);
    evol(1, 1);                                          /* (every bar, BACK every 4) */
    play();
    beat_to(8);
    undo_mark(t, (undo_sess += 4u) | 3u);                /* a step edit (it takes an undo step) */
    t->step[2] = t->step[3];
    t->step[2].note[0] = 62;
    save(e1);
    beat_to(12);                                         /* bar 3: a pass from the edited pattern */
    ck(diff(t->step, e1[0]) >= 1u && diff(t->step, e1[0]) <= MUT_STEPS, "an edit by hand, then a pass: from the edited pattern");
    beat_to(16);                                         /* bar 4: BACK */
    ck(same(0, e1) && same(1, init) && same(3, init), "BACK after an edit: the edited pattern (the new start), the others their first");
    beat_to(20);
    trk[1].step[5].note[0] = 41;                         /* an edit without an undo step (the editor) */
    save(e1);
    beat_to(24);
    beat_to(32);                                         /* bar 8: BACK */
    ck(same(1, e1) && same(3, init), "an edit without an undo step (the editor): a new start as well");
    stop();

    /* ---- a new run: STOP / PLAY, EVOL off / on: a new snapshot */
    load(init);
    evol(1, 2);                                          /* BACK every 8 */
    play();
    beat_to(12);
    stop();
    save(e1);                                            /* evolved for 3 bars */
    play();
    for (i = 1; i <= 32u; i++)
        beat_to(i);
    ck(same(0, e1) && same(3, e1) && !same(0, init), "STOP, PLAY: BACK brings the pattern of the new run's start (not the first run's)");
    save(e1);
    evol(0, 2);
    for (i = 33; i <= 48u; i++)
        beat_to(i);
    ck(same(0, e1) && same(3, e1), "EVOL OFF while playing: frozen as it is (BACK too)");
    evol(1, 2);
    for (i = 49; i <= 52u; i++)                          /* bar 13: a pass, the snapshot: bar 12's */
        beat_to(i);
    ck(!same(0, e1), "EVOL on again: passes on the next bar");
    for (i = 53; i <= 64u; i++)                          /* bar 16: BACK to the pattern when it went on */
        beat_to(i);
    ck(same(0, e1) && same(3, e1), "... a new snapshot: BACK brings the pattern from when EVOL went on");
    stop();
    play();                                              /* (a section: seq_reset_tracks too) */
    beat_to(37);
    ev_reset++;
    clk_beat = 0;
    evolve_tick();
    ck(ev_bar == 0u && !ev_ok, "a section (seq_reset_tracks): bar 0 again, a new run");
    stop();

    /* ---- the undo model (seq.c side) */
    load(init);
    undo.valid = 0;
    song.sel = 0;
    evol(1, 0);
    play();
    for (i = 1; i <= 12u; i++)
        beat_to(i);
    ck(undo.valid && !undo.undone && undo.trk == 0u && !memcmp(undo.st, init[0], sizeof init[0]) && !mutate_depth(t),
       "the first pass: the undo step of the selected track (before EVOLVE); passes: no MUTATE turn-back");
    stop();
    save(e1);
    play();
    for (i = 1; i <= 8u; i++)
        beat_to(i);
    stop();
    ck(undo.trk == 0u && !memcmp(undo.st, init[0], sizeof init[0]) && !same(0, e1), "STOP, PLAY, more passes: the same undo step (before the first)");
    undo_mark(TDRUM, (undo_sess += 4u) | 3u);            /* another edit takes the undo */
    save(e1);
    play();
    beat_to(4);
    stop();
    ck(undo.trk == 0u && !memcmp(undo.st, e1[0], sizeof e1[0]), "after another edit: the next run's first pass takes the undo of then");
    load(init);
    song.sel = 1;
    play();
    beat_to(4);
    stop();
    ck(undo.trk == 1u && !memcmp(undo.st, init[1], sizeof init[1]), "track 2 selected: its undo step");
    load(init);
    song.sel = 2;                                        /* (empty: the next one that evolves) */
    trk[1].p[P_MUTE] = 0;
    undo.valid = 0;
    play();
    beat_to(4);
    stop();
    ck(undo.trk == TRK_DRUM && !memcmp(undo.st, init[TRK_DRUM], sizeof init[0]), "track 3 (empty) selected: the next track that evolves (drums)");
    song.sel = 0;

    /* ---- long runs: mutate's invariants */
    rng_state = 0xA11u;
    z = 0;
    why = run(1, 0, 3000, 0xBu, &z);
    ck(!why && z < 300u, why ? why : "EVOL 1, BACK NEVER, 3000 bars: scale, notes, LEN, ties, span, kicks, sounds, bits, conds, locks");
    why = run(1, 3, 3000, 0xBu, &z);
    ck(!why, why ? why : "EVOL 1, BACK 16, 3000 bars: back exactly every 16 bars, invariants");
    why = run(3, 1, 1000, 0xBu, &z);
    ck(!why, why ? why : "EVOL 4, BACK 4, 1000 bars: each phrase one pass from the first pattern");

    /* ---- the real UI: the page, the audio between the frames, TURN as well, the gesture */
    {
        uint32_t b0, b, ok2 = 1, passes = 0, backs = 0, frames_n = 0;
        static step_t cur[NTRK][NSTEP];
        panel = PANEL_DEFAULT;
        layers_init();
        settings.palette = 4;
        palette_set(4);
        host_tracks_init();
        for (i = 0; i < NPART; i++) {
            set_engine_of(&trk[i], TRK_DEF[i][0]);
            apply_preset_to(&trk[i], TRK_DEF[i][1]);
            trk[i].engine = trk[i].eng_req;
        }
        TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;
        song.sel = 0;
        song.g[G_BPM] = 240;
        go_home();
        frame();
        patterns();
        for (i = 0; i < NTRK; i++)
            trk[i].p[P_SDIV] = 2;
        for (i = 0; i < 8u && strcmp(cur_page()->title, "JAM"); i++)
            open_family(FAM_GLO);
        ui.force = 1;
        frame();
        ppm("page-jam");
        ck(!strcmp(cur_page()->title, "JAM") && cur_page()->id[0] == G_PROG && cur_page()->id[1] == G_EVOL && cur_page()->id[2] == G_EVBK &&
           cur_page()->id[3] == G_AFILL, "GLO tapped: GLOBAL, MASTER, SYSTEM, DRUMS, JAM (EVOL on KNOB 2, BACK on KNOB 3)");
        encs[panel.enc[EN_K2]] = 1;
        frame();
        encs[panel.enc[EN_K3]] = 1;
        frame();
        ck(song.g[G_EVOL] == 1 && song.g[G_EVBK] == 1, "KNOB 2: EVOL 1BAR, KNOB 3: BACK 4BAR");
        undo.valid = 0;
        trk[1].p[P_TURN] = 60;                           /* track 2 rewrites itself as well */
        frames(4);
        save(init);                                      /* (TURN took its undo step: none played yet) */
        transport_req = 1;
        frame();
        b0 = clk_beat >> 2;
        for (frames_n = 0; frames_n < 2000u && (clk_beat >> 2) < 12u; frames_n++) {
            save(cur);
            frame();
            b = clk_beat >> 2;
            if (b == b0) {
                ok2 &= same(0, cur) && same(TRK_DRUM, cur);   /* (track 2: TURN between the bars) */
                continue;
            }
            b0 = b;
            if (!(b % 4u)) {
                backs++;
                ok2 &= same(0, init) && same(TRK_DRUM, init) && (ev_ok & 2u) && !diff(trk[1].step, ev_snap[1]);
            } else {
                passes++;
                ok2 &= !same(0, cur) && !same(TRK_DRUM, cur);
            }
        }
        ck(ok2 && passes == 9u && backs == 3u, "PLAY, 12 bars: a pass on the bars, BACK on 4, 8, 12 (track 2 with TURN too), nothing between");
        ck(undo.trk == 0u && !memcmp(undo.st, init[0], sizeof init[0]), "the undo step: track 1 before EVOLVE");
        for (frames_n = 0; frames_n < 2000u && (clk_beat >> 2) < 14u; frames_n++)
            frame();
        transport_req = 2;
        frame();
        trk[1].p[P_TURN] = 0;
        frames(2);
        save(snapt);
        ck(!song.playing && !same(0, init), "STOP: the evolved patterns kept");
        press(B_EDIT);
        frames(10);
        edges_btn |= BT(B_OCTDN); fm1_in.buttons |= BT(B_OCTDN); frame(); fm1_in.buttons &= ~BT(B_OCTDN); frame();
        ok = same(0, init) && !memcmp(t->cond, init_cond[0], NSTEP);
        {
            int k = lock_find(t, 12, P_ED_FLT, 0);
            ok &= k >= 0 && t->lock[k].val == 30;
        }
        ck(ok, "EDIT + OCT-: track 1 back as before EVOLVE, exactly (its condition, its lock)");
        edges_btn |= BT(B_OCTUP); fm1_in.buttons |= BT(B_OCTUP); frame(); fm1_in.buttons &= ~BT(B_OCTUP); frame();
        ck(same(0, snapt), "EDIT + OCT+: the evolved one again");
        release(B_EDIT);

        /* DICE and GRIDS by hand: new starting points */
        load(init);
        evol(1, 1);
        transport_req = 1;
        frame();
        for (frames_n = 0; frames_n < 2000u && (clk_beat >> 2) < 2u; frames_n++)
            frame();
        pattern_dice(TDRUM, 1);                          /* bar 2: a new groove */
        save(e1);
        grids_turn(0, 3);                                /* .. a touch of GRIDS X: another */
        save(snapt);
        ok = !same(TRK_DRUM, e1);
        for (frames_n = 0; frames_n < 2000u && (clk_beat >> 2) < 4u; frames_n++)
            frame();
        ck(ok && same(TRK_DRUM, snapt) && same(0, init), "DICE, then GRIDS by hand: BACK brings the GRIDS groove (the new start), track 1 its first");
        transport_req = 2;
        frames(2);
        evol(0, 0);
    }

    printf(bad ? "evolve: %d FAILED\n" : "evolve: all ok\n", bad);
    return bad != 0;
}
