/* SPDX-License-Identifier: GPL-3.0-only */
/* TURN (SEQ > PATTERN 2, seq.c turing_*) on the real sequencer (seq_tick: each step as it comes round),
 * and the gesture on the real UI (the harness of ui_pages_test.c):
 *   0 %     nothing changes, nothing is armed, no undo taken, no random number drawn
 *   100 %   every step with notes rewritten on every pass: another note of the scale in the register (the
 *           span of the notes, an octave at least), the chord's size, the step's time, flags, levels,
 *           ratchets, conditions and locks kept; ties, rests, empty steps and the steps past LEN as they
 *           were; CHORD on: the chord of the new note; the shift register shifts one bit a rewrite
 *   rates   25 / 50 / 75 %: the share of the steps rewritten (within 4 %); the same seed: the same run
 *   drums   kicks on the beats kept (and none added there), only the sounds the pattern used, no level or
 *           ratchet without its hit, the density near the pattern's; hits come, go and move to their kind
 *   rec     not while the track records (and not armed then)
 *   undo    TURN up from 0 keeps the pattern before it as the undo step; down: frozen as it plays; up again:
 *           the same undo step; EDIT + OCT- brings it back exactly, OCT+ the turned one; another edit takes it
 *   other   TURN cannot be locked on a step, a preset keeps it, the PATTERN 2 page
 * Exit status: the number of failed checks. */
#define UI_TEST_MAIN ui_main
#include "ui_pages_test.c"

static int bad;
static void ck(int ok, const char *what)
{
    printf("turing: %-84s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

typedef struct { step_t st[NSTEP]; uint8_t cond[NSTEP]; int8_t micro[NSTEP]; plock_t lk[NLOCK]; } snap_t;
static void snap(const track_t *t, snap_t *s)
{
    memcpy(s->st, t->step, sizeof s->st);
    memcpy(s->cond, t->cond, sizeof s->cond);
    memcpy(s->micro, t->micro, sizeof s->micro);
    memcpy(s->lk, t->lock, sizeof s->lk);
}
static int same(const track_t *t, const snap_t *s)
{
    static snap_t now;
    snap(t, &now);
    return !memcmp(&now, s, sizeof now);
}
static int in_scale(const track_t *t, int32_t n)
{
    return (scale_mask(t) >> (uint32_t)((n - t->p[P_ROOT] + 120) % 12)) & 1u;
}

/* one pass of t's pattern through the sequencer: each step as it comes round (seq_tick), 1/16 at the clock */
static uint32_t abs_at[NTRK];
static void pass(track_t *t)
{
    uint32_t len = trk_len(t), i, a = abs_at[trk_index(t)], u = BEAT_U / 4u;
    song.playing = 1;
    t->p[P_SDIV] = 2;
    a += (len - a % len) % len;                      /* (from its step 0) */
    for (i = 0; i < len; i++, a++) {
        clk_beat = a / 4u;
        clk_pos = (a % 4u) * u;
        seq_tick(t, 0);
    }
    abs_at[trk_index(t)] = a;
}
static void passes(track_t *t, uint32_t n)
{
    while (n--)
        pass(t);
}

static void note(track_t *t, uint32_t i, uint32_t n, const uint8_t *notes, uint32_t time, uint32_t lvl, uint32_t rat)
{
    put_step(t, i, n, notes, time, 0);
    t->step[i].lvl = (uint8_t)lvl;
    t->step[i].rat = (uint8_t)rat;
}
/* track 0: D minor, a tie, a chord with levels / ratchets, a slide, a condition, a lock, a step past LEN */
static void synth_pattern(track_t *t)
{
    static const uint8_t D3[1] = {50}, F3[1] = {53}, DFA[3] = {62, 65, 69}, A3[1] = {57}, C4[1] = {60}, E3[1] = {52},
                         G3[1] = {55}, X[1] = {99};
    t->p[P_ROOT] = 2;
    t->p[P_SCALE] = 2;
    t->p[P_VOICE] = V_POLY;
    t->p[P_SLEN] = 16;
    t->p[P_CHORD] = 0;
    steps_clear(t);
    memset(t->cond, 0, sizeof t->cond);
    locks_clear(t);
    note(t, 0, 1, D3, ST_NOTE, LV_SOFT, 1);
    note(t, 1, 0, D3, ST_TIE, 0, 0);
    note(t, 3, 1, F3, ST_NOTE, 0, 0);
    note(t, 4, 3, DFA, ST_NOTE, LV_GHOST | LV_HARD << 2, 1u << 4);
    note(t, 6, 1, A3, ST_NOTE, 0, 0);
    t->step[6].flags = SF_SLIDE | SF_ACCENT;
    note(t, 7, 1, C4, ST_NOTE, 0, 0);
    note(t, 8, 0, C4, ST_TIE, 0, 0);
    note(t, 9, 0, C4, ST_TIE, 0, 0);
    note(t, 10, 1, E3, ST_NOTE, 0, 2);
    t->cond[10] = CN_P50;
    note(t, 12, 1, G3, ST_NOTE, LV_HARD, 0);
    lock_set(t, 12, P_ED_FLT, 30);
    note(t, 20, 1, X, ST_NOTE, 0, 0);                        /* past LEN: never touched */
}
static int has_notes(const step_t *s) { return s->time == ST_NOTE && s->n; }
/* t against the pattern it started from (a): what must always hold; *moved: steps whose notes changed */
static const char *synth_bad(const track_t *t, const snap_t *a, uint32_t *moved)
{
    static snap_t now;
    uint32_t len = trk_len(t), i, j, k;
    snap(t, &now);
    if (memcmp(now.cond, a->cond, sizeof now.cond) || memcmp(now.lk, a->lk, sizeof now.lk))
        return "a condition or a lock changed";
    for (i = 0; i < NSTEP; i++) {
        const step_t *s = &now.st[i], *o = &a->st[i];
        if (i >= len || !has_notes(o)) {
            if (memcmp(s, o, sizeof *s))
                return "a step without notes (or past LEN) changed";
            continue;
        }
        if (s->n != o->n || s->time != o->time || s->flags != o->flags || s->vel != o->vel || s->lvl != o->lvl || s->rat != o->rat)
            return "a step's size, time, flags, velocity, levels or ratchets changed";
        if (s->note[0] < t->tu_lo || s->note[0] > t->tu_hi)
            return "a note out of the register";
        for (k = 0; k < s->n; k++) {
            if (!in_scale(t, s->note[k]) || s->note[k] > t->tu_hi + 12u || s->note[k] < t->tu_lo)
                return "a note out of the scale (or a chord's out of the register)";
            for (j = 0; j < k; j++)
                if (s->note[j] == s->note[k])
                    return "the same note twice in a step";
        }
        if (moved)
            *moved += memcmp(s->note, o->note, sizeof s->note) != 0;
    }
    return 0;
}
static void run_passes(track_t *t, uint32_t n)      /* UI frames (the audio between) until t played n passes */
{
    uint32_t p = t->pass, guard = 20000;
    while (t->pass < p + n && guard--)
        frame();
}
static void turn(track_t *t, int16_t v)
{
    t->p[P_TURN] = v;
    turing_arm();
}

/* the drum groove of mutate_test.c, with two sounds of a kind (snare / clap, hat / open hat) */
static void groove(track_t *t, uint32_t len)
{
    uint32_t i;
    t->p[P_SLEN] = (int16_t)len;
    steps_clear(t);
    memset(t->cond, 0, sizeof t->cond);
    locks_clear(t);
    for (i = 0; i < len; i++) {
        if (i % 4u == 0u || i == 10u)
            dstep_set(&t->dstep[i], LANE_KICK, LV_NORM, 0);
        if (i % 8u == 4u)
            dstep_set(&t->dstep[i], LANE_SNARE, LV_NORM, 0);
        if (i % 2u == 0u && i != 14u)
            dstep_set(&t->dstep[i], LANE_HAT, i % 4u ? LV_SOFT : LV_NORM, i == 6u);
    }
    dstep_set(&t->dstep[14], LANE_OPEN, LV_NORM, 0);
    dstep_set(&t->dstep[12], LANE_CLAP, LV_HARD, 0);
    dstep_set(&t->dstep[40], LANE_BELL, LV_NORM, 0);        /* past LEN: never touched */
    t->cond[3] = CN_1_2;
    lock_set(t, 4, P_SLDEPTH, 50);
}
static const char *drum_bad(const track_t *t, const snap_t *a)
{
    static snap_t now;
    uint32_t len = trk_len(t), i, l, used = 0;
    snap(t, &now);
    if (memcmp(now.cond, a->cond, sizeof now.cond) || memcmp(now.lk, a->lk, sizeof now.lk))
        return "a condition or a lock changed";
    for (i = 0; i < len; i++)
        used |= dstep_mask((const dstep_t *)&a->st[i]);
    for (i = 0; i < NSTEP; i++) {
        const dstep_t *s = (const dstep_t *)&now.st[i], *o = (const dstep_t *)&a->st[i];
        if (i >= len) {
            if (memcmp(s, o, sizeof *s))
                return "a step past LEN changed";
            continue;
        }
        if (dstep_mask(s) & ~used)
            return "a sound the pattern did not use";
        if (i % 4u == 0u && (dstep_has(s, LANE_KICK) != dstep_has(o, LANE_KICK) || dstep_has(s, LANE_KICK2) != dstep_has(o, LANE_KICK2)))
            return "a kick on a beat came or went";
        for (l = 0; l < DRUM_LANES; l++)
            if (!dstep_has(s, l) && (dstep_lvl(s, l) || dstep_rat(s, l)))
                return "level / ratchet bits on a lane without a hit";
    }
    return 0;
}
static uint32_t hits_of(const track_t *t, uint32_t lanes)
{
    uint32_t i, n = 0, m;
    for (i = 0; i < trk_len(t); i++)
        for (m = dstep_mask(&t->dstep[i]) & lanes; m; m &= m - 1u)
            n++;
    return n;
}

int main(int argc, char **argv)
{
    static snap_t a, b, c;
    static uint32_t seen[128];
    track_t *t = &trk[0];
    const char *why = 0;
    uint32_t i, k, p, ok, moved;

    outdir = argc > 1 ? argv[1] : "build/host";
    memset(trk, 0, sizeof trk);
    memset(&song, 0, sizeof song);
    host_tracks_init();
    rng_state = 0x7A9Eu;

    /* ---- TURN 0: nothing */
    synth_pattern(t);
    snap(t, &a);
    undo.valid = 0;
    turn(t, 0);
    passes(t, 50);
    ck(same(t, &a) && !t->tu_arm && !undo.valid && TP[P_TURN].def == 0,
       "TURN 0 (the default): 50 passes, the pattern as it was, not armed, no undo taken");

    /* ---- TURN 100: every step with notes, every pass */
    turn(t, 100);
    ck(t->tu_arm && t->tu_lo == 50 && t->tu_hi == 69 && undo.valid && !undo.undone && undo.trk == 0 &&
       !memcmp(undo.st, a.st, sizeof a.st), "TURN up: armed, the register D3..A4 (the pattern's span), its undo step taken");
    for (p = 0, ok = 1; p < 200u && ok; p++) {
        snap(t, &b);
        moved = 0;
        pass(t);
        if ((why = synth_bad(t, &a, 0)) != 0)
            break;
        synth_bad(t, &b, &moved);                    /* (against the pass before: every step with notes moved) */
        ok &= moved == 7u;
        for (i = 0; i < 16u; i++)
            if (has_notes(&t->step[i]))
                seen[t->step[i].note[0]]++;
    }
    ck(!why && ok, why ? why : "TURN 100, 200 passes: each of the 7 steps with notes rewritten every pass, structure kept");
    for (i = 50, ok = 1, k = 0; i <= 69u; i++)
        if (in_scale(t, (int32_t)i))
            ok &= seen[i] > 20u, k++;
    ck(ok && k == 12u, "the walk reaches every note of D minor in the register (12 notes)");
    {   /* the shift register: one new bit a rewrite */
        uint16_t r0;
        step_t *s = &t->step[3];
        uint8_t was = s->note[0];
        for (i = 0, ok = 1; i < 200u; i++) {
            r0 = t->tu_reg;
            turing_step(t, 3);
            ok &= (uint16_t)(t->tu_reg >> 1) == (r0 & 0x7FFFu) && s->note[0] != was && in_scale(t, s->note[0]);
            was = s->note[0];
        }
        ck(ok, "a Turing machine: a rewrite shifts one random bit into the 16-bit register, a new note each time");
    }

    /* ---- the rates */
    {
        static const int16_t PCT[3] = {25, 50, 75};
        for (k = 0; k < 3u; k++) {
            char what[96];
            uint32_t n = 0, e;
            synth_pattern(t);
            snap(t, &a);
            turn(t, 0);
            turn(t, PCT[k]);
            for (p = 0; p < 600u; p++) {
                snap(t, &b);
                pass(t);
                synth_bad(t, &b, &n);
            }
            e = n * 1000u / (600u * 7u);
            why = synth_bad(t, &a, 0);
            snprintf(what, sizeof what, "TURN %d %%: %u.%u %% of the steps with notes rewritten (within 4 %%), invariants",
                     PCT[k], e / 10u, e % 10u);
            ck(!why && e + 40u > (uint32_t)PCT[k] * 10u && e < (uint32_t)PCT[k] * 10u + 40u, what);
        }
    }

    /* ---- the same seed: the same run */
    {
        uint32_t h1, h2;
        synth_pattern(t);
        turn(t, 0);
        rng_state = 99u;
        turn(t, 40);
        passes(t, 100);
        h1 = pattern_sum(t);
        synth_pattern(t);
        turn(t, 0);
        rng_state = 99u;
        turn(t, 40);
        passes(t, 100);
        h2 = pattern_sum(t);
        synth_pattern(t);
        turn(t, 0);
        rng_state = 98u;
        turn(t, 40);
        passes(t, 100);
        ck(h1 == h2 && h1 != pattern_sum(t), "the same seed: the same 100 passes (another seed: others)");
    }

    /* ---- chords: CHORD on: the chord of the new note; off: its shape; scales, a narrow register */
    {
        static const uint8_t CEG[3] = {60, 64, 67}, CE[2] = {60, 64}, G4[1] = {67}, CEGB[4] = {48, 52, 55, 59};
        track_t *u = &trk[1];
        uint8_t ch[4];
        u->p[P_ROOT] = 0;
        u->p[P_SCALE] = 1;
        u->p[P_SLEN] = 8;
        u->p[P_CHORD] = 1;                               /* TRIAD */
        steps_clear(u);
        note(u, 0, 3, CEG, ST_NOTE, 0, 0);
        note(u, 2, 2, CE, ST_NOTE, LV_SOFT | LV_HARD << 2, 0);
        note(u, 5, 1, G4, ST_NOTE, 0, 0);
        snap(u, &a);
        turn(u, 100);
        for (p = 0, ok = 1; p < 300u && ok; p++) {
            pass(u);
            ok &= !synth_bad(u, &a, 0) && chord_notes(u, u->step[0].note[0], ch) == 3u && !memcmp(u->step[0].note, ch, 3) &&
                  chord_notes(u, u->step[2].note[0], ch) >= 2u && !memcmp(u->step[2].note, ch, 2) && u->step[5].n == 1u;
        }
        ck(ok, "CHORD TRIAD, C major: a triad stays the triad of its new note, a dyad its first two, a note one");
        turn(u, 0);                                      /* (down before the pattern changes, as on the device) */
        u->p[P_CHORD] = 0;
        u->p[P_SCALE] = 0;                               /* CHR: semitones */
        steps_clear(u);
        note(u, 0, 4, CEGB, ST_NOTE, 0, 0x55);
        note(u, 4, 2, CE, ST_NOTE, 0, 0);
        snap(u, &a);
        turn(u, 100);
        for (p = 0, ok = 1; p < 300u && ok; p++) {
            pass(u);
            why = synth_bad(u, &a, 0);
            ok &= !why && u->step[4].note[1] - u->step[4].note[0] == 4;
        }
        ck(ok && u->tu_lo == 48u && u->tu_hi == 64u, why ? why : "CHORD off, CHR: a 4-note chord keeps 4 notes, a major third its shape");
        turn(u, 0);
        u->p[P_SCALE] = 5;                               /* PEN, one note over and over: an octave around it */
        u->p[P_SLEN] = 16;
        steps_clear(u);
        for (i = 0; i < 16u; i += 4u)
            note(u, i, 1, G4, ST_NOTE, 0, 0);
        snap(u, &a);
        turn(u, 100);
        for (p = 0, ok = u->tu_lo == 61u && u->tu_hi == 73u; p < 300u && ok; p++) {
            pass(u);
            why = synth_bad(u, &a, 0);
            ok &= !why;
        }
        ck(ok, why ? why : "PEN, one note (G4): the register an octave around it (C#4..C#5), the notes in it");
        turn(u, 0);
    }

    /* ---- the drum track */
    {
        track_t *d = TDRUM;
        uint32_t h0, sn0, ht0, h = 0, sn = 0, ht = 0, moves = 0, gone = 0, came = 0, changed = 0;
        rng_state = 0xD5u;
        groove(d, 16);
        snap(d, &a);
        turn(d, 0);
        passes(d, 20);
        ck(same(d, &a) && !d->tu_arm, "drums, TURN 0: 20 passes, the groove as it was");
        h0 = hits_of(d, 0xFFFFu);
        sn0 = hits_of(d, 1u << LANE_SNARE | 1u << LANE_CLAP);
        ht0 = hits_of(d, 1u << LANE_HAT | 1u << LANE_OPEN);
        turn(d, 100);
        for (p = 0; p < 3000u; p++) {
            snap(d, &b);
            pass(d);
            if ((why = drum_bad(d, &a)) != 0)
                break;
            changed += !same(d, &b);
            h += hits_of(d, 0xFFFFu);
            sn += hits_of(d, 1u << LANE_SNARE | 1u << LANE_CLAP);
            ht += hits_of(d, 1u << LANE_HAT | 1u << LANE_OPEN);
            for (i = 0; i < 16u; i++) {
                uint32_t o = dstep_mask((const dstep_t *)&b.st[i]), n = dstep_mask(&d->dstep[i]);
                moves += (o & ~n) && (n & ~o);
                gone += (o & ~n) && !(n & ~o);
                came += !(o & ~n) && (n & ~o);
            }
        }
        ck(!why, why ? why : "drums, TURN 100, 3000 passes: kicks on the beats, LEN, sounds used, bits, conditions, locks");
        ck(changed > 2900u && moves > 1000u && gone > 1000u && came > 1000u,
           "drums: the groove changes every pass; hits come, go and move to a sound of their kind");
        ck(h > 3000u * h0 * 6u / 10u && h < 3000u * h0 * 15u / 10u && sn > 3000u * sn0 / 2u && sn < 3000u * sn0 * 2u &&
           ht > 3000u * ht0 / 2u && ht < 3000u * ht0 * 2u, "drums: as busy as the groove (all, the snares, the hats: near their share)");
        turn(d, 0);
        snap(d, &b);
        passes(d, 10);
        ck(same(d, &b), "drums, TURN back to 0: what plays stays");
    }

    /* ---- not while recording */
    synth_pattern(t);
    snap(t, &a);
    turn(t, 0);
    song.rec = 1u;
    turn(t, 100);
    passes(t, 10);
    ck(!t->tu_arm && same(t, &a), "recording track 1: TURN 100 not armed, nothing rewritten");
    song.rec = 0;
    turing_arm();
    ck(t->tu_arm, "the take over: armed");
    song.rec = 1u;                                       /* (REC before the UI sees it: the ISR checks too) */
    passes(t, 10);
    ck(same(t, &a), "recording started, armed: nothing rewritten");
    turing_arm();
    ck(!t->tu_arm, "... and the UI turns it off");
    song.rec = 0;
    turn(t, 0);

    /* ---- what TURN is: no lock, a pattern parameter (presets keep it) */
    ck(!p_lockable(P_TURN) && !lock_set(t, 3, P_TURN, 5) && !lock_set(TDRUM, 3, P_TURN, 5), "TURN cannot be locked on a step (synth, drums)");
    t->p[P_TURN] = 63;
    set_engine_of(t, 0);
    apply_preset_to(t, 3);
    ck(t->p[P_TURN] == 63 && param_kept(P_TURN), "a preset keeps TURN (a pattern parameter)");
    turn(t, 0);

    /* ---- the gesture and the undo, on the real UI */
    {
        static snap_t orig, f, t1;
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
        synth_pattern(t);
        t->p[P_SDIV] = 3;
        snap(t, &orig);
        undo_mark(TDRUM, (undo_sess += 4u) | 3u);            /* (an older undo step, of another track) */
        for (i = 0; i < 5u && strcmp(cur_page()->title, "PATTERN 2"); i++)
            open_family(FAM_SEQ);
        ui.force = 1;
        frame();
        ppm("page-pattern2");
        ck(!strcmp(cur_page()->title, "PATTERN 2") && cur_page()->id[0] == P_TURN, "SEQ tapped: STEP, PATTERN, PATTERN 2 (TURN on KNOB 1)");
        encs[panel.enc[EN_K1]] = 2;
        frame();
        frame();
        ck(t->p[P_TURN] > 0 && t->tu_arm && undo.trk == 0 && !memcmp(undo.st, orig.st, sizeof orig.st),
           "KNOB 1 up: TURN up, armed, the pattern before it is the undo step");
        encs[panel.enc[EN_K1]] = 60;
        frame();
        transport_req = 1;
        run_passes(t, 6);
        ck(!same(t, &orig) && !synth_bad(t, &orig, 0), "PLAY: the pattern turns as it plays (in the scale, the register, its structure)");
        encs[panel.enc[EN_K1]] = -100;
        frame();
        frame();
        snap(t, &f);
        run_passes(t, 4);
        ck(t->p[P_TURN] == 0 && !t->tu_arm && same(t, &f), "KNOB 1 down to 0: what you hear stays (frozen), playing on");
        encs[panel.enc[EN_K1]] = 30;
        frame();
        frame();
        ck(t->tu_arm && !memcmp(undo.st, orig.st, sizeof orig.st), "up again: the same undo step (the pattern before the first TURN)");
        run_passes(t, 4);
        encs[panel.enc[EN_K1]] = -100;
        frame();
        transport_req = 2;
        frame();
        snap(t, &t1);
        ck(!song.playing && !same(t, &orig), "TURN 0, stopped: the turned pattern kept");
        press(B_EDIT);
        frames(10);
        edges_btn |= BT(B_OCTDN); fm1_in.buttons |= BT(B_OCTDN); frame(); fm1_in.buttons &= ~BT(B_OCTDN); frame();
        ck(same(t, &orig), "EDIT + OCT-: the pattern from before TURN went up, exactly (conditions, locks)");
        edges_btn |= BT(B_OCTUP); fm1_in.buttons |= BT(B_OCTUP); frame(); fm1_in.buttons &= ~BT(B_OCTUP); frame();
        ck(same(t, &t1), "EDIT + OCT+: the turned one again");
        release(B_EDIT);
        frames(16);                                          /* (KNOB 1..4 quiet ~250 ms after a layer: #39) */
        t->step[2] = t->step[3];                             /* another edit: it takes the undo */
        undo_mark(t, (undo_sess += 4u) | 3u);
        t->step[2].note[0] = 62;
        snap(t, &c);
        for (i = 0; i < 5u && strcmp(cur_page()->title, "PATTERN 2"); i++)
            open_family(FAM_SEQ);
        encs[panel.enc[EN_K1]] = 10;
        frame();
        frame();
        ck(t->tu_arm && !memcmp(undo.st, c.st, sizeof c.st), "after another edit, TURN up: the undo step is the pattern now");
        encs[panel.enc[EN_K1]] = -100;
        frame();
    }

    printf(bad ? "turing: %d FAILED\n" : "turing: all ok\n", bad);
    return bad != 0;
}
