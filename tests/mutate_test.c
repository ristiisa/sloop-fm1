/* SPDX-License-Identifier: GPL-3.0-only */
/* MUTATE (EDIT + KNOB 4), on the real seq.c mutate / mutate_back: thousands of passes on synth parts
 * (scales, chords, ties, mono) and on the drum track keep every invariant (notes in the scale and in
 * range, at most 4 a step, LEN, the level / ratchet bits, whole ties, the kicks on the beats); a pass
 * changes a little; turned back, each pass is undone exactly; the same seed, the same passes.
 * Build with the same generated headers and flags as hostsim.c. */
#define main hostsim_main
#include "hostsim.c"
#undef main

static int bad;
static void check(const char *what, int ok)
{
    printf("mutate: %-68s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

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
static uint32_t span(const track_t *t)
{
    uint32_t i, k, lo = 127, hi = 0;
    for (i = 0; i < trk_len(t); i++)
        for (k = 0; t->step[i].time == ST_NOTE && k < t->step[i].n; k++) {
            lo = t->step[i].note[k] < lo ? t->step[i].note[k] : lo;
            hi = t->step[i].note[k] > hi ? t->step[i].note[k] : hi;
        }
    return hi >= lo ? hi - lo : 0u;
}
static uint32_t changed(const step_t *a, const step_t *b)
{
    uint32_t i, n = 0;
    for (i = 0; i < NSTEP; i++)
        n += memcmp(&a[i], &b[i], sizeof a[i]) != 0;
    return n;
}

/* a synth part against its first pattern: what must always hold */
static const char *synth_bad(const track_t *t, const step_t *init, uint32_t span0)
{
    uint32_t len = trk_len(t), i, j, k;
    for (i = len; i < NSTEP; i++)
        if (memcmp(&t->step[i], &init[i], sizeof init[i]))
            return "a step past LEN changed";
    for (i = 0; i < len; i++) {
        const step_t *s = &t->step[i];
        uint32_t most = init[i].time == ST_NOTE && init[i].n > 1u ? init[i].n : 1u;
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
            if (s->note[k] > 127u || !in_scale(t, s->note[k]))
                return "a note out of the scale";
            for (j = 0; j < k; j++)
                if (s->note[j] == s->note[k])
                    return "the same note twice in a step";
        }
    }
    if (span(t) > (span0 > 12u ? span0 : 12u))
        return "the notes spread over more than an octave (or their span)";
    return 0;
}
/* a pitch moved: one degree of the scale (no note of the scale between) */
static int one_degree(const track_t *t, int32_t a, int32_t b)
{
    int32_t m, lo = a < b ? a : b, hi = a < b ? b : a;
    if (a == b)
        return 0;
    for (m = lo + 1; m < hi; m++)
        if (in_scale(t, m))
            return 0;
    return 1;
}

typedef struct { uint32_t moves, levels, rats, adds, removes, zero, most, backs, wrong_move; } stats_t;
/* n passes on t (every 5th turned back at once, compared, then passed again); 0 = every pass held */
static const char *synth_run(track_t *t, uint32_t n, stats_t *st)
{
    static step_t init[NSTEP], before[NSTEP];
    uint32_t p, i, k, c, span0 = span(t);
    const char *why;
    memcpy(init, t->step, sizeof init);
    memset(st, 0, sizeof *st);
    for (p = 0; p < n; p++) {
        memcpy(before, t->step, sizeof before);
        mutate(t);
        c = changed(before, t->step);
        st->most = c > st->most ? c : st->most;
        if (c > MUT_STEPS)
            return "a pass changed more than MUT_STEPS steps";
        st->zero += !c;
        for (i = 0; i < NSTEP; i++) {
            const step_t *a = &before[i], *b = &t->step[i];
            if (!memcmp(a, b, sizeof *a))
                continue;
            if (!(a->time == ST_NOTE && a->n) && b->time == ST_NOTE && b->n)
                st->adds++;
            else if (a->time == ST_NOTE && a->n && !(b->time == ST_NOTE && b->n))
                st->removes++;
            else
                for (k = 0; k < a->n; k++) {
                    if (a->note[k] != b->note[k]) {
                        st->moves++;
                        st->wrong_move += !one_degree(t, a->note[k], b->note[k]);
                    }
                    st->levels += ((a->lvl ^ b->lvl) >> (2u * k) & 3u) != 0;
                    st->rats += ((a->rat ^ b->rat) >> (2u * k) & 3u) != 0;
                }
        }
        if ((why = synth_bad(t, init, span0)) != 0)
            return why;
        if (p % 5u == 4u) {
            if (!mutate_back(t) || memcmp(before, t->step, sizeof before))
                return "turned back: not the pattern before the pass";
            st->backs++;
            mutate(t);
            if ((why = synth_bad(t, init, span0)) != 0)
                return why;
        }
    }
    return 0;
}

static track_t *synth_setup(uint32_t i, uint32_t root, uint32_t scale, uint32_t voice, uint32_t len)
{
    track_t *t = &trk[i];
    t->p[P_ROOT] = (int16_t)root;
    t->p[P_SCALE] = (int16_t)scale;
    t->p[P_VOICE] = (int16_t)voice;
    t->p[P_SLEN] = (int16_t)len;
    steps_clear(t);
    return t;
}

/* the drum track against its first pattern */
static const char *drum_bad(const track_t *t, const dstep_t *init)
{
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
            if ((l == LANE_KICK || l == LANE_KICK2) && i % 4u == 0u && dstep_has(&init[i], l) && !dstep_has(s, l))
                return "a kick on the beat went";
            if (dstep_rat(s, l) && !((MUT_HATS >> l) & 1u))       /* (the grooves start without ratchets) */
                return "a ratchet on a lane that is not a hat";
        }
    }
    if (used & ~used0)
        return "a sound the pattern did not use";
    return 0;
}
typedef struct { uint32_t ghosts, gone, moved, levels, rats, zero, most; } dstats_t;
static const char *drum_run(track_t *t, uint32_t n, dstats_t *st)
{
    static dstep_t init[NSTEP], before[NSTEP];
    uint32_t p, i, l, c, len = trk_len(t);
    const char *why;
    memcpy(init, t->dstep, sizeof init);
    memset(st, 0, sizeof *st);
    for (p = 0; p < n; p++) {
        uint32_t on = 0, off = 0;
        memcpy(before, t->dstep, sizeof before);
        mutate(t);
        c = changed((const step_t *)before, t->step);
        st->most = c > st->most ? c : st->most;
        if (c > MUT_STEPS)
            return "a pass changed more than MUT_STEPS steps";
        st->zero += !c;
        for (i = 0; i < len; i++)
            for (l = 0; l < DRUM_LANES; l++) {
                int a = dstep_has(&before[i], l), b = dstep_has(&t->dstep[i], l);
                on += !a && b;
                off += a && !b;
                if (!a && b && dstep_lvl(&t->dstep[i], l) == LV_GHOST && i % 4u)
                    st->ghosts++;
                if (a && b && dstep_lvl(&before[i], l) != dstep_lvl(&t->dstep[i], l))
                    st->levels++;
                if (a && b && dstep_rat(&before[i], l) != dstep_rat(&t->dstep[i], l))
                    st->rats++;
            }
        st->moved += on && off && on == off;
        st->gone += off && !on;
        if ((why = drum_bad(t, init)) != 0)
            return why;
        if (p % 5u == 4u) {
            if (!mutate_back(t) || memcmp(before, t->dstep, sizeof before))
                return "turned back: not the pattern before the pass";
            mutate(t);
            if ((why = drum_bad(t, init)) != 0)
                return why;
        }
    }
    return 0;
}

static void groove(track_t *t, uint32_t len)
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
    if (len > 14u)
        dstep_set(&t->dstep[14], LANE_OPEN, LV_NORM, 0);
    if (len > 12u)
        dstep_set(&t->dstep[12], LANE_CLAP, LV_HARD, 0);
    dstep_set(&t->dstep[40], LANE_BELL, LV_NORM, 0);        /* past LEN: never touched */
}

int main(void)
{
    static const uint8_t D3[1] = {50}, F3[1] = {53}, DFA[3] = {62, 65, 69}, A3[1] = {57}, C4[1] = {60}, E3[1] = {52}, G3[1] = {55};
    static const uint8_t X[1] = {99};
    static step_t snap[21][NSTEP], keep[NSTEP];
    track_t *t;
    stats_t st;
    dstats_t ds;
    const char *why;
    uint32_t i, ok, h1, h2;

    memset(trk, 0, sizeof trk);
    memset(&song, 0, sizeof song);
    host_tracks_init();
    rng_state = 0x5EED1u;

    /* ---- a synth part in D minor: a tie, a chord with levels / ratchets, a slide, a ratchet */
    t = synth_setup(0, 2, 2, V_POLY, 16);
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
    note(t, 20, 1, X, ST_NOTE, 0, 0);                            /* past LEN: never touched */
    memcpy(keep, t->step, sizeof keep);
    why = synth_run(t, 5000, &st);
    check(why ? why : "D minor, 5000 passes: notes in the scale, <= 4, LEN, bits, ties, span", !why);
    check("each kind of change happens: pitch, level, ratchet, a note added, one taken",
          st.moves > 1000 && st.levels > 200 && st.rats > 100 && st.adds > 20 && st.removes > 20);
    check("a pitch moves one degree of the scale", st.moves && !st.wrong_move);
    check("a pass changes a little (1..4 steps, never none: a pass changes a step once)",
          st.most <= MUT_STEPS && st.most >= 2u && !st.zero);
    check("every 5th pass turned back at once: exactly the pattern before it", st.backs == 1000);

    /* ---- turning back: up to 16 passes, the last first; then nothing more */
    memcpy(t->step, keep, sizeof keep);
    for (i = 0; i < 20u; i++) {
        memcpy(snap[i], t->step, sizeof snap[i]);
        mutate(t);
    }
    memcpy(snap[20], t->step, sizeof snap[20]);
    ok = mutate_depth(t) == MUT_DEPTH;
    for (i = 0; i < MUT_DEPTH; i++)
        ok &= mutate_back(t) == 1 && !memcmp(t->step, snap[19u - i], sizeof snap[0]) && mutate_depth(t) == MUT_DEPTH - 1u - i;
    check("20 passes, 16 turned back: each pattern again, in order", ok);
    check("the 17th: nothing (the oldest 4 are not kept), the pattern stays",
          !mutate_back(t) && !memcmp(t->step, snap[4], sizeof snap[0]));
    mutate(t);
    mutate(t);
    memcpy(snap[0], t->step, sizeof snap[0]);
    t->step[2].time = ST_NOTE, t->step[2].n = 1, t->step[2].note[0] = 62;   /* another edit */
    memcpy(snap[1], t->step, sizeof snap[1]);
    check("the pattern changed since (an edit, undo, recording): nothing to turn back",
          !mutate_depth(t) && !mutate_back(t) && !memcmp(t->step, snap[1], sizeof snap[1]));
    mutate(t);
    check("another track: its own passes, the first track's gone", mutate_depth(t) == 1u);
    {
        track_t *u = synth_setup(1, 2, 2, V_POLY, 16);
        note(u, 0, 1, D3, ST_NOTE, 0, 0);
        note(u, 4, 1, F3, ST_NOTE, 0, 0);
        note(u, 8, 1, A3, ST_NOTE, 0, 0);
        ok = mutate(u) == 1u && !mutate_depth(t) && !mutate_back(t);
        check("... a pass on track 2: none to turn back on track 1", ok);
    }

    /* ---- the same seed: the same passes */
    memcpy(t->step, keep, sizeof keep);
    rng_state = 77u;
    for (i = 0; i < 300u; i++)
        mutate(t);
    h1 = pattern_sum(t);
    memcpy(t->step, keep, sizeof keep);
    rng_state = 77u;
    for (i = 0; i < 300u; i++)
        mutate(t);
    h2 = pattern_sum(t);
    memcpy(t->step, keep, sizeof keep);
    rng_state = 78u;
    for (i = 0; i < 300u; i++)
        mutate(t);
    check("the same seed: the same 300 passes (another seed: others)", h1 == h2 && h1 != pattern_sum(t));

    /* ---- chromatic (CHR): semitones; a mono part: one note a step; one note over and over: an octave */
    t = synth_setup(0, 0, 0, V_POLY, 12);
    note(t, 0, 1, C4, ST_NOTE, 0, 0);
    note(t, 3, 2, DFA, ST_NOTE, 0, 0);
    note(t, 6, 1, A3, ST_NOTE, 0, 0);
    note(t, 7, 0, A3, ST_TIE, 0, 0);
    note(t, 9, 1, E3, ST_NOTE, 0, 0);
    why = synth_run(t, 3000, &st);
    check(why ? why : "CHR, 12 steps, 3000 passes: invariants", !why);
    check("CHR: a pitch moves a semitone", st.moves > 500 && !st.wrong_move);
    t = synth_setup(2, 9, 6, V_LEGATO, 16);                      /* A minor pentatonic, LEGATO */
    for (i = 0; i < 16u; i += 2u)
        note(t, i, 1, (i & 4u) ? C4 : A3, ST_NOTE, 0, 0);
    note(t, 5, 0, C4, ST_TIE, 0, 0);
    why = synth_run(t, 3000, &st);
    check(why ? why : "A minor pentatonic, LEGATO: one note a step, invariants", !why);
    t = synth_setup(2, 0, 1, V_POLY, 16);                        /* C major, every note C4 */
    for (i = 0; i < 16u; i += 4u)
        note(t, i, 1, C4, ST_NOTE, 0, 0);
    why = synth_run(t, 4000, &st);
    check(why ? why : "C major, one note: the notes stay within an octave", !why);
    t = synth_setup(2, 0, 1, V_POLY, 16);
    memcpy(keep, t->step, sizeof keep);
    check("an empty part: nothing to mutate, unchanged", !mutate(t) && !memcmp(keep, t->step, sizeof keep) && !mutate_depth(t));

    /* ---- the drum track: ghosts, moves, levels, hat ratchets; the kicks on the beats stay */
    t = TDRUM;
    rng_state = 0xD8u;
    groove(t, 16);
    why = drum_run(t, 5000, &ds);
    check(why ? why : "drums, 16 steps, 5000 passes: kicks on the beats, LEN, bits, sounds", !why);
    check("ghost hits added and taken, hits moved, levels, ratchets",
          ds.ghosts > 200 && ds.gone > 200 && ds.moved > 200 && ds.levels > 200 && ds.rats > 100);
    check("a pass changes a little (1..4 steps, never none)", ds.most <= MUT_STEPS && ds.most >= 2u && !ds.zero);
    groove(t, 12);
    why = drum_run(t, 3000, &ds);
    check(why ? why : "drums, 12 steps, 3000 passes: invariants", !why);
    groove(t, 16);
    memcpy(keep, t->step, sizeof keep);
    for (i = 0; i < 12u; i++)
        mutate(t);
    for (i = 0; i < 12u; i++)
        mutate_back(t);
    check("drums: 12 passes, 12 back: the groove exactly", !memcmp(keep, t->step, sizeof keep) && !mutate_depth(t));
    steps_clear(t);
    for (i = 0; i < 16u; i += 4u)
        dstep_set(&t->dstep[i], LANE_KICK, LV_NORM, 0);
    memcpy(keep, t->step, sizeof keep);
    check("drums, only kicks on the beats: nothing to mutate", !mutate(t) && !memcmp(keep, t->step, sizeof keep));

    printf(bad ? "mutate: %d FAILED\n" : "mutate: all ok\n", bad);
    return bad != 0;
}
