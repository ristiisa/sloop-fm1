/* SPDX-License-Identifier: GPL-3.0-only */
/* DICE (EDIT + PRESETS, seq.c dice.c) on the real generator, and the gesture on the real UI (the harness
 * of ui_pages_test.c):
 *   drums  every style, LEN 16 / 32 / 64 / 12 / 24: thousands of rolls keep the step layout (no level or
 *          ratchet without its hit), LEN (the steps, conditions and locks past it stay; within it no
 *          condition, no lock), the kick on the 1 of every bar and the style's signature (HOUSE a kick on
 *          every beat, TRAP the snare / clap on 3, BOOM BAP / BREAK / DNB the snare on 2 and 4, AMAPIANO
 *          the shaker on every 16th); the rolls differ, ghosts and ratchets come
 *   synth  bass, melody, chords (CHORD on, POLY), mono with CHORD on, scales and keys: the notes in the
 *          scale and the register, <= 4 a step (mono: 1), the chords the scale's, ties after a note, no
 *          ratchet held into a tie, a slide into a note; a bass sound around C2, the notes there were kept
 *   back   20 rolls, 15 turned back: each roll again exactly, then the pattern before the first (its
 *          conditions and locks); a pattern or key changed since: nothing to turn back
 *   UI     EDIT + PRESETS: a roll (DICE TRAP 1 on the 808 kit), each one undo (OCT- / OCT+ exactly),
 *          left: the roll before; EDIT + ALGORITHM: the style; the same seed: the same roll
 * Exit status: the number of failed checks. */
#define UI_TEST_MAIN ui_main
#include "ui_pages_test.c"

static int bad;
static void ck(int ok, const char *what)
{
    printf("dice: %-82s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

typedef struct { step_t st[NSTEP]; uint8_t cond[NSTEP]; plk_t lk[PLK_MAX]; } snap_t;
static void snap(const track_t *t, snap_t *s)
{
    memcpy(s->st, t->step, sizeof s->st);
    memcpy(s->cond, t->cond, sizeof s->cond);
    plk_save(t, s->lk);
}
static int same(const track_t *t, const snap_t *s)
{
    static snap_t now;
    snap(t, &now);
    return !memcmp(&now, s, sizeof now);
}
/* a pattern to roll over: conditions and locks within LEN and past it */
static void setup(track_t *t, uint32_t len)
{
    t->p[P_SLEN] = (int16_t)len;
    steps_clear(t);
    if (is_drum(t)) {
        dstep_set(&t->dstep[0], LANE_KICK, LV_NORM, 0);
        dstep_set(&t->dstep[2], LANE_HAT, LV_SOFT, 1);
        dstep_set(&t->dstep[63], LANE_BELL, LV_HARD, 2);
    } else {
        static const uint8_t N[2] = {60, 64};
        put_step(t, 0, 1, N, ST_NOTE, 0);
        put_step(t, 2, 2, N, ST_NOTE, 0);
        put_step(t, 63, 1, N + 1, ST_NOTE, 0);
    }
    t->cond[2] = CN_P50;
    t->cond[63] = CN_FILL;
    plk_set(t, 2, is_drum(t) ? P_SLDEPTH : P_REV, 90);
    plk_set(t, 63, is_drum(t) ? P_SLDEPTH : P_REV, 10);
}
static const char *past_len(const track_t *t, const snap_t *init)
{
    uint32_t len = trk_len(t), i;
    for (i = len; i < NSTEP; i++)
        if (memcmp(&t->step[i], &init->st[i], sizeof init->st[i]) || t->cond[i] != init->cond[i])
            return "a step past LEN changed (or its condition)";
    if (len < 64u && plk_count(t, 63) != 1u)
        return "a lock past LEN went";
    for (i = 0; i < len; i++)
        if (t->cond[i] || plk_count(t, i))
            return "a condition or a lock left within LEN";
    return 0;
}

/* ---- drums */
typedef struct { uint32_t ghosts, rats, hard, distinct, repeats, hits; } dstat_t;
static const char *drum_bad(const track_t *t, uint32_t style, const snap_t *init, dstat_t *st)
{
    uint32_t len = trk_len(t), i, l;
    const char *why = past_len(t, init);
    if (why)
        return why;
    for (i = 0; i < len; i++) {
        const dstep_t *s = &t->dstep[i];
        uint32_t b = i % 16u;
        for (l = 0; l < DRUM_LANES; l++) {
            if (!dstep_has(s, l)) {
                if (dstep_lvl(s, l) || dstep_rat(s, l))
                    return "a level / ratchet on a lane without a hit";
                continue;
            }
            st->hits++;
            st->ghosts += dstep_lvl(s, l) == LV_GHOST;
            st->hard += dstep_lvl(s, l) == LV_HARD;
            st->rats += dstep_rat(s, l) != 0u;
        }
        if (!b && !dstep_has(s, LANE_KICK))
            return "no kick on the 1 of a bar";
        switch (style) {
        case DS_HOUSE:
            if (b % 4u == 0u && !dstep_has(s, LANE_KICK))
                return "HOUSE: a beat without its kick";
            break;
        case DS_TRAP:
            if (b == 8u && !dstep_has(s, LANE_SNARE) && !dstep_has(s, LANE_CLAP))
                return "TRAP: no snare / clap on 3";
            break;
        case DS_BOOMBAP:
        case DS_BREAK:
        case DS_DNB:
            if ((b == 4u || b == 12u) && (!dstep_has(s, LANE_SNARE) || dstep_lvl(s, LANE_SNARE) == LV_GHOST))
                return "the snare on 2 and 4 missing";
            break;
        case DS_AMAPIANO:
            if (!dstep_has(s, LANE_SHAKER))
                return "AMAPIANO: a 16th without its shaker";
            break;
        default:
            break;
        }
    }
    return 0;
}

static int cmp32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
static uint32_t distinct(uint32_t *h, uint32_t n)
{
    uint32_t i, d = n > 0;
    qsort(h, n, sizeof *h, cmp32);
    for (i = 1; i < n; i++)
        d += h[i] != h[i - 1u];
    return d;
}

#define ROLLS 2000u
static void drum_style(uint32_t style)
{
    static const uint8_t LENS[5] = {16, 32, 64, 12, 24};
    static uint32_t h[ROLLS];
    static snap_t init;
    track_t *t = TDRUM;
    dstat_t st;
    const char *why = 0;
    uint32_t k, r, prev = 0;
    char what[120];
    memset(&st, 0, sizeof st);
    for (k = 0; k < 5u && !why; k++) {
        setup(t, LENS[k]);
        snap(t, &init);
        for (r = 0; r < ROLLS && !why; r++) {
            dice_roll(t, style, 0);
            why = drum_bad(t, style, &init, &st);
            h[r] = pattern_sum(t);
            st.repeats += !k && r && h[r] == prev;
            prev = h[r];
        }
        if (k == 0u)
            st.distinct = distinct(h, ROLLS);
    }
    snprintf(what, sizeof what, "%s: %u rolls x LEN 16 32 64 12 24: layout, LEN, the 1, its signature", N_DICE[style], ROLLS);
    ck(!why, why ? why : what);
    snprintf(what, sizeof what, "%s: rolls differ (%u of %u at LEN 16, %u twice in a row)", N_DICE[style], st.distinct, ROLLS, st.repeats);
    ck(st.distinct > ROLLS / 2u && st.repeats < ROLLS / 50u, what);
    snprintf(what, sizeof what, "%s: levels and ratchets (ghosts %u, hard %u, ratchets %u)", N_DICE[style], st.ghosts, st.hard, st.rats);
    ck(st.ghosts > 100u && st.hard > 100u && st.rats > 20u, what);
}

/* ---- synth parts */
static uint32_t role_of(const track_t *t) { return t->p[P_CHORD] && t->p[P_VOICE] == V_POLY ? 2u : dice.bass ? 0u : 1u; }
typedef struct { uint32_t notes, ties, slides, rats, soft, chords; } sstat_t;
static const char *synth_bad(const track_t *t, const snap_t *init, sstat_t *st)
{
    uint32_t len = trk_len(t), i, j, k, role = role_of(t), mask = dice_mask(t), any = 0;
    const char *why = past_len(t, init);
    if (why)
        return why;
    for (i = 0; i < len; i++) {
        const step_t *s = &t->step[i];
        if (s->time > ST_REST)
            return "a step time out of range";
        if (s->time != ST_NOTE) {
            if (s->n || s->lvl || s->rat || (s->flags & ~SF_SLIDE) || (s->time == ST_REST && s->flags))
                return "a TIE / REST that holds notes, levels or flags";
            if (s->time == ST_TIE && (!i || t->step[i - 1u].time == ST_REST))
                return "a TIE after no note";
            st->ties += s->time == ST_TIE;
        } else {
            uint8_t c[4];
            any = 1;
            if (!s->n || s->n > 4u || (t->p[P_VOICE] != V_POLY && s->n != 1u))
                return "a note step with no notes, more than 4, or a chord on a mono part";
            if (s->n < 4u && ((s->lvl | s->rat) >> (2u * s->n)))
                return "level / ratchet bits past the step's notes";
            for (k = 0; k < s->n; k++) {
                for (j = 0; j < k; j++)
                    if (s->note[j] == s->note[k])
                        return "the same note twice in a step";
                if (t->p[P_CHORD] != 5 && !((mask >> (uint32_t)((s->note[k] - t->p[P_ROOT] + 120) % 12)) & 1u))
                    return "a note out of the scale";
            }
            if (role == 2u) {
                if (s->n != chord_notes(t, s->note[0], c) || memcmp(c, s->note, s->n))
                    return "a chord that is not the scale's on its root";
                if (s->note[0] < dice.lo || s->note[0] > dice.lo + 11)
                    return "a chord root out of its octave";
                st->chords++;
            } else if (s->note[0] < dice.lo || s->note[0] > dice.hi) {
                return "a note out of the register";
            }
            if (s->rat && i + 1u < len && t->step[i + 1u].time == ST_TIE)
                return "a ratchet held into a tie";
            st->notes++;
            st->rats += s->rat != 0u;
            st->soft += (s->lvl & 3u) == LV_SOFT || (s->lvl & 3u) == LV_GHOST;
        }
        if (s->flags & SF_SLIDE) {
            if (i + 1u >= len || t->step[i + 1u].time != ST_NOTE || s->rat)
                return "a slide not into a note";
            st->slides++;
        }
    }
    return any ? 0 : "no note";
}

static void synth_case(uint32_t ti, uint32_t root, uint32_t scale, uint32_t voice, uint32_t chord, uint32_t len, int bass,
                       int keep, int32_t trans, const char *name)
{
    static snap_t init;
    track_t *t = &trk[ti];
    sstat_t st;
    const char *why = 0;
    uint32_t style, r;
    char what[140];
    t->p[P_ROOT] = (int16_t)root;
    t->p[P_SCALE] = (int16_t)scale;
    t->p[P_VOICE] = (int16_t)voice;
    t->p[P_CHORD] = (int16_t)chord;
    t->p[P_TRANS] = (int16_t)trans;
    setup(t, len);
    if (!keep)
        for (r = 0; r < len; r++)
            step_clear(&t->step[r]);
    snap(t, &init);
    memset(&st, 0, sizeof st);
    dice.n = 0;                                    /* (a new journal: the register of this pattern) */
    for (style = DS_HOUSE; style < DS_COUNT && !why; style++)
        for (r = 0; r < 500u && !why; r++) {
            dice_roll(t, style, bass);
            why = synth_bad(t, &init, &st);
        }
    snprintf(what, sizeof what, "%s: 7 styles x 500 rolls: scale, register %u..%u, <= 4, ties, slides", name, dice.lo, dice.hi);
    ck(!why, why ? why : what);
    snprintf(what, sizeof what, "%s: notes %u, ties %u, slides %u, ratchets %u, soft %u, chords %u", name, st.notes, st.ties,
             st.slides, st.rats, st.soft, st.chords);
    ck(st.notes > 3000u && st.ties > 300u && st.soft > 300u && (role_of(t) != 2u || st.chords == st.notes) &&
       (role_of(t) == 2u || st.slides > 50u), what);
}

static void back_case(track_t *t, uint32_t len, const char *name)
{
    static snap_t orig, r[20];
    uint32_t i, ok;
    char what[120];
    setup(t, len);
    snap(t, &orig);
    dice.n = 0;
    for (i = 0; i < 20u; i++) {
        dice_roll(t, DS_HOUSE + i % 6u, 0);
        snap(t, &r[i]);
    }
    ok = dice_depth(t) == DICE_DEPTH;
    for (i = 0; i < 15u; i++)
        ok &= dice_back(t) == 1 && same(t, &r[18u - i]) && dice_depth(t) == 15u - i;
    snprintf(what, sizeof what, "%s: 20 rolls, 15 turned back: each roll again exactly, in order", name);
    ck(ok, what);
    ok = dice_back(t) == 1 && same(t, &orig) && !dice_depth(t);
    snprintf(what, sizeof what, "%s: one more: the pattern before the first roll (its conditions and locks)", name);
    ck(ok, what);
    ck(!dice_back(t) && same(t, &orig), "... and nothing more: the pattern stays");
}

int main(int argc, char **argv)
{
    static snap_t a, b;
    uint32_t s, ok, i;
    int16_t sc;
    track_t *t;

    outdir = argc > 1 ? argv[1] : "build/host";
    memset(trk, 0, sizeof trk);
    memset(&song, 0, sizeof song);
    host_tracks_init();
    rng_state = 0xD1CEu;

    for (s = DS_HOUSE; s < DS_COUNT; s++)
        drum_style(s);

    synth_case(0, 2, 2, V_POLY, 0, 16, 1, 0, 0, "bass, D minor, LEN 16");
    ck(dice.bass && dice.lo >= 30u && dice.hi <= 55u, "... an empty bass part: around C2 .. E3");
    synth_case(1, 9, 6, V_LEGATO, 0, 32, 0, 1, 0, "melody, A minor pentatonic, LEGATO, LEN 32, notes there");
    ck(!dice.bass && dice.lo <= 60u && dice.hi >= 64u && dice.lo >= 45u, "... the register around the notes there were (C4 .. E4)");
    synth_case(2, 0, 1, V_POLY, 2, 64, 0, 0, 0, "7TH chords, C major, LEN 64");
    synth_case(0, 0, 0, V_POLY, 1, 12, 0, 0, 0, "triads, CHR (minor), LEN 12");
    synth_case(1, 4, 12, V_MONO, 3, 24, 0, 0, 0, "MONO, CHORD 9TH on, E blues, LEN 24");
    synth_case(2, 6, 9, V_POLY, 5, 16, 0, 0, 0, "POWER chords, F# lydian, LEN 16");
    synth_case(1, 5, 2, V_LEGATO, 0, 16, 0, 0, -24, "a TRANSPOSE -24 sound (808), F minor");
    ck(dice.bass && dice.hi <= 55u, "... a bass, where its keys play");

    back_case(TDRUM, 32, "drums");
    trk[0].p[P_CHORD] = 2;
    back_case(&trk[0], 16, "chords");
    trk[0].p[P_CHORD] = 0;
    back_case(&trk[0], 64, "melody");
    t = &trk[0];
    dice_roll(t, DS_TRAP, 1);
    t->step[3].time = ST_NOTE, t->step[3].n = 1, t->step[3].note[0] = 62, t->step[3].lvl = 0;
    snap(t, &a);
    ck(!dice_depth(t) && !dice_back(t) && same(t, &a), "the pattern changed since (an edit, undo, mutate): nothing to turn back");
    dice_roll(t, DS_TRAP, 1);
    sc = t->p[P_SCALE];
    t->p[P_SCALE] = (int16_t)(sc + 1);
    ck(!dice_depth(t), "the key changed since: nothing to turn back");
    t->p[P_SCALE] = sc;
    ck(dice_depth(t) == 1u, "... back as it was: the roll turns back again");

    /* the same seed: the same roll */
    setup(TDRUM, 16);
    rng_state = 4242u;
    dice.n = 0;
    dice_roll(TDRUM, DS_BREAK, 0);
    snap(TDRUM, &a);
    setup(TDRUM, 16);
    rng_state = 4242u;
    dice.n = 0;
    dice_roll(TDRUM, DS_BREAK, 0);
    snap(TDRUM, &b);
    ok = !memcmp(&a, &b, sizeof a);
    setup(TDRUM, 16);
    rng_state = 4243u;
    dice.n = 0;
    dice_roll(TDRUM, DS_BREAK, 0);
    ck(ok && !same(TDRUM, &a), "the same seed: the same roll (another seed: another)");

    {   /* ---- the gesture, on the real UI: EDIT + PRESETS / ALGORITHM; OCT- / OCT+ */
        static snap_t orig, r1, r2;
        panel = PANEL_DEFAULT;
        layers_init();
        host_tracks_init();
        for (i = 0; i < NPART; i++) {
            set_engine_of(&trk[i], TRK_DEF[i][0]);
            apply_preset_to(&trk[i], TRK_DEF[i][1]);
            trk[i].engine = trk[i].eng_req;
        }
        TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;                 /* the 808: DS_KIT is TRAP */
        song.sel = TRK_DRUM;
        go_home();
        frame();
        setup(TDRUM, 16);
        snap(TDRUM, &orig);
        press(B_EDIT);
        frames(10);
        encs[panel.enc[EN_PRESET]] = 1;
        frame();
        snap(TDRUM, &r1);
        ck(!same(TDRUM, &orig) && !strcmp(ui.msg, "DICE TRAP 1"), "EDIT + PRESETS right on the 808 kit: a new groove, DICE TRAP 1");
        encs[panel.enc[EN_PRESET]] = 1;
        frame();
        snap(TDRUM, &r2);
        ck(!same(TDRUM, &r1) && !strcmp(ui.msg, "DICE TRAP 2"), "again: another, DICE TRAP 2");
        edges_btn |= BT(B_OCTDN); fm1_in.buttons |= BT(B_OCTDN); frame(); fm1_in.buttons &= ~BT(B_OCTDN); frame();
        ck(same(TDRUM, &r1), "EDIT + OCT-: the last roll undone (one roll, one undo), exactly");
        edges_btn |= BT(B_OCTUP); fm1_in.buttons |= BT(B_OCTUP); frame(); fm1_in.buttons &= ~BT(B_OCTUP); frame();
        ck(same(TDRUM, &r2), "EDIT + OCT+: redo, exactly");
        encs[panel.enc[EN_PRESET]] = -1;
        frame();
        ck(same(TDRUM, &r1) && !strcmp(ui.msg, "DICE TRAP 1"), "PRESETS left: the roll before, DICE TRAP 1");
        encs[panel.enc[EN_PRESET]] = -1;
        frame();
        ck(same(TDRUM, &orig) && !strcmp(ui.msg, "DICE 0"), "again: the groove before the first roll (conditions, locks), DICE 0");
        encs[panel.enc[EN_PRESET]] = -1;
        frame();
        ck(same(TDRUM, &orig), "again: nothing more");
        encs[panel.enc[EN_ALGO]] = 1;
        frame();
        ck(dice_pick == DS_HOUSE && !strcmp(ui.msg, "STYLE HOUSE") && song.sel == TRK_DRUM, "EDIT + ALGORITHM: the style (HOUSE), not the track");
        encs[panel.enc[EN_PRESET]] = 1;
        frame();
        ck(!strcmp(ui.msg, "DICE HOUSE 1") && dstep_has(&TDRUM->dstep[4], LANE_KICK) && dstep_has(&TDRUM->dstep[12], LANE_KICK),
           "PRESETS: a HOUSE groove, DICE HOUSE 1");
        encs[panel.enc[EN_ALGO]] = -3;
        frame();
        ck(dice_pick == DS_KIT && !strcmp(ui.msg, "STYLE KIT: TRAP"), "ALGORITHM back: KIT (the 808: TRAP)");
        release(B_EDIT);
        ck(ui.layer == LY_PLAY && cur_page()->scope == SC_TRK, "EDIT let go: no EDIT page (the hold was used)");
        song.sel = 0;                                       /* 808 BOOM: a bass */
        go_home();
        frame();
        steps_clear(&trk[0]);
        press(B_EDIT);
        frames(10);
        encs[panel.enc[EN_PRESET]] = 1;
        frame();
        release(B_EDIT);
        for (i = 0, ok = 1, s = 0; i < trk_len(&trk[0]); i++)
            if (trk[0].step[i].time == ST_NOTE) {
                s++;
                ok &= trk[0].step[i].note[0] <= 55u && trk[0].step[i].note[0] >= 30u;
            }
        ck(s && ok && dice.bass, "track 1 (808 BOOM): a bass line around C2");
        song.sel = 1;                                       /* RHODES */
        go_home();
        frame();
        steps_clear(&trk[1]);
        press(B_EDIT);
        frames(10);
        encs[panel.enc[EN_PRESET]] = 1;
        frame();
        release(B_EDIT);
        for (i = 0, ok = 1, s = 0; i < trk_len(&trk[1]); i++)
            if (trk[1].step[i].time == ST_NOTE) {
                s++;
                ok &= trk[1].step[i].note[0] >= 50u && trk[1].step[i].note[0] <= 80u;
            }
        ck(s && ok && !dice.bass, "track 2 (RHODES): a melody around C4");
    }

    printf(bad ? "dice: %d FAILED\n" : "dice: all ok\n", bad);
    return bad != 0;
}
