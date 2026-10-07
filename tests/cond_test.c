/* SPDX-License-Identifier: GPL-3.0-only */
/* Step conditions (seq.c CN_*) on the real sequencer, the audio running block by block, and the real UI
 * (the harness of ui_pages_test.c) for the gestures:
 *   ALWAYS     every step on every pass (as before the conditions)
 *   chance     12 / 25 / 50 / 75 / 88 %: the share of passes it plays, drums and synths
 *   a:b        on pass a of every b, each track on its own length; FIRST / !FIRST, again after STOP or a section
 *   FILL       FX + a black key held: FILL steps play, !FILL steps do not (and back when let go)
 *   AFILL      GLO -> JAM: FILL by itself in the last bar (half bar) of every 2..16 bars of the clock from PLAY
 *              or a section, not of the tracks' loops (LEN 3, 5, 12, 16, 32); FILL held adds to it; shown
 *   a step     keeps its chord and ratchets when it plays; plays nothing (no ratchet, no slide held) when not
 *   editing    SEQ + steps held + KNOB 4; EDIT shift, length x2, undo / redo carry them; recording into an
 *              empty step, erase, clear: ALWAYS; recording onto a step keeps its condition
 * Exit status: the number of failed checks. */
#define UI_TEST_MAIN ui_main
#include "ui_pages_test.c"

static int bad;
static void ck(int ok, const char *what)
{
    printf("cond: %-80s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

/* note-ons of every track by its pass (1..63, counted here: step 0 starts one) and step */
#define NP 64u
static uint32_t hits[NTRK][NP][NSTEP], tot[NTRK][NSTEP], pass_no[NTRK], last_abs[NTRK];
static void block(void)
{
    static int32_t out[CTL * 2];
    uint32_t va = vage, da = drums.age, i, k, n;
    mix_block(out, CTL);
    for (i = 0; i < NTRK; i++) {
        track_t *t = &trk[i];
        if (!song.playing)
            continue;
        if (t->seq_abs != last_abs[i]) {
            last_abs[i] = t->seq_abs;
            if (!t->seq_idx)
                pass_no[i]++;
        }
        n = 0;
        if (i < NPART) {
            for (k = 0; k < NVOICE; k++)
                n += t->v[k].age > va;
        } else {
            for (k = 0; k < NDRUM; k++)
                n += drums.v[k].age > da;
        }
        if (pass_no[i] < NP)
            hits[i][pass_no[i]][t->seq_idx % NSTEP] += n;
        tot[i][t->seq_idx % NSTEP] += n;
    }
}
static void count_reset(void)
{
    memset(hits, 0, sizeof hits);
    memset(tot, 0, sizeof tot);
    memset(pass_no, 0, sizeof pass_no);
    memset(last_abs, 0xFF, sizeof last_abs);
}
static void stop(void)
{
    if (song.playing) {
        transport_req = 2;
        block();
    }
}
/* every track empty, 240 BPM, 1/32 steps, 16 long, nothing held */
static void reset(void)
{
    uint32_t i;
    stop();
    host_tracks_init();
    memset(&drums, 0, sizeof drums);
    drums.set = -2;
    song.g[G_BPM] = 240;
    song.rec = 0;
    song.sel = 0;
    rec_wait = 0;
    fm1_in.notes = fm1_in.buttons = 0;
    kb_prev = 0;
    for (i = 0; i < NTRK; i++) {
        trk[i].p[P_SDIV] = 3;
        trk[i].p[P_SLEN] = 16;
        trk[i].p[P_AMODE] = 0;
    }
    count_reset();
}
static void play(void)
{
    count_reset();
    transport_req = 1;
    block();
}
static void run_passes(uint32_t i, uint32_t n)    /* until track i has played n passes */
{
    uint32_t guard = 4000000u;
    while (pass_no[i] <= n && guard--)
        block();
}
static void note(track_t *t, uint32_t idx, uint8_t nt)
{
    put_step(t, idx, 1, &nt, ST_NOTE, 0);
}
static void hit(uint32_t idx, uint32_t lane)
{
    dstep_set(&TDRUM->dstep[idx], lane, LV_NORM, 0);
}
/* step idx of track i played on pass p exactly when (p - 1) % b == a - 1 (passes 1..n) */
static int cycle_ok(uint32_t i, uint32_t idx, uint32_t a, uint32_t b, uint32_t n)
{
    uint32_t p;
    for (p = 1; p <= n; p++)
        if ((hits[i][p][idx] != 0u) != ((p - 1u) % b == a - 1u))
            return 0;
    return 1;
}

static void t_always(void)
{
    uint32_t p, k, ok = 1;
    reset();
    trk[0].p[P_SLEN] = 4;
    TDRUM->p[P_SLEN] = 3;
    for (k = 0; k < 4u; k++)
        note(&trk[0], k, (uint8_t)(48 + k));
    for (k = 0; k < 3u; k++)
        hit(k, k);
    trk[1].p[P_SLEN] = 4;
    trk[1].cond[1] = CN_P50;                         /* (an empty step: nothing to play whatever its condition) */
    play();
    run_passes(0, 6);
    for (p = 1; p <= 6u; p++) {
        for (k = 0; k < 4u; k++)
            ok &= hits[0][p][k] == 1u;
        for (k = 0; k < 3u; k++)
            ok &= hits[TRK_DRUM][p][k] == 1u;
    }
    for (k = 0; k < 4u; k++)
        ok &= tot[1][k] == 0u;
    ck(ok, "ALWAYS: every step plays on every pass (synth LEN 4, drums LEN 3); an empty step stays silent");
}

static void t_chance(void)
{
    static const uint8_t SY[3] = {CN_P88, CN_P75, CN_P50}, DR[3] = {CN_P12, CN_P25, CN_P50};
    static const uint32_t PCT[CN_P88 + 1] = {100, 12, 25, 50, 75, 88};
    uint32_t r, n = 2000;
    for (r = 0; r < 3u; r++) {
        uint32_t ns = 0, nd = 0;
        int32_t es, ed;
        char what[96];
        reset();
        trk[0].p[P_SLEN] = 1;
        TDRUM->p[P_SLEN] = 1;
        note(&trk[0], 0, 60);
        hit(0, 4);
        trk[0].cond[0] = SY[r];
        TDRUM->cond[0] = DR[r];
        play();
        run_passes(TRK_DRUM, n);
        ns = tot[0][0];
        nd = tot[TRK_DRUM][0];
        es = (int32_t)(ns * 1000u / n) - (int32_t)PCT[SY[r]] * 10;
        ed = (int32_t)(nd * 1000u / n) - (int32_t)PCT[DR[r]] * 10;
        snprintf(what, sizeof what, "chance: synth %s %u of %u, drums %s %u of %u (within 4 %%)", N_COND[SY[r]], ns, n,
                 N_COND[DR[r]], nd, n);
        ck(es > -40 && es < 40 && ed > -40 && ed < 40 && ns < n && nd > 0u, what);
    }
}

static void t_cycles(void)
{
    static const uint8_t C0[4] = {CN_1_2, CN_2_3, CN_3_4, CN_4_4}, CD[3] = {CN_2_2, CN_1_3, CN_3_3};
    static const uint8_t C1[5] = {CN_1_4, CN_2_4, CN_2_2, CN_1_3, CN_ALWAYS};
    uint32_t k, ok = 1;
    reset();
    trk[0].p[P_SLEN] = 4;
    trk[1].p[P_SLEN] = 5;
    TDRUM->p[P_SLEN] = 3;
    for (k = 0; k < 4u; k++) {
        note(&trk[0], k, 60);
        trk[0].cond[k] = C0[k];
    }
    for (k = 0; k < 5u; k++) {
        note(&trk[1], k, 67);
        trk[1].cond[k] = C1[k];
    }
    for (k = 0; k < 3u; k++) {
        hit(k, 0);
        TDRUM->cond[k] = CD[k];
    }
    play();
    run_passes(1, 13);                              /* (the longest: 13 x 5 steps; the others play more) */
    for (k = 0; k < 4u; k++)
        ok &= cycle_ok(0, k, CN_CYCLE[C0[k] - CN_1_2] >> 4, CN_CYCLE[C0[k] - CN_1_2] & 15u, 16);
    for (k = 0; k < 4u; k++)
        ok &= cycle_ok(1, k, CN_CYCLE[C1[k] - CN_1_2] >> 4, CN_CYCLE[C1[k] - CN_1_2] & 15u, 13);
    ok &= cycle_ok(1, 4, 1, 1, 13);
    for (k = 0; k < 3u; k++)
        ok &= cycle_ok(TRK_DRUM, k, CN_CYCLE[CD[k] - CN_1_2] >> 4, CN_CYCLE[CD[k] - CN_1_2] & 15u, 21);
    ck(ok, "a:b: on pass a of every b, each track on its length (LEN 4, 5, 3: 1:2 .. 4:4)");
}

static void t_first(void)
{
    uint32_t p, ok = 1, before;
    reset();
    trk[0].p[P_SLEN] = 2;
    TDRUM->p[P_SLEN] = 2;
    note(&trk[0], 0, 60);
    note(&trk[0], 1, 62);
    trk[0].cond[0] = CN_FIRST;
    trk[0].cond[1] = CN_NFIRST;
    hit(0, 0);
    TDRUM->cond[0] = CN_NFIRST;
    play();
    run_passes(0, 5);
    for (p = 1; p <= 5u; p++)
        ok &= hits[0][p][0] == (p == 1u) && hits[0][p][1] == (p != 1u) && hits[TRK_DRUM][p][0] == (p != 1u);
    ck(ok, "1ST plays only the first pass, !1ST every other (synth and drums)");
    stop();
    play();
    run_passes(0, 3);
    ck(hits[0][1][0] == 1u && hits[0][2][0] == 0u && hits[0][1][1] == 0u && hits[0][2][1] == 1u,
       "STOP, PLAY: 1ST plays again on the first pass");
    run_passes(0, 6);
    before = tot[0][0];
    live_req = 0;                                    /* a section, on the next bar: the passes start again */
    while (live_req >= 0)
        block();
    run_passes(0, pass_no[0] + 3u);
    ck(before == 1u && tot[0][0] == 2u && trk[0].seq_pass >= 3u, "a section change: the passes count again from 1 (1ST once more)");
}

static void t_fill(void)
{
    uint32_t p, ok = 1, req = (uint32_t)punch.req;
    reset();
    trk[0].p[P_SLEN] = 2;
    TDRUM->p[P_SLEN] = 2;
    note(&trk[0], 0, 60);
    trk[0].cond[0] = CN_FILL;
    hit(0, 0);
    hit(1, 2);
    TDRUM->cond[0] = CN_FILL;
    TDRUM->cond[1] = CN_NFILL;
    play();
    run_passes(0, 2);
    for (p = 1; p <= 2u; p++)
        ok &= !hits[0][p][0] && !hits[TRK_DRUM][p][0] && hits[TRK_DRUM][p][1] == 1u;
    ck(ok, "FILL not held: FILL steps silent, !FILL steps play");
    fm1_in.buttons = ly_bit[LY_FX];                  /* FX + F#3 (a black key) held */
    fm1_in.notes = 1u << 1;
    block();
    ck(fill_keys == 2u && kb_kind[1] == KS_FILL && (uint32_t)punch.req == req, "FX + a black key: FILL (no punch-in effect)");
    p = pass_no[0] + 1u;
    run_passes(0, p + 1u);
    ok = hits[0][p][0] == 1u && hits[0][p + 1u][0] == 1u && hits[TRK_DRUM][p][0] == 1u && !hits[TRK_DRUM][p][1];
    ck(ok, "FILL held: FILL steps play, !FILL steps do not (synth and drums)");
    fm1_in.buttons = 0;                              /* FX let go first: the key still ends FILL */
    block();
    ck(fill_keys == 2u, "FX let go, the black key still held: FILL on");
    fm1_in.notes = 0;
    block();
    p = pass_no[0] + 1u;
    run_passes(0, p);
    ck(!fill_keys && !hits[0][p][0] && hits[TRK_DRUM][p][1] == 1u, "the black key up: FILL off, as before");
}

static void t_keeps(void)
{
    static const uint8_t CH[3] = {60, 64, 67};
    uint32_t p, ok = 1, k;
    reset();
    trk[0].p[P_SLEN] = 2;
    TDRUM->p[P_SLEN] = 2;
    put_step(&trk[0], 0, 3, CH, ST_NOTE, 0);
    trk[0].step[0].rat = 0x15;                       /* every note x2 */
    trk[0].cond[0] = CN_1_2;
    dstep_set(&TDRUM->dstep[0], 0, LV_NORM, 2);       /* two lanes x3 */
    dstep_set(&TDRUM->dstep[0], 4, LV_HARD, 2);
    TDRUM->cond[0] = CN_2_2;
    play();
    run_passes(0, 4);
    for (p = 1; p <= 4u; p++) {
        ok &= hits[0][p][0] == (p & 1u ? 6u : 0u);
        ok &= hits[TRK_DRUM][p][0] == (p & 1u ? 0u : 6u);
    }
    ck(ok, "a chord x2 (1:2) and two lanes x3 (2:2): all of it when it plays, no ratchet when not");

    reset();
    trk[0].p[P_SLEN] = 2;
    put_step(&trk[0], 0, 1, CH, ST_NOTE, SF_SLIDE);  /* a slide into a step that does not play */
    note(&trk[0], 1, 62);
    trk[0].cond[1] = CN_FILL;
    play();
    while (pass_no[0] < 1u || trk[0].seq_idx != 1u)
        block();
    block();
    for (k = 0, ok = 1; k < NVOICE; k++)
        ok &= !(trk[0].v[k].active && trk[0].v[k].gate);
    ck(ok && !trk[0].seq_n, "a slide into a step whose condition fails: released (a rest)");
}

static void t_edit(void)
{
    track_t *t = &trk[0];
    uint32_t k, ok;
    reset();
    t->p[P_SLEN] = 4;
    for (k = 0; k < 4u; k++)
        note(t, k, 60);
    t->cond[0] = CN_1_2;
    t->cond[3] = CN_FILL;
    ui.step_sess = 0;
    pattern_rotate(t, 1);
    ok = t->cond[1] == CN_1_2 && t->cond[0] == CN_FILL && !t->cond[2] && !t->cond[3];
    pattern_rotate(t, -1);
    ok &= t->cond[0] == CN_1_2 && t->cond[3] == CN_FILL && !t->cond[1];
    ck(ok, "EDIT + KNOB 1 shift: the conditions move with their steps (both ways, round the end)");
    pattern_length(t, 1);
    ck(t->p[P_SLEN] == 8 && t->cond[4] == CN_1_2 && t->cond[7] == CN_FILL && t->cond[0] == CN_1_2,
       "EDIT + KNOB 2 x2: the copy has the conditions");
    t->cond[5] = CN_P25;
    ck(undo_swap(0) && t->p[P_SLEN] == 4 && t->cond[0] == CN_1_2 && t->cond[3] == CN_FILL && !t->cond[4] && !t->cond[5],
       "undo: the conditions as before the hold");
    ck(undo_swap(1) && t->p[P_SLEN] == 8 && t->cond[4] == CN_1_2 && t->cond[5] == CN_P25, "redo: back, as left");
    ui.step_sess = 0;
    pattern_length(t, -1);
    ck(t->p[P_SLEN] == 4 && t->cond[4] == CN_1_2, "LENGTH / 2: the steps beyond (and their conditions) kept");
    steps_clear(t);
    for (k = 0, ok = 1; k < NSTEP; k++)
        ok &= !t->cond[k];
    ck(ok, "clear: every condition ALWAYS");

    /* erase: a step erased empty loses its condition, a step that keeps notes keeps it */
    reset();
    note(t, 2, 60);
    put_step(t, 3, 2, (const uint8_t[]){60, 64}, ST_NOTE, 0);
    t->cond[2] = t->cond[3] = CN_3_4;
    hit(5, 0);
    hit(6, 0);
    hit(6, 4);
    TDRUM->cond[5] = TDRUM->cond[6] = CN_P75;
    er_notes[60 >> 5] |= 1u << (60 & 31u);
    erase_step(t, 2);
    erase_step(t, 3);
    er_notes[60 >> 5] = 0;
    er_lanes = 1u;
    erase_step(TDRUM, 5);
    erase_step(TDRUM, 6);
    er_lanes = 0;
    ck(!t->cond[2] && t->cond[3] == CN_3_4 && !TDRUM->cond[5] && TDRUM->cond[6] == CN_P75,
       "erase: an emptied step ALWAYS, a step with notes / hits left keeps its condition");
}

static void t_record(void)
{
    track_t *t = &trk[0];
    uint32_t k, later, idx, ok;
    reset();
    for (k = 0; k < NSTEP; k++)
        t->cond[k] = TDRUM->cond[k] = CN_P50;       /* (stale: on empty steps) */
    play();
    run_passes(0, 1);
    song.rec = 1u;
    idx = rec_target(t, &later) % trk_len(t);
    input_on(t, 60, 100);
    input_off(t, 60);
    ok = t->step[idx].n == 1u && t->cond[idx] == CN_ALWAYS && t->cond[(idx + 1u) % 16u] == CN_P50;
    t->cond[idx] = CN_1_3;
    input_on(t, 64, 100);
    input_off(t, 64);
    ok &= t->step[idx].n == 2u && t->cond[idx] == CN_1_3;
    ck(ok, "live recording: a new step ALWAYS, a note added to a step keeps its condition");
    song.rec = 1u << TRK_DRUM;
    idx = rec_target(TDRUM, &later) % trk_len(TDRUM);
    drum_input(0, LV_NORM, 0, 1);
    ok = dstep_has(&TDRUM->dstep[idx], 0) && TDRUM->cond[idx] == CN_ALWAYS;
    TDRUM->cond[idx] = CN_2_4;
    drum_input(4, LV_NORM, 0, 1);
    ok &= dstep_has(&TDRUM->dstep[idx], 4) && TDRUM->cond[idx] == CN_2_4;
    ck(ok, "live recording, drums: a new step ALWAYS, a hit added keeps the condition");
    song.rec = 0;
    stop();
}

/* the real gestures: SEQ + steps held + KNOB 4, the dial, the tiles; KNOB 4 alone: LENGTH */
static void t_ui(void)
{
    track_t *t = &trk[0];
    reset();
    go_home();
    frame();
    note(t, 4, 60);
    note(t, 6, 62);
    t->cond[6] = CN_P25;
    press(B_SEQ);
    frames(10);
    fm1_in.notes = 1u << key_of_white(4) | 1u << key_of_white(6);
    frame();
    encs[panel.enc[EN_K4]] = 3;
    frame();
    ck(t->cond[4] == CN_P50 && t->cond[6] == CN_P88, "SEQ + steps 5 and 7 held + KNOB 4: each condition 3 on (50 %, 88 %)");
    ui.force = 1;
    frame();
    ppm("layer-steps-cond");
    encs[panel.enc[EN_K4]] = -40;
    frame();
    encs[panel.enc[EN_K4]] = 1;
    frame();
    ck(t->cond[4] == CN_P12 && t->cond[6] == CN_P12, "KNOB 4 left to ALWAYS (bounded), one right: 12 %");
    fm1_in.notes = 0;
    frame();
    ck(step_on(&t->step[4]) && step_on(&t->step[6]) && t->p[P_SLEN] == 16, "steps let go after KNOB 4: kept, LENGTH untouched");
    encs[panel.enc[EN_K4]] = -1;
    frame();
    ck(t->p[P_SLEN] < 16, "SEQ + KNOB 4, no step held: LENGTH as before");
    release(B_SEQ);
    t->p[P_SLEN] = 16;

    song.sel = TRK_DRUM;
    go_home();
    frame();
    hit(2, 5);
    pen_lane = 5;
    press(B_SEQ);
    frames(10);
    fm1_in.notes = 1u << key_of_white(2);
    frame();
    encs[panel.enc[EN_K4]] = 15;
    frame();
    fm1_in.notes = 0;
    frame();
    release(B_SEQ);
    ck(TDRUM->cond[2] == CN_FILL && dstep_has(&TDRUM->dstep[2], 5), "drums: SEQ + step 3 held + KNOB 4: the step's condition (FILL)");
    song.sel = 0;
}

/* ---- AFILL (GLO -> JAM): FILL by itself in the last bar of every 2 / 4 / 8 / 16 (2H..16H: its last half
 * bar), bars of 4 beats of the clock from PLAY or a section, whatever the tracks' lengths */
#define AB_N 1024u                                   /* note-ons by 1/16 step of the clock: section s at s x 512 */
static uint32_t ab_hits[NTRK][AB_N], ab_sec, ab_last;
static void ab_block(void)
{
    static int32_t out[CTL * 2];
    uint32_t va = vage, da = drums.age, i, k, n, x;
    mix_block(out, CTL);
    if (!song.playing || trk[0].seq_abs == SEQ_NONE)
        return;
    if (trk[0].seq_abs < ab_last)
        ab_sec++;                                    /* (a section: the clock from 0 again) */
    ab_last = trk[0].seq_abs;
    for (i = 0; i < NTRK; i++) {
        n = 0;
        if (i < NPART) {
            for (k = 0; k < NVOICE; k++)
                n += trk[i].v[k].age > va;
        } else {
            for (k = 0; k < NDRUM; k++)
                n += drums.v[k].age > da;
        }
        x = ab_sec * 512u + trk[i].seq_abs;
        if (x < AB_N)
            ab_hits[i][x] += n;
    }
}
static void ab_until(uint32_t sec, uint32_t abs)  /* until the clock is at 1/16 step abs of section sec */
{
    uint32_t guard = 4000000u;
    while ((ab_sec < sec || trk[0].seq_abs == SEQ_NONE || trk[0].seq_abs < abs) && guard--)
        ab_block();
}
/* every track 1/16 (16 steps a bar) on its own length, a note / hit on every step: FILL, !FILL, ALWAYS, ... */
static void ab_setup(const uint8_t *len, uint32_t afill)
{
    uint32_t i, k;
    reset();
    for (i = 0; i < NTRK; i++) {
        trk[i].p[P_SDIV] = 2;
        trk[i].p[P_SLEN] = len[i];
        for (k = 0; k < len[i]; k++) {
            if (i < NPART)
                note(&trk[i], k, (uint8_t)(48u + 7u * i + k % 5u));
            else
                hit(k, k % 3u);
            trk[i].cond[k] = k % 3u == 0u ? CN_FILL : k % 3u == 1u ? CN_NFILL : CN_ALWAYS;
        }
    }
    song.g[G_AFILL] = (int16_t)afill;
    memset(ab_hits, 0, sizeof ab_hits);
    ab_sec = ab_last = 0;
    transport_req = 1;
    ab_block();
}
/* the bar of 1/16 step abs is a fill bar of AFILL a (written out on its own: N bars, the last one / half) */
static int ab_fill(uint32_t a, uint32_t abs)
{
    static const uint8_t NB[9] = {0, 2, 4, 8, 16, 2, 4, 8, 16};
    uint32_t bar = abs / 16u;
    return a && bar % NB[a] == NB[a] - 1u && (a <= 4u || abs % 16u >= 8u);
}
/* steps 0..n-1 of section sec, every track: FILL steps played exactly in the fill bars (or with FILL held:
 * steps m0..m1-1), !FILL ones exactly outside, ALWAYS always; once each */
static int ab_ok(uint32_t a, uint32_t sec, uint32_t n, uint32_t m0, uint32_t m1)
{
    uint32_t i, s;
    for (i = 0; i < NTRK; i++)
        for (s = 0; s < n; s++) {
            uint32_t c = trk[i].cond[s % (uint32_t)trk[i].p[P_SLEN]], h = ab_hits[i][sec * 512u + s];
            int f = ab_fill(a, s) || (s >= m0 && s < m1), want = c == CN_FILL ? f : c == CN_NFILL ? !f : 1;
            if (h > 1u || (h != 0u) != want) {
                printf("cond: AFILL %u, track %u, step %u of section %u: %u note-ons, want %d\n", a, i + 1u, s, sec, h, want);
                return 0;
            }
        }
    return 1;
}

static void t_afill(void)
{
    static const uint8_t LA[NTRK] = {3, 5, 12, 16}, LB[NTRK] = {32, 16, 5, 3};
    const uint8_t *L[2] = {LA, LB};
    uint32_t a, r, ok;
    char what[120];
    ck(G_AFILL + 1 == G_PROG && GP[G_AFILL].max == 8 && GP[G_AFILL].def == 0 && str_eq(GP[G_AFILL].names[4], "16") &&
       str_eq(GP[G_AFILL].names[5], "2H"), "AFILL: OFF, 2, 4, 8, 16 bars, 2H..16H (half a bar); OFF by default");
    for (r = 0; r < 2u; r++)
        for (a = 0; a <= 8u; a++) {
            uint32_t bars = a ? 2u * (2u << ((a - 1u) & 3u)) + 1u : 5u;
            ab_setup(L[r], a);
            ab_until(0, bars * 16u);
            ok = ab_ok(a, 0, bars * 16u, 0, 0);
            snprintf(what, sizeof what, "AFILL %s (LEN %u, %u, %u, drums %u): FILL / !FILL by the bars, %u bars", N_AFILL[a],
                     L[r][0], L[r][1], L[r][2], L[r][3], bars);
            ck(ok, what);
            stop();
        }

    /* FILL held by hand: in bar 2 (not an AFILL bar) and over bar 4 (one): FILL as either says */
    ab_setup(LA, 2);
    ab_until(0, 15);
    fm1_in.buttons = ly_bit[LY_FX];                  /* FX + a black key, from the last step of bar 1 */
    fm1_in.notes = 1u << 1;
    ab_block();
    fm1_in.buttons = 0;
    ab_until(0, 31);
    fm1_in.notes = 0;
    ab_block();
    ab_until(0, 47);
    fm1_in.buttons = ly_bit[LY_FX];
    fm1_in.notes = 1u << 3;
    ab_block();
    fm1_in.buttons = 0;
    ab_until(0, 63);
    fm1_in.notes = 0;
    ab_block();
    ab_until(0, 9u * 16u);
    ck(ab_ok(2, 0, 9u * 16u, 16, 32) && !fill_keys, "AFILL 4 and FILL held (bar 2, and over bar 4): FILL plays when either is on");
    for (ok = 1, r = 48; r < 64u; r++)               /* (bar 4: held and AFILL, as by either alone) */
        ok &= ab_hits[0][r] == (r % 3u != 1u);
    ck(ok, "FILL held in an AFILL bar: that bar as by either alone (no step twice)");
    stop();

    /* a section restarts the count: AFILL 2, a section asked for in bar 3 starts on bar 4 (would be a fill bar) */
    ab_setup(LA, 1);
    ab_until(0, 40);
    live_req = 0;
    ab_until(1, 4u * 16u);
    ck(live_req < 0 && ab_ok(1, 0, 48, 0, 0) && ab_ok(1, 1, 64, 0, 0) && !ab_hits[0][48],
       "a section: the bars count again from its start (its 1st bar no fill, its 2nd one)");
    stop();
    ck(!fill_on(), "stopped: no AFILL");
}

/* what shows: the TRACKS header's beat lights in the FX colour (and "fill"), the FX layer's black keys */
static uint32_t beat_lights_fx(void)
{
    uint32_t k, n = 0;
    for (k = 0; k < 4u; k++)
        n += swap16(screen[29u * 240u + 107u + 9u * k]) == TE_DRUM;
    return n;
}
static void t_afill_ui(void)
{
    uint32_t guard, lit_off, lit_on, n_off, n_on;
    reset();
    song.g[G_AFILL] = 2;
    go_home();
    ui.force = 1;
    frame();
    transport_req = 1;
    frame();
    n_off = beat_lights_fx();
    press(B_FX);
    frames(12);
    lit_off = keys_lit();
    for (guard = 2000; !fill_on() && guard; guard--)
        frame();
    frame();
    lit_on = keys_lit();
    ui.force = 1;
    frame();
    ppm("layer-fx-afill");
    release(B_FX);
    go_home();
    ui.force = 1;
    frame();
    n_on = beat_lights_fx();
    ppm("page-tracks-afill");
    ck(fill_on() && (clk_beat >> 2) % 4u == 3u, "AFILL 4: FILL on in the 4th bar");
    ck(lit_off == 0u && lit_on == 0x52A52Au, "the FX layer: the black keys lit in an AFILL bar (as if held), not before");
    ck(n_off == 0u && n_on == 3u, "TRACKS header: the beat lights of an AFILL bar in the FX colour");
    while (fill_on() && guard--)
        frame();
    ui.force = 1;
    frame();
    ck(!fill_on() && beat_lights_fx() == 0u, "the bar after: FILL off, the beat lights as before");
    stop();
    song.g[G_AFILL] = 0;

    /* GLO tapped to its JAM page (after DRUMS): KNOB 4 AFILL */
    palette_set(4);                                  /* (the colours of ui_pages_test.c's renders) */
    go_home();
    frame();
    for (guard = 0; guard < 8u && !str_eq(cur_page()->title, "JAM"); guard++)
        tap(B_GLO);
    encs[panel.enc[EN_K4]] = 1;
    frame();
    ui.force = 1;
    frame();
    ppm("page-jam");
    ck(str_eq(cur_page()->title, "JAM") && cur_page()->fam == FAM_GLO && !str_eq(cur_page()[-1].title, "JAM") &&
       str_eq(cur_page()[-1].title, "DRUMS") && song.g[G_AFILL] == 1, "GLO -> JAM (after DRUMS): KNOB 4 AFILL (OFF -> 2)");
    song.g[G_AFILL] = 0;
    go_home();
    frame();
}

int main(int argc, char **argv)
{
    outdir = argc > 1 ? argv[1] : "build/host";
    panel = PANEL_DEFAULT;                           /* (the layers' buttons: FX for FILL) */
    layers_init();
    ck(CN_COUNT == 19 && str_eq(N_COND[CN_ALWAYS], "ALWAYS") && str_eq(N_COND[CN_4_4], "4:4") &&
       str_eq(N_COND[CN_NFIRST], "!1ST"), "the conditions: ALWAYS, 5 chances, 9 cycles, FILL / !FILL, 1ST / !1ST");
    t_always();
    t_chance();
    t_cycles();
    t_first();
    t_fill();
    t_keeps();
    t_edit();
    t_record();
    t_ui();
    t_afill();
    t_afill_ui();
    puts(bad ? "cond test FAILED" : "cond: all checks ok");
    return bad ? 1 : 0;
}
