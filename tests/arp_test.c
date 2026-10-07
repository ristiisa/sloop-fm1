/* SPDX-License-Identifier: GPL-3.0-only */
/* The arp modes (the order of the notes on C E G B), the rhythm (ACC, HITS of STEPS, RAT) and what
 * it records, on the real seq.c arp. Build with the same generated headers and flags as hostsim.c. */
#define main hostsim_main
#include "hostsim.c"
#undef main

static int bad;
static void check(const char *what, int ok)
{
    printf("arp: %-70s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

static track_t *setup(uint32_t mode, const uint8_t *notes, uint32_t n, uint32_t oct)
{
    track_t *t = &trk[0];
    uint32_t i;
    memset(trk, 0, sizeof trk);
    memset(&song, 0, sizeof song);
    host_tracks_init();
    t->p[P_AMODE] = (int16_t)mode;
    t->p[P_AOCT] = (int16_t)oct;
    for (i = 0; i < n; i++)
        arp_add(t, notes[i]);
    return t;
}
static uint32_t unit(const track_t *t) { return BEAT_U / DIV_DEN[(uint32_t)t->p[P_ARATE] % 6u]; }
/* one arp step (stopped): its first note, 0 = a rest */
static uint32_t step(track_t *t)
{
    arp_tick(t, unit(t));
    return t->arp_n ? t->arp_ch[0] : 0u;
}
static int seq_is(uint32_t mode, const uint8_t *notes, uint32_t n, uint32_t oct, const uint8_t *want, uint32_t len)
{
    track_t *t = setup(mode, notes, n, oct);
    uint32_t i;
    for (i = 0; i < len; i++)
        if (step(t) != want[i])
            return 0;
    return 1;
}
static uint32_t place(const uint8_t *l, uint32_t n, uint32_t note)
{
    uint32_t i;
    for (i = 0; i < n && l[i] != note; i++)
        ;
    return i;
}

int main(void)
{
    static const uint8_t CEGB[4] = {60, 64, 67, 71}, PLAYED[4] = {67, 60, 71, 64};
    track_t *t;
    uint32_t i, k, ok;

    check("MODE names: OFF..CHRD (15)", TP[P_AMODE].max == 14 && str_eq(TP[P_AMODE].names[A_CHRD], "CHRD") &&
          str_eq(TP[P_AMODE].names[A_UPDN2], "UPDN+"));
    {
        static const uint8_t W[] = {60, 64, 67, 71, 60, 64};
        check("UP: C E G B C E", seq_is(A_UP, PLAYED, 4, 1, W, 6));
    }
    {
        static const uint8_t W[] = {71, 67, 64, 60, 71};
        check("DN: B G E C B", seq_is(A_DN, CEGB, 4, 1, W, 5));
    }
    {
        static const uint8_t W[] = {60, 64, 67, 71, 67, 64, 60, 64};
        check("UPDN: C E G B G E C E (the ends once)", seq_is(A_UPDN, CEGB, 4, 1, W, 8));
    }
    {
        static const uint8_t W[] = {67, 60, 71, 64, 67};
        check("ORD: as played (G C B E), whatever the ORD parameter", seq_is(A_ORD, PLAYED, 4, 1, W, 5));
    }
    {
        static const uint8_t W[] = {60, 64, 67, 71, 71, 67, 64, 60, 60, 64};
        check("UPDN+: C E G B B G E C C E (the ends twice)", seq_is(A_UPDN2, CEGB, 4, 1, W, 10));
    }
    {
        static const uint8_t W[] = {60, 71, 64, 67, 60};
        check("CONV: C B E G C (outside in)", seq_is(A_CONV, CEGB, 4, 1, W, 5));
    }
    {
        static const uint8_t W[] = {67, 64, 71, 60, 67};
        check("DIVG: G E B C G (inside out)", seq_is(A_DIVG, CEGB, 4, 1, W, 5));
    }
    {
        static const uint8_t W[] = {60, 64, 60, 67, 60, 71, 60, 64};
        check("THMB: C E C G C B C E (the lowest between)", seq_is(A_THMB, CEGB, 4, 1, W, 8));
    }
    {
        static const uint8_t W[] = {71, 60, 71, 64, 71, 67, 71, 60};
        check("PNKY: B C B E B G B C (the highest between)", seq_is(A_PNKY, CEGB, 4, 1, W, 8));
    }
    {
        static const uint8_t W[] = {60, 72, 64, 76, 67, 79, 71, 83, 60};
        check("OCTI: C C' E E' G G' B B' (OCT 1 plays 2)", seq_is(A_OCTI, CEGB, 4, 1, W, 9));
    }
    {
        static const uint8_t W[] = {60, 72, 84, 64, 76, 88, 67};
        check("OCTI, OCT 3: C C' C'' E E' E'' G", seq_is(A_OCTI, CEGB, 4, 3, W, 7));
    }
    {
        static const uint8_t W[] = {60, 72, 72, 60};
        check("UPDN+, one note over 2 octaves: C C' C' C", seq_is(A_UPDN2, CEGB, 1, 2, W, 4));
    }
    {
        static const uint8_t W[] = {60, 60, 60};
        check("THMB / PNKY / DRNK with one note: that note",
              seq_is(A_THMB, CEGB, 1, 1, W, 3) && seq_is(A_PNKY, CEGB, 1, 1, W, 3) && seq_is(A_DRNK, CEGB, 1, 1, W, 3));
    }

    t = setup(A_DRNK, CEGB, 4, 1);
    ok = step(t) == 60;
    for (i = 0, k = 0; i < 400; i++) {
        uint32_t a = place(CEGB, 4, k ? k : 60), n = step(t), b = place(CEGB, 4, n);
        ok &= b < 4 && (a > b ? a - b : b - a) == 1u;
        k = n;
    }
    check("DRNK: starts low, then one place up or down each step (400 steps)", ok);

    t = setup(A_SHUF, CEGB, 4, 2);
    {
        uint8_t l[8] = {60, 64, 67, 71, 72, 76, 79, 83};
        uint32_t last = 99, r;
        ok = 1;
        for (r = 0; r < 100; r++) {
            uint32_t seen = 0;
            for (i = 0; i < 8; i++) {
                uint32_t p = place(l, 8, step(t));
                ok &= p < 8 && !((seen >> p) & 1u) && !(i == 0 && p == last);
                seen |= 1u << p;
                last = p;
            }
            ok &= seen == 0xFFu;
        }
    }
    check("SHUF: every note once a round, no note twice in a row (100 rounds, OCT 2)", ok);

    t = setup(A_RND, CEGB, 4, 1);
    for (i = 0, ok = 1; i < 200; i++)
        ok &= place(CEGB, 4, step(t)) < 4;
    check("RND: always a held note", ok);

    t = setup(A_CHRD, CEGB, 4, 2);
    ok = step(t) && t->arp_n == 4 && !memcmp(t->arp_ch, CEGB, 4);
    ok &= step(t) && t->arp_n == 4 && t->arp_ch[0] == 72 && t->arp_ch[3] == 83;
    ok &= step(t) && t->arp_ch[0] == 60;
    check("CHRD: all held notes each step, an octave up each step over OCT", ok);

    /* rhythm */
    t = setup(A_UP, CEGB, 4, 1);
    t->p[P_AACC] = 2;
    for (i = 0, ok = 1; i < 9; i++) {
        step(t);
        ok &= t->arp_vel == (i % 3u ? 72u : 127u);
    }
    check("ACC 1IN3: hard on every 3rd step, soft between", ok);
    t = setup(A_UP, CEGB, 4, 1);
    t->p[P_AACC] = 4;
    for (i = 0, ok = 1; i < 16; i++) {
        step(t);
        ok &= t->arp_vel == ((0x49u >> (i & 7u)) & 1u ? 127u : 72u);
    }
    check("ACC 3-3-2: X..X..X.", ok);
    t = setup(A_UP, CEGB, 4, 1);
    step(t);
    check("ACC OFF: velocity 100 (as the keys)", t->arp_vel == 100);

    t = setup(A_UP, CEGB, 4, 1);
    t->p[P_AHITS] = 3;
    t->p[P_ASTEPS] = 8;
    {
        static const uint8_t W[] = {60, 0, 0, 64, 0, 0, 67, 0, 71, 0, 0, 60};
        for (i = 0, ok = 1; i < sizeof W; i++)
            ok &= step(t) == W[i];
    }
    check("HITS 3 of STEPS 8: X..X..X. and the rests do not move the order", ok);
    t = setup(A_UP, CEGB, 4, 1);
    t->p[P_AHITS] = 16;
    t->p[P_ASTEPS] = 5;
    for (i = 0, ok = 1; i < 10; i++)
        ok &= step(t) != 0;
    check("HITS >= STEPS: every step", ok);

    t = setup(A_UP, CEGB, 4, 1);
    t->p[P_ARAT] = 2;
    t->p[P_AGATE] = 64;
    {
        uint32_t u = unit(t), q = u / 6u, on = 0, prev = 0;
        arp_tick(t, u);
        ok = t->arp_snd && t->arp_rat == 2;
        for (i = 0; i < 5; i++) {                     /* the rest of the step in sixths */
            arp_tick(t, q);
            on += t->arp_snd && !prev;
            prev = t->arp_snd;
        }
        ok &= on == 2 && t->arp_rat == 0 && t->arp_ch[0] == 60;
        arp_tick(t, q);
        ok &= t->arp_snd && t->arp_ch[0] == 64;
    }
    check("RAT X3: three hits in the step (gate 50 %), then the next note", ok);

    /* recording: what it plays, with its level and ratchet */
    t = setup(A_CHRD, CEGB, 2, 1);
    t->p[P_AACC] = 1;
    t->p[P_ARAT] = 1;
    song.playing = song.rec = 1;
    arp_tick(t, CTL * (uint32_t)song.g[G_BPM]);
    {
        const step_t *s = &t->step[0];
        check("REC: a CHRD step recorded with its notes, level (hard) and ratchet (x2)",
              s->n == 2 && s->note[0] == 60 && s->note[1] == 64 && (s->lvl & 3u) == LV_HARD && (s->rat & 3u) == 1u);
    }

    /* off */
    t = setup(A_UP, CEGB, 4, 1);
    step(t);
    t->p[P_AMODE] = 0;
    arp_tick(t, unit(t));
    t->nheld = 0;
    arp_tick(t, unit(t));
    check("no keys: nothing sounds", !t->arp_snd);

    puts(bad ? "arp test FAILED" : "arp: all checks ok");
    return bad ? 1 : 0;
}
