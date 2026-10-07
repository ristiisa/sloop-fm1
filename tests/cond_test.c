/* SPDX-License-Identifier: GPL-3.0-only */
/* Step conditions (seq.c CN_*) on the real sequencer, the audio running block by block, and the real UI
 * (the harness of ui_pages_test.c) for the gestures:
 *   ALWAYS     every step on every pass (as before the conditions)
 *   chance     12 / 25 / 50 / 75 / 88 %: the share of passes it plays, drums and synths
 *   a:b        on pass a of every b, each track on its own length; FIRST / !FIRST, again after STOP or a section
 *   FILL       FX + a black key held: FILL steps play, !FILL steps do not (and back when let go)
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
    puts(bad ? "cond test FAILED" : "cond: all checks ok");
    return bad ? 1 : 0;
}
