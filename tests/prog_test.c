/* SPDX-License-Identifier: GPL-3.0-only */
/* PROG (GLO > JAM, seq.c prog_*): the key follows a chord progression, one chord a bar, on the real
 * sequencer (events_block at the clock) and the real UI (the harness of ui_pages_test.c):
 *   bars      every progression over 36 bars: each bar's step plays the chord of (bar mod its length),
 *             on three tracks at once in their own scales and roots (MAJ / MIN, PEN on D, CHR)
 *   exact     the shift of each bar and each note moved, against a reference built from the scale's notes,
 *             for 9 scales x 4 roots x every progression; spot values (C MAJ I V vi IV, CHR, PEN)
 *   diatonic  a triad of the scale moved stays a triad of the scale (the same steps apart)
 *   arp       its notes follow the bar they start in; what it holds (arp_ch) stays as played
 *   drums     the drum track plays exactly the same hits with PROG on as off
 *   off       OFF / stopped: every note as played
 *   sections  a song section (seq_reset_tracks) and PLAY start the progression over
 *   held      a key held over a bar line keeps its pitch on one voice (no retrigger) and its release ends
 *             it; a TIE over a bar line keeps the pitch; a slide into the next bar moves; MIDI in follows;
 *             a long run of BLUES with chords, ratchets, ties, slides, an arp at full gate, MONO and LEGATO
 *             parts and random keys leaves no voice gated once the keys are up and the transport stops
 *   pattern   never written (the steps as recorded); a live note is recorded as played
 *   saved     PROG is a global of the project (tests/project_test.c checks the format)
 *   page      GLO > JAM after DRUMS: KNOB 1 sets PROG, the page shows the progression and the bar playing
 * Exit status: the number of failed checks. */
#define UI_TEST_MAIN ui_main
#include "ui_pages_test.c"

static int bad;
static void ck(int ok, const char *what)
{
    printf("prog: %-86s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

/* ---- the reference: the progressions as roman numerals (1 = I), minor ones flagged */
static const uint8_t REF[9][12] = {{1, 5, 6, 4}, {6, 4, 1, 5}, {1, 6, 4, 5}, {2, 5, 1, 1}, {1, 4, 5, 4}, {1, 6, 3, 7},
                                   {1, 4, 5, 1}, {1, 7, 6, 5}, {1, 1, 1, 1, 4, 4, 1, 1, 5, 4, 1, 5}};
static const uint8_t REFN[9] = {4, 4, 4, 4, 4, 4, 4, 4, 12};
static const uint8_t REFMIN[9] = {0, 0, 0, 0, 0, 1, 1, 1, 0};
static const uint16_t MASKS[] = {0xFFF, 0xAB5, 0x5AD};   /* (CHR, MAJ, MIN as SCALE_MASK: checked below) */

static int ref_in(uint32_t mask, int32_t root, int32_t n) { return (mask >> (uint32_t)((n - root + 120) % 12)) & 1u; }
static uint32_t popc(uint32_t m) { uint32_t n = 0; while (m) { n += m & 1u; m >>= 1; } return n; }
/* the shift of bar b (progression p 0-based): the degree's steps of the scale (CHR: semitones), the
 * nearer way (a tie: up) */
static int32_t ref_shift(uint32_t p, uint32_t b, uint32_t mask)
{
    static const int8_t MAJ[7] = {0, 2, 4, 5, 7, 9, 11}, MIN[7] = {0, 2, 3, 5, 7, 8, 10};
    int32_t deg = REF[p][b % REFN[p]] - 1, cnt = mask == 0xFFFu ? 12 : (int32_t)popc(mask);
    int32_t up = mask == 0xFFFu ? (REFMIN[p] ? MIN : MAJ)[deg] : deg % cnt, down = up - cnt;
    return -down < up ? down : up;
}
/* note n (in the scale) d notes of the scale up / down: by the list of the scale's notes */
static int32_t ref_move(uint32_t mask, int32_t root, int32_t n, int32_t d)
{
    int32_t list[128], k = 0, i, at = -1;
    for (i = 0; i < 128; i++)
        if (ref_in(mask, root, i)) {
            if (i == n)
                at = k;
            list[k++] = i;
        }
    if (at < 0 || at + d < 0 || at + d >= k)
        return -1;
    return list[at + d];
}

/* ---- the sequencer at the clock, without the audio: released voices end at once */
static void blk(void)
{
    uint32_t k, i;
    events_block(CTL);
    for (k = 0; k < NPART; k++)
        for (i = 0; i < NVOICE; i++)
            if (!trk[k].v[i].gate) {
                trk[k].v[i].active = 0;
                trk[k].v[i].stage = 0;
            }
}
/* the notes gated on t, sorted */
static uint32_t sounding(const track_t *t, uint8_t *nt)
{
    uint32_t i, j, n = 0;
    for (i = 0; i < NVOICE; i++)
        if (t->v[i].gate) {
            for (j = n++; j > 0 && nt[j - 1] > t->v[i].note; j--)
                nt[j] = nt[j - 1];
            nt[j] = t->v[i].note;
        }
    return n;
}
static uint32_t gated_all(void)
{
    uint32_t k, i, n = 0;
    for (k = 0; k < NPART; k++)
        for (i = 0; i < NVOICE; i++)
            n += trk[k].v[i].gate != 0;
    return n;
}
static int sounds(const track_t *t, const int32_t *want, uint32_t n)   /* exactly these (any order) */
{
    uint8_t nt[NVOICE];
    int32_t w[8];
    uint32_t i, j;
    for (i = 0; i < n; i++) {                       /* (sorted) */
        for (j = i; j > 0 && w[j - 1] > want[i]; j--)
            w[j] = w[j - 1];
        w[j] = want[i];
    }
    if (sounding(t, nt) != n)
        return 0;
    for (i = 0; i < n; i++)
        if (nt[i] != w[i])
            return 0;
    return 1;
}

static void reset(void)
{
    uint32_t i;
    memset(trk, 0, sizeof trk);
    memset(&song, 0, sizeof song);
    memset(roll, 0, sizeof roll);
    memset(pg_src, 0, sizeof pg_src);
    clk_beat = clk_pos = 0;
    transport_req = 0;
    arrangement_enabled = 0;
    host_tracks_init();
    song.g[G_BPM] = 240;
    for (i = 0; i < NPART; i++) {
        host_preset(&trk[i], 0, 0);
        trk[i].p[P_VOICE] = V_POLY;
        trk[i].p[P_AMODE] = 0;
        trk[i].p[P_GLIDE] = 0;
        trk[i].p[P_SCALE] = 1;
        trk[i].p[P_ROOT] = 0;
        trk[i].p[P_SDIV] = 2;
        trk[i].p[P_SLEN] = 16;
        trk[i].p[P_SGATE] = 64;
    }
    TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;
}
static void steps(track_t *t, uint32_t i, uint32_t n, const uint8_t *notes, uint32_t time, uint32_t flags)
{
    put_step(t, i, n, notes, time, flags);
    t->seq_active = 1;
}
static void play_to(uint32_t beat)
{
    while (clk_beat < beat)
        blk();
}

int main(int argc, char **argv)
{
    uint32_t p, b, i, k;
    outdir = argc > 1 ? argv[1] : "build/host";
    ck(SCALE_MASK[0] == MASKS[0] && SCALE_MASK[1] == MASKS[1] && SCALE_MASK[2] == MASKS[2] && NPROG == 9u &&
       GP[G_PROG].max == 9 && GP[G_PROG].def == 0 && str_eq(GP[G_PROG].label, "PROG"),
       "PROG: OFF + 9 progressions, OFF by default");
    for (p = 0, k = 1; p < NPROG; p++) {
        k &= str_len(PROG_DEG[p]) == REFN[p] && str_len(N_PROG[p + 1u]) <= 5u && ((PROG_MINOR >> p) & 1u) == REFMIN[p];
        for (b = 0; b < REFN[p]; b++)
            k &= PROG_DEG[p][b] - '0' == REF[p][b];
    }
    ck(k, "the progressions: I V vi IV, vi IV I V, I vi IV V, ii V I I, I IV V IV, i VI III VII, i iv v i, ANDAL, BLUES");

    /* ---- bars: every progression, 36 bars, three tracks */
    {
        static const uint8_t CEG[3] = {60, 64, 67}, CEbG[3] = {60, 63, 67}, DPEN[3] = {62, 66, 69}, CHRN[2] = {60, 63};
        int ok_all = 1, ok_1564 = 1, ok_blues = 1;
        uint32_t checked = 0;
        for (p = 0; p < NPROG; p++) {
            uint32_t last = SEQ_NONE;
            reset();
            song.g[G_PROG] = (int16_t)(p + 1u);
            trk[0].p[P_SCALE] = REFMIN[p] ? 2 : 1;
            trk[1].p[P_SCALE] = 5;                  /* PEN on D */
            trk[1].p[P_ROOT] = 2;
            trk[2].p[P_SCALE] = 0;                  /* CHR */
            steps(&trk[0], 0, 3, REFMIN[p] ? CEbG : CEG, ST_NOTE, 0);
            steps(&trk[1], 0, 3, DPEN, ST_NOTE, 0);
            steps(&trk[2], 0, 2, CHRN, ST_NOTE, 0);
            for (i = 0; i < 16u; i += 2u)
                dstep_set(&TDRUM->dstep[i], i % 4u ? 2u : 0u, LV_NORM, 0);
            TDRUM->seq_active = 1;
            seq_start();
            while (clk_beat < 4u * 36u) {
                blk();
                if (trk[0].seq_abs != last && trk[0].seq_idx == 0u) {
                    int32_t w[3];
                    last = trk[0].seq_abs;
                    b = trk[0].seq_abs / 16u;
                    for (k = 0; k < 3u; k++)
                        w[k] = ref_move(scale_mask(&trk[0]), 0, trk[0].step[0].note[k], ref_shift(p, b, scale_mask(&trk[0])));
                    ok_all &= sounds(&trk[0], w, 3);
                    for (k = 0; k < 3u; k++)
                        w[k] = ref_move(scale_mask(&trk[1]), 2, DPEN[k], ref_shift(p, b, scale_mask(&trk[1])));
                    ok_all &= sounds(&trk[1], w, 3);
                    for (k = 0; k < 2u; k++)
                        w[k] = CHRN[k] + ref_shift(p, b, 0xFFF);
                    ok_all &= sounds(&trk[2], w, 2);
                    if (p == 0u) {                  /* C major: C E G, G B D below, A C E below, F A C above */
                        static const int32_t W[4][3] = {{60, 64, 67}, {55, 59, 62}, {57, 60, 64}, {65, 69, 72}};
                        ok_1564 &= sounds(&trk[0], W[b % 4u], 3);
                    }
                    if (p == 8u) {                  /* BLUES: I I I I IV IV I I V IV I V */
                        static const int8_t D[12] = {0, 0, 0, 0, 5, 5, 0, 0, -5, 5, 0, -5};
                        int32_t c[2] = {60 + D[b % 12u], 63 + D[b % 12u]};
                        ok_blues &= sounds(&trk[2], c, 2);
                    }
                    checked++;
                }
            }
        }
        ck(ok_all && checked == 9u * 36u, "every progression, 36 bars: each bar the chord of bar mod length (MAJ / MIN, D PEN, CHR)");
        ck(ok_1564, "1564 on C major: C E G | G B D | A C E | F A C (the nearer way)");
        ck(ok_blues, "BLUES on CHR: I I I I IV IV I I V IV I V (+5 / -5 semitones)");
    }

    /* ---- exact: shifts and notes against the reference, 9 scales x 4 roots x every progression */
    {
        static const uint8_t SCL[9] = {0, 1, 2, 3, 5, 6, 7, 12, 14};   /* CHR MAJ MIN DOR PEN MPEN HARM BLUES DIMHW */
        static const uint8_t ROOTS[4] = {0, 2, 7, 11};
        int ok_d = 1, ok_n = 1, ok_tri = 1;
        track_t *t = &trk[0];
        reset();
        song.playing = 1;
        for (i = 0; i < 9u; i++)
            for (k = 0; k < 4u; k++)
                for (p = 0; p < NPROG; p++) {
                    t->p[P_SCALE] = SCL[i];
                    t->p[P_ROOT] = ROOTS[k];
                    song.g[G_PROG] = (int16_t)(p + 1u);
                    for (b = 0; b < REFN[p]; b++) {
                        uint32_t mask = scale_mask(t);
                        int32_t d, n;
                        clk_beat = 4u * b + (b & 3u);       /* (any beat of the bar) */
                        d = prog_shift(t);
                        ok_d &= d == ref_shift(p, b, mask);
                        for (n = 24; n <= 100; n++) {
                            if (!ref_in(mask, ROOTS[k], n))
                                continue;
                            ok_n &= (int32_t)prog_note(t, (uint32_t)n, d) == ref_move(mask, ROOTS[k], n, d);
                            if (mask != 0xFFFu) {           /* the triad on n: the same steps apart, moved */
                                int32_t a = ref_move(mask, ROOTS[k], n, 2), c = ref_move(mask, ROOTS[k], n, 4);
                                int32_t a2 = (int32_t)prog_note(t, (uint32_t)a, d), c2 = (int32_t)prog_note(t, (uint32_t)c, d);
                                int32_t n2 = (int32_t)prog_note(t, (uint32_t)n, d);
                                ok_tri &= ref_move(mask, ROOTS[k], n2, 2) == a2 && ref_move(mask, ROOTS[k], n2, 4) == c2;
                            }
                        }
                    }
                }
        ck(ok_d, "the shift of every bar: the degree in the scale (CHR: semitones, minor ones minor), nearer way");
        ck(ok_n, "every note of 9 scales x 4 roots moved exactly as the reference");
        ck(ok_tri, "chords stay diatonic: a triad of the scale moved is a triad of the scale");
        t->p[P_ROOT] = 0;
        t->p[P_SCALE] = 0;
        song.g[G_PROG] = 1;
        clk_beat = 4;
        k = prog_note(t, 60, prog_shift(t)) == 55u;          /* CHR, 1564 bar 2: V = 7 semitones: down 5 */
        song.g[G_PROG] = 6;
        k &= prog_note(t, 60, prog_shift(t)) == 56u;         /* CHR, 1637 (minor) bar 2: VI = Ab: down 4 */
        t->p[P_SCALE] = 5;
        song.g[G_PROG] = 1;
        k &= prog_note(t, 60, prog_shift(t)) == 57u && prog_note(t, 64, prog_shift(t)) == 62u &&
             prog_note(t, 67, prog_shift(t)) == 64u;          /* PEN: 4 steps of 5: one step down */
        t->p[P_SCALE] = 1;                                  /* C major, V (down 3): D -> A, folded up an octave */
        k &= prog_note(t, 2, prog_shift(t)) == 9u && prog_note(t, 124, prog_shift(t)) == 119u;
        clk_beat = 12;                                      /* IV (up 3): G9 -> C10, folded down an octave */
        k &= prog_note(t, 127, prog_shift(t)) == 120u && prog_note(t, 2, prog_shift(t)) == 7u;
        ck(k, "spot values: CHR V -5, CHR minor VI -4, PEN V one step down, folded into 0..127");
        song.playing = 0;
        ck(prog_shift(t) == 0, "stopped: no shift");
        song.playing = 1;
        song.g[G_PROG] = 0;
        ck(prog_shift(t) == 0, "OFF: no shift");
        song.g[G_PROG] = 1;
        ck(prog_shift(TDRUM) == 0, "the drum track: no shift");
    }

    /* ---- arp: its notes follow the bar they start in */
    {
        track_t *t = &trk[0];
        int ok = 1, raw = 1;
        uint32_t fired = 0;
        reset();
        song.g[G_PROG] = 3;                         /* 1645 */
        t->p[P_SCALE] = 3;                          /* D dorian */
        t->p[P_ROOT] = 2;
        t->p[P_AMODE] = 1;                          /* UP */
        t->p[P_ARATE] = 2;
        t->p[P_AGATE] = 64;
        t->p[P_AOCT] = 1;
        arp_add(t, 62);
        arp_add(t, 65);
        arp_add(t, 69);
        seq_start();
        while (clk_beat < 4u * 20u) {
            uint32_t bar = clk_beat / 4u, idx = t->arp_idx;
            blk();
            if (t->arp_idx != idx && t->arp_snd) {
                int32_t w = ref_move(scale_mask(t), 2, t->arp_ch[0], ref_shift(2, bar, scale_mask(t)));
                ok &= sounds(t, &w, 1);
                raw &= t->arp_ch[0] == 62u || t->arp_ch[0] == 65u || t->arp_ch[0] == 69u;
                fired++;
            }
        }
        ck(ok && fired >= 300u, "arp: each note moved by the chord of the bar it starts in (D dorian, 1645)");
        ck(raw && t->nheld == 3u && t->held[0] == 62u, "arp: what it holds and its order as played");
        while (t->nheld)
            arp_remove(t, t->held[0]);
        seq_stop();
        blk();
        ck(!gated_all(), "arp: nothing left sounding");
    }

    /* ---- drums: the same hits with PROG on as off */
    {
        static uint16_t h[2][4096];
        uint32_t run, n = 0;
        for (run = 0; run < 2u; run++) {
            reset();
            song.g[G_PROG] = run ? 9 : 0;
            for (i = 0; i < 16u; i++)
                dstep_set(&TDRUM->dstep[i], i % 16u, i & 1u ? LV_SOFT : LV_HARD, i % 3u == 0u);
            TDRUM->seq_active = 1;
            seq_start();
            drums.hits = 0;
            for (n = 0; n < 4096u; n++) {
                blk();
                h[run][n] = drums.hits;
                drums.hits = 0;
            }
            seq_stop();
        }
        ck(!memcmp(h[0], h[1], sizeof h[0]), "drums: every hit the same with PROG BLUES as OFF");
    }

    /* ---- OFF: every step as written; sections and PLAY start the progression over */
    {
        static const uint8_t CEG[3] = {60, 64, 67};
        static const int32_t W[3] = {60, 64, 67}, V[3] = {55, 59, 62};
        uint32_t last = SEQ_NONE, n = 0;
        int ok = 1;
        reset();
        steps(&trk[0], 0, 3, CEG, ST_NOTE, 0);
        seq_start();
        while (clk_beat < 4u * 12u) {
            blk();
            if (trk[0].seq_abs != last && trk[0].seq_idx == 0u) {
                last = trk[0].seq_abs;
                ok &= sounds(&trk[0], W, 3);
                n++;
            }
        }
        ck(ok && n == 12u, "OFF: every bar as written");
        song.g[G_PROG] = 1;
        play_to(4u * 13u);                          /* bar 13: V (13 mod 4 = 1) */
        blk();
        ok = sounds(&trk[0], V, 3);
        seq_reset_tracks(0);                        /* a song section starts on this bar */
        blk();
        ok &= sounds(&trk[0], W, 3);
        play_to(4u);
        blk();
        ok &= sounds(&trk[0], V, 3);
        ck(ok, "a song section starts the progression over (I, then V)");
        play_to(4u * 2u + 2u);
        seq_stop();
        blk();
        seq_start();
        blk();
        ck(sounds(&trk[0], W, 3), "STOP, PLAY: from I again");
        seq_stop();
    }

    /* ---- held over a bar line */
    {
        track_t *t = &trk[0];
        static const uint8_t C[1] = {60}, E[1] = {64}, CE[2] = {60, 64};
        int32_t w;
        uint32_t vi = 0, age = 0, ok;
        reset();
        song.g[G_PROG] = 1;
        seq_start();
        play_to(2);
        input_on(t, 64, 100);                       /* bar 1 (I): E */
        w = 64;
        ok = sounds(t, &w, 1);
        for (i = 0; i < NVOICE; i++)
            if (t->v[i].gate)
                vi = i, age = t->v[i].age;
        play_to(4u + 2u);                           /* bar 2 (V) */
        ok &= sounds(t, &w, 1) && t->v[vi].gate && t->v[vi].note == 64u && t->v[vi].age == age;
        input_on(t, 60, 100);                       /* bar 2: C -> G below */
        w = 55;
        ok &= t->v[vi].note == 64u;
        {
            int32_t two[2] = {55, 64};
            ok &= sounds(t, two, 2);
        }
        input_off(t, 64);
        ok &= sounds(t, &w, 1);
        play_to(8u + 1u);                           /* bar 3 (vi): the G goes on */
        ok &= sounds(t, &w, 1);
        input_off(t, 60);
        ok &= !gated_all();
        ck(ok, "a key over a bar line: its pitch on one voice (no retrigger), its release ends it");
        input_on(t, 64, 100);                       /* bar 3 (vi): E -> C */
        w = 60;
        ok = sounds(t, &w, 1);
        seq_stop();
        blk();
        ok &= sounds(t, &w, 1);                     /* (a key down over STOP: as it started) */
        input_off(t, 64);
        ok &= !gated_all();
        ck(ok, "a key down over STOP keeps its pitch to its release");

        /* MIDI in: channel 1 -> track 1 */
        reset();
        song.g[G_PROG] = 1;
        seq_start();
        play_to(4u + 1u);                           /* bar 2 (V) */
        midi_in_q[mi_w++ % MQ] = 0x09u | 0x90u << 8 | 67u << 16 | 90u << 24;
        blk();
        w = 62;                                     /* G -> D */
        ok = sounds(t, &w, 1);
        play_to(8u + 1u);
        midi_in_q[mi_w++ % MQ] = 0x08u | 0x80u << 8 | 67u << 16;
        blk();
        ck(ok && !gated_all(), "MIDI in follows, its note-off ends the note it started");

        /* a TIE over the bar line keeps the pitch; a slide into the next bar moves */
        reset();
        song.g[G_PROG] = 1;
        t->p[P_SLEN] = 32;
        steps(t, 14, 1, C, ST_NOTE, 0);
        steps(t, 15, 0, C, ST_TIE, 0);
        steps(t, 16, 0, C, ST_TIE, 0);
        steps(t, 17, 0, C, ST_TIE, 0);
        steps(t, 28, 1, C, ST_NOTE, SF_SLIDE);      /* bar 2 (V): C -> G, slides into */
        steps(t, 29, 1, C, ST_NOTE, SF_SLIDE);
        steps(t, 30, 1, C, ST_NOTE, SF_SLIDE);
        steps(t, 31, 1, C, ST_NOTE, SF_SLIDE);
        steps(t, 0, 2, CE, ST_NOTE, 0);             /* bar 3 (vi): C E -> A C */
        seq_start();
        {
            int32_t c[1] = {60}, g[1] = {55}, ac[2] = {57, 60}, a[1] = {57};
            ok = 1;
            play_to(3u);
            blk();
            while (trk_grid(t, &i, &k) % 32u != 17u)
                blk();
            ok &= sounds(t, c, 1);                  /* (the tie in bar 2: still C) */
            while (trk_grid(t, &i, &k) % 32u != 19u)
                blk();
            ok &= !gated_all();
            while (trk_grid(t, &i, &k) % 32u != 31u)
                blk();
            ok &= sounds(t, g, 1);
            while (clk_beat < 8u)
                blk();
            blk();
            ok &= sounds(t, ac, 2);                 /* (slid into: G let go) */
            /* the second pass: bar 3 (vi) and 4 (IV): the tie from vi */
            while (clk_beat < 8u + 3u || trk_grid(t, &i, &k) % 32u != 17u)
                blk();
            ok &= sounds(t, a, 1);
        }
        ck(ok, "a TIE over a bar line keeps its pitch; a slide into the next bar moves");
        seq_stop();
        blk();
        ck(!gated_all(), "STOP: nothing left sounding");
    }

    /* ---- a long run of BLUES: chords, ratchets, ties, slides, an arp at full gate, MONO / LEGATO, keys */
    {
        static const uint8_t TRI[3] = {60, 64, 67}, A[2] = {57, 64}, LO[1] = {48}, HI[1] = {72};
        static step_t before[NTRK][NSTEP];
        uint32_t seed = 12345u, held[NPART] = {0}, n;
        int ok;
        reset();
        song.g[G_PROG] = 9;
        steps(&trk[0], 0, 3, TRI, ST_NOTE, SF_ACCENT);
        trk[0].step[0].rat = 0x15;                  /* x2 each */
        steps(&trk[0], 3, 2, A, ST_NOTE, 0);
        steps(&trk[0], 4, 0, A, ST_TIE, 0);
        steps(&trk[0], 5, 0, A, ST_TIE, 0);
        steps(&trk[0], 15, 3, TRI, ST_NOTE, SF_SLIDE);
        trk[0].p[P_SGATE] = 127;
        trk[1].p[P_VOICE] = V_MONO;
        trk[1].p[P_SCALE] = 2;
        steps(&trk[1], 0, 1, LO, ST_NOTE, SF_SLIDE);
        steps(&trk[1], 1, 0, LO, ST_TIE, 0);
        steps(&trk[1], 7, 1, HI, ST_NOTE, 0);
        trk[1].step[7].rat = 0x03;
        steps(&trk[1], 15, 1, LO, ST_NOTE, SF_SLIDE);
        trk[2].p[P_VOICE] = V_LEGATO;
        trk[2].p[P_SCALE] = 0;
        trk[2].p[P_AMODE] = 7;                      /* CONV */
        trk[2].p[P_AGATE] = 127;
        trk[2].p[P_ARAT] = 5;                       /* UP3: a run */
        for (i = 0; i < NTRK; i++)
            memcpy(before[i], trk[i].step, sizeof before[i]);
        seq_start();
        for (n = 0; clk_beat < 4u * 48u; n++) {
            blk();
            if (n % 97u == 0u) {                    /* keys down and up on every part, random times */
                uint32_t pt = (seed = seed * 1664525u + 1013904223u) >> 30, note = 48u + ((seed >> 8) % 36u);
                track_t *t = &trk[pt % NPART];
                if (held[pt % NPART]) {
                    input_off(t, held[pt % NPART]);
                    held[pt % NPART] = 0;
                } else {
                    input_on(t, note, 100);
                    held[pt % NPART] = note;
                }
            }
            if (n % 1500u == 0u)
                trk[1].p[P_SCALE] = (int16_t)((trk[1].p[P_SCALE] + 1) % 16);   /* the key changed while notes sound */
        }
        for (i = 0; i < NPART; i++)
            if (held[i])
                input_off(&trk[i], held[i]);
        for (i = 0; i < 4u; i++)
            blk();
        ok = 1;
        for (i = 0; i < NVOICE; i++)                /* the arp's notes (they are held: its gate) */
            ok &= !trk[2].v[i].gate || trk[2].arp_snd;
        while (trk[2].nheld)
            arp_remove(&trk[2], trk[2].held[0]);
        seq_stop();
        blk();
        blk();
        ck(ok && !gated_all(), "BLUES, 48 bars of everything: no voice left gated once keys are up and STOP");
        ok = 1;
        for (i = 0; i < NTRK; i++)
            ok &= !memcmp(before[i], trk[i].step, sizeof before[i]);
        ck(ok, "the patterns as written (PROG plays them, never writes them)");
    }

    /* ---- recording: what is played goes in as played */
    {
        track_t *t = &trk[0];
        int32_t w = 59;
        int ok, found = 0;
        reset();
        song.g[G_PROG] = 1;
        seq_start();
        song.rec = 1;
        play_to(4u + 1u);                           /* bar 2 (V): E -> B */
        input_on(t, 64, 100);
        ok = sounds(t, &w, 1);
        blk();
        input_off(t, 64);
        for (i = 0; i < NSTEP; i++)
            for (k = 0; k < t->step[i].n && t->step[i].time == ST_NOTE; k++) {
                found += t->step[i].note[k] == 64u;
                ok &= t->step[i].note[k] == 64u;
            }
        ck(ok && found == 1, "a live note recorded as played (E, heard as B in bar V)");
        song.rec = 0;
        seq_stop();
    }

    /* ---- the JAM page */
    {
        uint32_t h0 = 0, h1 = 0, h2 = 0, jam = 0xFF;
        int ok;
        panel = PANEL_DEFAULT;
        layers_init();
        settings.palette = 4;
        palette_set(4);
        reset();
        for (i = 0; i + 1u < NPAGES; i++)
            if (str_eq(PAGES[i].title, "DRUMS") && PAGES[i].fam == FAM_GLO)
                jam = i + 1u;
        ok = jam < NPAGES && str_eq(PAGES[jam].title, "JAM") && PAGES[jam].fam == FAM_GLO &&
             PAGES[jam].scope == SC_GLOBAL && PAGES[jam].id[0] == G_PROG && PAGES[jam].id[1] == 0xFF;
        ck(ok, "GLO > JAM, after DRUMS: PROG on KNOB 1");
        go_home();
        ui.force = 1;
        frame();
        for (i = 0; i < 8u && ui.page != jam; i++)
            open_family(FAM_GLO);
        ui.force = 1;
        frame();
        for (i = Y_GRAPH * 240u; i < (Y_GRAPH + H_GRAPH) * 240u; i++)
            h0 = (h0 ^ screen[i]) * 16777619u;
        encs[panel.enc[EN_K1]] = 1;
        frame();
        ok = ui.page == jam && song.g[G_PROG] >= 1;
        encs[panel.enc[EN_K1]] = 40;
        frame();
        ok &= song.g[G_PROG] == 9;
        ui.force = 1;
        frame();
        ppm("page-jam");
        for (i = Y_GRAPH * 240u; i < (Y_GRAPH + H_GRAPH) * 240u; i++)
            h1 = (h1 ^ screen[i]) * 16777619u;
        transport_req = 1;
        while (clk_beat < 4u * 5u)
            frame();
        ppm("page-jam-playing");
        for (i = Y_GRAPH * 240u; i < (Y_GRAPH + H_GRAPH) * 240u; i++)
            h2 = (h2 ^ screen[i]) * 16777619u;
        ck(ok && h0 != h1 && h1 != h2, "KNOB 1 sets PROG (OFF .. BLUES); the page shows it and the bar playing");
        transport_req = 2;
        frame();
    }

    printf("%s\n", bad ? "PROG TEST FAILED" : "prog test passed");
    return bad;
}
