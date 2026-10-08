/* SPDX-License-Identifier: GPL-3.0-only */
/* Parameter locks (SLOOP 2.4's: seq.c lock_*, track_t.lock) with what SLOOP 2.5 adds, on the real sequencer
 * and UI (the harness of ui_pages_test.c):
 *   lockable   COLOR's TYPE, AMT, RATE (synths and drums), the drum track's SLICER; not TURN, not the arp's
 *   played     a locked step plays its value (in p[] from its step), the next step without one the track's own
 *   gestures   SEQ + a step held: ALGORITHM walks to COLOR, PRESETS locks it; the tile's mark, the footer's dot
 *              (the step shown), a page shows the value a lock holds now in red
 *   tools      EDIT SHIFT, LENGTH x2, undo / redo, DICE (and back) move or drop the locks (and nudges) with their
 *              steps; MUTATE leaves them; an emptied step loses its locks
 *   saved      a lock in force: a project keeps the track's own value
 * Exit status: the number of failed checks. */
#define UI_TEST_MAIN ui_main
#include "ui_pages_test.c"
#define PROJ_HOST 1
#include "../firmware/src/project.c"

static int bad;
static void ck(int ok, const char *what)
{
    printf("locks: %-84s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

static void block(void)
{
    static int32_t out[CTL * 2];
    mix_block(out, CTL);
}
static uint32_t run_to(track_t *t, uint32_t idx)   /* blocks until step idx of t plays */
{
    uint32_t n = 0;
    while ((t->seq_idx != idx || t->seq_abs == SEQ_NONE) && n < 200000u) {
        block();
        n++;
    }
    return n < 200000u;
}
static void stop(void)
{
    transport_req = 2;
    block();
}
static void reset(void)
{
    uint32_t i;
    if (song.playing)
        stop();
    host_tracks_init();
    song.g[G_BPM] = 240;
    song.rec = 0;
    song.sel = 0;
    rec_wait = 0;
    fm1_in.notes = fm1_in.buttons = 0;
    for (i = 0; i < NTRK; i++) {
        trk[i].p[P_SDIV] = 3;
        trk[i].p[P_SLEN] = 16;
    }
}
static int has(const track_t *t, uint32_t step, uint32_t id, int32_t v)   /* a lock of id on step, value v */
{
    int k = lock_find(t, step, id, 0);
    return k >= 0 && t->lock[k].val == v;
}
static void note(track_t *t, uint32_t idx, uint8_t nt) { put_step(t, idx, 1, &nt, ST_NOTE, 0); }

static void t_played(void)
{
    track_t *t = &trk[0];
    uint32_t ok;
    reset();
    note(t, 0, 60);
    note(t, 2, 62);
    note(t, 4, 64);
    t->p[P_COLOR] = 1;
    t->p[P_CAMT] = 64;
    TDRUM->p[P_SLCR] = 0;
    dstep_set(&TDRUM->dstep[3], LANE_KICK, LV_NORM, 0);
    ck(p_lockable(P_COLOR) && p_lockable(P_CAMT) && p_lockable(P_CRATE) && p_lockable(P_SLCR) && p_lockable(P_SLDEPTH) &&
       !p_lockable(P_TURN) && !p_lockable(P_AACC) && !p_lockable(P_ACYC) && !p_lockable(P_SLEN),
       "lockable: COLOR TYPE / AMT / RATE, SLICER; not TURN, the arp, the pattern");
    ok = lock_set(t, 2, P_COLOR, 4) && lock_set(t, 2, P_CAMT, 120) && lock_set(TDRUM, 3, P_SLCR, 2) &&
         lock_set(TDRUM, 3, P_COLOR, 3);
    transport_req = 1;
    block();
    ok &= run_to(t, 1) && t->p[P_COLOR] == 1 && t->p[P_CAMT] == 64;
    ok &= run_to(t, 2) && t->p[P_COLOR] == 4 && t->p[P_CAMT] == 120 && lock_on(t, P_COLOR) && lock_on(t, P_CAMT);
    ok &= run_to(TDRUM, 3) && TDRUM->p[P_SLCR] == 2 && TDRUM->p[P_COLOR] == 3;
    ok &= run_to(t, 4) && t->p[P_COLOR] == 1 && t->p[P_CAMT] == 64 && !lock_on(t, P_COLOR);
    ok &= run_to(TDRUM, 4) && TDRUM->p[P_SLCR] == 0 && TDRUM->p[P_COLOR] == 0;
    ck(ok, "played: COLOR locked on a synth step, SLICER and COLOR on a drum step, the track's own after");
    {   /* a project captured while a lock holds: the track's own value */
        static project_t q;
        static uint8_t x[PX_BYTES];
        run_to(t, 2);
        proj_capture(&q, x);
        ck(t->p[P_COLOR] == 4 && (int8_t)x[4u + (P_COLOR - P_E0_V24)] == 1 && q.t[0].p[P_DIST] == t->p[P_DIST],
           "saved while a lock holds (COLOR 4): the extension keeps the track's own (1)");
    }
    stop();
    ck(t->p[P_COLOR] == 1 && t->p[P_CAMT] == 64, "STOP: the track's own values back");
}

static void t_gestures(void)
{
    track_t *t = &trk[0];
    uint32_t guard, i;
    reset();
    go_home();
    frame();
    note(t, 5, 60);
    ui.lock_par = P_SLDEPTH;
    press(B_SEQ);
    frames(10);
    fm1_in.notes = 1u << key_of_white(5);
    frame();
    for (guard = 0; guard < 30u && ui.lock_par != P_COLOR; guard++) {
        encs[panel.enc[EN_ALGO]] = 1;
        frame();
    }
    ck(ui.lock_par == P_COLOR, "SEQ + a step held: ALGORITHM walks the lockable ones to COLOR (TYPE)");
    encs[panel.enc[EN_PRESET]] = 2;
    frame();
    ck(has(t, 5, P_COLOR, 2) && !t->p[P_COLOR], "PRESETS: a lock of TYPE on the step (2: WAH), the track's own OFF");
    ui.force = 1;
    frame();
    ppm("layer-steps-lock-color");
    ck(str_eq(sub_line(), "lock type wah"), "the title: the lock (lock type wah)");
    fm1_in.notes = 0;
    frame();
    release(B_SEQ);
    ck(step_on(&t->step[5]) && (lock_marks(t, 0) >> 5 & 1u), "the step kept; the footer's mark on it (lock_marks)");
    for (i = 0; i < NPAGES && PAGES[i].id[0] != P_COLOR; i++)
        ;
    ui.page = (uint8_t)i;
    page_entered();
    transport_req = 1;
    frame();
    for (guard = 0; guard < 400u && !(t->seq_idx == 5u && song.playing); guard++)
        frame();
    ck(lock_on(t, P_COLOR) && t->p[P_COLOR] == 2, "playing: on the step the lock holds TYPE (lock_on: a page shows it red)");
    ui.force = 1;
    frame();
    ppm("page-color-locked");
    stop();
    song.sel = TRK_DRUM;
    go_home();
    frame();
    dstep_set(&TDRUM->dstep[2], LANE_KICK, LV_NORM, 0);
    pen_lane = LANE_KICK;
    ui.lock_par = P_SLCR;
    press(B_SEQ);
    frames(10);
    fm1_in.notes = 1u << key_of_white(2);
    frame();
    encs[panel.enc[EN_PRESET]] = 3;
    frame();
    fm1_in.notes = 0;
    frame();
    release(B_SEQ);
    ck(has(TDRUM, 2, P_SLCR, 2), "the drum track: SEQ + a step held + PRESETS: a SLICER lock (OFF + 3: STUT, its last)");
    song.sel = 0;
}

static void t_tools(void)
{
    track_t *t = &trk[0];
    uint32_t ok;
    reset();
    go_home();
    frame();
    note(t, 1, 60);
    note(t, 3, 62);
    note(t, 15, 64);
    t->p[P_SLEN] = 16;
    lock_set(t, 3, P_COLOR, 3);
    lock_set(t, 3, P_E1, 7);
    lock_set(t, 15, P_CRATE, 99);
    t->micro[3] = 9;
    t->cond[3] = CN_1_2;
    press(B_EDIT);
    frames(10);
    encs[panel.enc[EN_K1]] = 1;                       /* SHIFT: one later */
    frame();
    ok = has(t, 4, P_COLOR, 3) && has(t, 4, P_E1, 7) && has(t, 0, P_CRATE, 99) && t->micro[4] == 9 && !t->micro[3] &&
         t->cond[4] == CN_1_2 && lock_find(t, 3, P_COLOR, 0) < 0;
    ck(ok, "EDIT SHIFT: the locks, the nudge and the condition one step later with their notes (the last round to 1)");
    encs[panel.enc[EN_K1]] = -1;
    frame();
    ck(has(t, 3, P_COLOR, 3) && has(t, 15, P_CRATE, 99) && t->micro[3] == 9, "SHIFT back: as they were");
    t->p[P_SLEN] = 8;
    encs[panel.enc[EN_K2]] = 1;                       /* LENGTH x2: steps 0..7 again on 8..15 */
    frame();
    ok = t->p[P_SLEN] == 16 && has(t, 3, P_COLOR, 3) && has(t, 11, P_COLOR, 3) && has(t, 11, P_E1, 7) &&
         t->micro[11] == 9 && lock_find(t, 15, P_CRATE, 0) < 0;
    ck(ok, "LENGTH x2: the locks and nudges of 1..8 again on 9..16 (theirs replaced)");
    tap(B_OCTDN);                                     /* undo (the EDIT hold is one session: to before SHIFT) */
    ok = t->p[P_SLEN] == 16 && lock_find(t, 11, P_COLOR, 0) < 0 && has(t, 15, P_CRATE, 99) && has(t, 3, P_COLOR, 3) &&
         !t->micro[11];
    tap(B_OCTUP);                                     /* redo */
    ok &= t->p[P_SLEN] == 16 && has(t, 11, P_COLOR, 3) && t->micro[11] == 9;
    ck(ok, "EDIT + OCT- / OCT+: undo / redo bring the locks and nudges back as they were");
    {   /* MUTATE: the pattern varies, the locks stay */
        plock_t was[NLOCK];
        memcpy(was, t->lock, sizeof was);
        encs[panel.enc[EN_K4]] = 3;
        frame();
        ck(!memcmp(was, t->lock, sizeof was), "MUTATE: the locks stay where they are");
        encs[panel.enc[EN_K4]] = -3;
        frame();
    }
    encs[panel.enc[EN_PRESET]] = 1;                   /* DICE: a new pattern: no lock, no nudge within LEN */
    frame();
    ok = !locks_on(t, 3) && !locks_on(t, 11) && !t->micro[3] && !t->micro[11];
    encs[panel.enc[EN_PRESET]] = -1;                  /* .. back: the pattern before, its locks and nudges */
    frame();
    ok &= has(t, 3, P_COLOR, 3) && has(t, 11, P_COLOR, 3) && t->micro[11] == 9;
    ck(ok, "DICE: the steps rolled lose their locks and nudges; back: they return");
    release(B_EDIT);
    press(B_SEQ);
    frames(10);
    fm1_in.notes = 1u << key_of_white(3);             /* a set step: let go untouched, it goes */
    frame();
    fm1_in.notes = 0;
    frame();
    release(B_SEQ);
    ck(!step_on(&t->step[3]) && !locks_on(t, 3) && !t->micro[3] && t->cond[3] == CN_ALWAYS,
       "SEQ: a step cleared loses its locks, nudge and condition");
}

int main(int argc, char **argv)
{
    outdir = argc > 1 ? argv[1] : "build/host";
    panel = PANEL_DEFAULT;
    layers_init();
    palette_set(4);
    t_played();
    t_gestures();
    t_tools();
    puts(bad ? "locks test FAILED" : "locks: all checks ok");
    return bad ? 1 : 0;
}
