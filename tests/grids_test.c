/* SPDX-License-Identifier: GPL-3.0-only */
/* GRIDS (the drum screen's MAP page, seq.c grids.c) on the real generator, and the gesture on the real UI
 * (the harness of ui_pages_test.c):
 *   map      the 25 nodes as Grids' resources.cc (a checksum of the table), ReadDrumMap at known X / Y
 *            (reference values computed from the original tables and U8Mix)
 *   density  0: no kick, snare or hat; up: the hits of a lower density stay (and more come); 127: a hit
 *            wherever the map is above 0
 *   levels   each hit's level from the map's (over 192: the accent, HARD), each step left out at or under
 *            the threshold; at 16 steps a bar the 32nd after a hit is its ratchet x2, at DIV 1/32 the map's
 *            32 steps one a step (no ratchets)
 *   chaos    0: every bar the same; up: only more hits (the map's levels pushed up), the bars and the seeds
 *            differ
 *   kept     the other lanes, the steps past LEN (their conditions and locks); a step whose kick, snare or
 *            hat changed has no condition, no lock, the others keep theirs
 *   UI       TRACKS -> SEQ: DRUMS (grid) -> kit -> map; KNOB 1..4 X, Y, DENSITY, CHAOS: the pattern follows;
 *            a touch is one undo (EDIT + OCT- / OCT+ exactly); CHAOS touched again: a new draw
 * Exit status: the number of failed checks. */
#define UI_TEST_MAIN ui_main
#include "ui_pages_test.c"

static int bad;
static void ck(int ok, const char *what)
{
    printf("grids: %-82s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

typedef struct { dstep_t st[NSTEP]; uint8_t cond[NSTEP]; plk_t lk[PLK_MAX]; } snap_t;
static void snap(const track_t *t, snap_t *s)
{
    memcpy(s->st, t->dstep, sizeof s->st);
    memcpy(s->cond, t->cond, sizeof s->cond);
    plk_save(t, s->lk);
}
static int same(const track_t *t, const snap_t *s)
{
    static snap_t now;
    snap(t, &now);
    return !memcmp(&now, s, sizeof now);
}
static void knobs(uint32_t x, uint32_t y, uint32_t d, uint32_t c, uint32_t seed)
{
    grids.k[0] = (uint8_t)x, grids.k[1] = (uint8_t)y, grids.k[2] = (uint8_t)d, grids.k[3] = (uint8_t)c;
    grids.seed = seed;
}
static const uint32_t KSH = 1u << LANE_KICK | 1u << LANE_SNARE | 1u << LANE_HAT;
static uint32_t ksh_hits(const track_t *t, uint32_t len)
{
    uint32_t i, n = 0, p;
    for (i = 0; i < len; i++)
        for (p = 0; p < 3u; p++)
            n += dstep_has(&t->dstep[i], GRIDS_LANE[p]);
    return n;
}
/* a pattern on every lane, conditions and locks within LEN and past it */
static void setup(uint32_t len, uint32_t div)
{
    uint32_t i;
    TDRUM->p[P_SLEN] = (int16_t)len;
    TDRUM->p[P_SDIV] = (int16_t)div;
    steps_clear(TDRUM);
    for (i = 0; i < NSTEP; i++) {
        if (i % 4u == 0u) dstep_set(&TDRUM->dstep[i], LANE_KICK, LV_NORM, 0);
        if (i % 3u == 1u) dstep_set(&TDRUM->dstep[i], LANE_BELL, LV_SOFT, 1);
        if (i % 5u == 2u) dstep_set(&TDRUM->dstep[i], LANE_CLAP, LV_HARD, 0);
        if (i % 7u == 3u) dstep_set(&TDRUM->dstep[i], LANE_HAT, LV_GHOST, 2);
        TDRUM->cond[i] = (uint8_t)(i % 3u ? CN_P50 : CN_ALWAYS);
    }
    for (i = 0; i < 12u; i++)
        plk_set(TDRUM, i * 5u + 1u, P_SLDEPTH, (int32_t)i * 10);
}
/* everything but the kick, snare and hat as before; past LEN all as before; within LEN a step whose
 * kick / snare / hat changed: no condition, no lock, the others theirs */
static const char *kept_bad(const track_t *t, const snap_t *was)
{
    uint32_t len = trk_len(t), i, l;
    static snap_t now;
    snap(t, &now);
    for (i = 0; i < NSTEP; i++) {
        const dstep_t *a = &was->st[i], *b = &t->dstep[i];
        int ch = 0;
        for (l = 0; l < DRUM_LANES; l++) {
            int d = dstep_has(a, l) != dstep_has(b, l) || dstep_lvl(a, l) != dstep_lvl(b, l) || dstep_rat(a, l) != dstep_rat(b, l);
            if (d && (!((KSH >> l) & 1u) || i >= len))
                return "another lane (or a step past LEN) changed";
            ch |= d;
        }
        if (ch && (t->cond[i] || plk_count(t, i)))
            return "a step rewritten kept its condition or its lock";
        if (!ch && t->cond[i] != was->cond[i])
            return "a step not rewritten lost its condition";
        if (!ch) {
            uint32_t k, n = 0;
            for (k = 0; k < PLK_MAX && was->lk[k].id; k++)
                n += (was->lk[k].ts & 63u) == i;
            if (n != plk_count(t, i))
                return "a step not rewritten lost its lock";
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    static snap_t a, b, c;
    uint32_t i, p, ok, n, x, y, d, lo, hi;
    outdir = argc > 1 ? argv[1] : "build/host";
    memset(trk, 0, sizeof trk);
    memset(&song, 0, sizeof song);
    host_tracks_init();
    rng_state = 0x6121D5u;

    {   /* ---- the map */
        uint32_t h = 2166136261u;
        static const struct { uint8_t step, part, x, y, v; } REF[] = {
            {0, 0, 0, 0, 143}, {0, 0, 128, 128, 253}, {8, 1, 128, 128, 253}, {4, 2, 100, 200, 244},
            {12, 0, 255, 255, 7}, {30, 2, 37, 90, 66}, {24, 1, 64, 0, 216}, {6, 0, 191, 250, 2},
        };
        for (i = 0; i < 25u; i++)
            for (p = 0; p < 96u; p++)
                h = (h ^ GRIDS_NODE[i][p]) * 16777619u;
        ck(h == 0x649CC492u, "the 25 nodes as Grids' resources.cc (FNV-1a of node_0 .. node_24)");
        for (i = 0, ok = 1; i < sizeof REF / sizeof REF[0]; i++) {
            grids_at(REF[i].x, REF[i].y);
            ok &= grids_level(REF[i].step, REF[i].part) == REF[i].v;
        }
        ck(ok, "ReadDrumMap at known X / Y (bilinear between the four nodes, U8Mix)");
        grids_at(128, 128);
        ck(grids_level(0, 0) == 255u * 255u / 256u * 255u / 256u && grids_n[0] == GRIDS_NODE[4],
           "X = Y = 128: node 4 (drum_map[2][2]), its level a little under (255 -> 253)");
        ck(g8(0) == 0u && g8(64) == 129u && g8(127) == 255u, "the knobs 0..127 over 0..255");
    }

    {   /* ---- density */
        static uint8_t hit[3][NSTEP];
        setup(64, 2);
        snap(TDRUM, &a);
        for (x = 0, ok = 1; x <= 127u; x += 21u)
            for (y = 0; y <= 127u; y += 18u) {
                knobs(x, y, 0, 127, 77u + x);
                grids_make(TDRUM);
                ok &= ksh_hits(TDRUM, 64) == 0u;
            }
        ck(ok, "DENSITY 0: no kick, snare or hat anywhere on the map (CHAOS up too)");
        ck(!kept_bad(TDRUM, &a), "... the other lanes stay");
        for (x = 0, ok = 1, lo = 1000, hi = 0; x <= 127u; x += 25u)
            for (y = 0; y <= 127u; y += 25u) {
                memset(hit, 0, sizeof hit);
                for (d = 0; d <= 127u; d += 8u) {
                    knobs(x, y, d, 0, 5);
                    grids_make(TDRUM);
                    for (i = 0; i < 16u; i++)
                        for (p = 0; p < 3u; p++) {
                            uint32_t on = dstep_has(&TDRUM->dstep[i], GRIDS_LANE[p]);
                            ok &= on || !hit[p][i];
                            hit[p][i] = (uint8_t)on;
                        }
                }
                knobs(x, y, 127, 0, 5);
                grids_make(TDRUM);
                grids_at(g8(x), g8(y));
                for (i = 0, n = 0; i < 16u; i++)
                    for (p = 0; p < 3u; p++) {
                        ok &= dstep_has(&TDRUM->dstep[i], GRIDS_LANE[p]) == (grids_level(2u * i, p) > 0u);
                        n += dstep_has(&TDRUM->dstep[i], GRIDS_LANE[p]);
                    }
                lo = n < lo ? n : lo;
                hi = n > hi ? n : hi;
            }
        ck(ok, "DENSITY up: the hits there were stay; 127: a hit wherever the map is above 0");
        printf("grids: hits a bar at DENSITY 127: %u .. %u of 48\n", lo, hi);
        ck(lo >= 20u, "DENSITY 127: dense everywhere on the map");
    }

    {   /* ---- levels, ratchets, DIV 1/32 */
        uint32_t lv[4] = {0, 0, 0, 0}, rats = 0;
        setup(32, 2);
        for (x = 0, ok = 1; x <= 127u; x += 9u)
            for (y = 0; y <= 127u; y += 13u) {
                uint32_t dn = (x + y) % 2u ? 70u : 120u, th = 255u - g8(dn);
                knobs(x, y, dn, 0, 9);
                grids_make(TDRUM);
                grids_at(g8(x), g8(y));
                for (i = 0; i < 32u; i++)
                    for (p = 0; p < 3u; p++) {
                        const dstep_t *s = &TDRUM->dstep[i];
                        uint32_t l = GRIDS_LANE[p], v = grids_level(2u * (i % 16u), p), w = grids_level(2u * (i % 16u) + 1u, p);
                        if (v <= th) {
                            ok &= !dstep_has(s, l);
                            continue;
                        }
                        ok &= dstep_has(s, l) && dstep_lvl(s, l) == (v > 192u ? LV_HARD : v > 128u ? LV_NORM : v > 64u ? LV_SOFT : LV_GHOST);
                        ok &= dstep_rat(s, l) == (w > th);
                        lv[dstep_lvl(s, l)]++;
                        rats += dstep_rat(s, l);
                    }
            }
        ck(ok, "each hit's level from the map (over 192 HARD, 128 NORM, 64 SOFT, else GHOST), none under");
        ck(lv[LV_HARD] && lv[LV_NORM] && lv[LV_SOFT] && lv[LV_GHOST] && rats,
           "... every level comes (GHOST at a high DENSITY); the 32nd after a hit: its ratchet x2");
        setup(64, 3);                                     /* DIV 1/32: 32 steps a bar */
        for (x = 0, ok = 1, rats = 0; x <= 127u; x += 31u)
            for (y = 0; y <= 127u; y += 31u) {
                knobs(x, y, 100, 0, 9);
                grids_make(TDRUM);
                grids_at(g8(x), g8(y));
                for (i = 0; i < 64u; i++)
                    for (p = 0; p < 3u; p++) {
                        uint32_t l = GRIDS_LANE[p];
                        ok &= dstep_has(&TDRUM->dstep[i], l) == (grids_level(i % 32u, p) > 255u - g8(100));
                        rats += dstep_rat(&TDRUM->dstep[i], l);
                    }
            }
        ck(ok && !rats, "DIV 1/32: the map's 32 steps one a step, no ratchets");
    }

    {   /* ---- chaos */
        static uint8_t base[NSTEP][3];
        uint32_t seeds = 0, bars = 0, more = 0;
        setup(64, 2);
        knobs(40, 90, 55, 0, 123);
        grids_make(TDRUM);
        for (i = 0, ok = 1; i < 64u; i++)
            for (p = 0; p < 3u; p++)
                ok &= dstep_has(&TDRUM->dstep[i], GRIDS_LANE[p]) == dstep_has(&TDRUM->dstep[i % 16u], GRIDS_LANE[p]) &&
                      dstep_lvl(&TDRUM->dstep[i], GRIDS_LANE[p]) == dstep_lvl(&TDRUM->dstep[i % 16u], GRIDS_LANE[p]);
        ck(ok, "CHAOS 0: every bar the same");
        for (i = 0; i < 64u; i++)
            for (p = 0; p < 3u; p++)
                base[i][p] = (uint8_t)dstep_has(&TDRUM->dstep[i], GRIDS_LANE[p]);
        n = ksh_hits(TDRUM, 64);
        snap(TDRUM, &a);
        for (d = 1, ok = 1; d <= 8u; d++) {
            knobs(40, 90, 55, 127, d * 0x9E3779B9u);
            grids_make(TDRUM);
            for (i = 0; i < 64u; i++)
                for (p = 0; p < 3u; p++)
                    ok &= dstep_has(&TDRUM->dstep[i], GRIDS_LANE[p]) || !base[i][p];
            more += ksh_hits(TDRUM, 64) > n;
            seeds += !same(TDRUM, &a);
            for (i = 16; i < 64u; i++)
                if (memcmp(&TDRUM->dstep[i], &TDRUM->dstep[i % 16u], sizeof(dstep_t))) {
                    bars++;
                    break;
                }
            if (d == 1u)
                snap(TDRUM, &b);
            if (d == 2u)
                snap(TDRUM, &c);
        }
        printf("grids: CHAOS 127, 8 seeds: %u with more hits, %u with bars that differ\n", more, bars);
        ck(ok && more >= 6u, "CHAOS up: the hits there were stay, more come");
        ck(bars >= 6u && memcmp(&b, &c, sizeof b), "... the bars differ, and the draws of two seeds");
        knobs(40, 90, 55, 127, 1u * 0x9E3779B9u);
        grids_make(TDRUM);
        ck(!memcmp(TDRUM->dstep, b.st, sizeof b.st), "the same knobs and seed: the same pattern");
    }

    {   /* ---- what stays: the other lanes, past LEN, conditions and locks */
        static const uint16_t L[4] = {16, 32, 12, 64};
        for (i = 0, ok = 1; i < 4u; i++)
            for (d = 0; d <= 127u; d += 42u) {
                const char *why;
                setup(L[i], i == 3u ? 3u : 2u);
                snap(TDRUM, &a);
                knobs(30u + 20u * i, 100u - 15u * i, d, i * 40u, i + 3u);
                grids_make(TDRUM);
                if ((why = kept_bad(TDRUM, &a)) != 0) {
                    printf("grids: LEN %u DENSITY %u: %s\n", L[i], d, why);
                    ok = 0;
                }
            }
        ck(ok, "the other lanes and the steps past LEN stay; a step rewritten: no condition, no lock");
        setup(16, 2);
        knobs(64, 64, 0, 0, 1);
        grids_make(TDRUM);
        ck(TDRUM->cond[1] == CN_P50 && plk_count(TDRUM, 1) == 1u && dstep_has(&TDRUM->dstep[1], LANE_BELL) &&
           TDRUM->cond[11] == CN_P50 && plk_count(TDRUM, 11) == 1u && !TDRUM->cond[4] && !TDRUM->cond[10],
           "DENSITY 0: the steps of the bell or a lock alone keep theirs, a kick's / hat's step loses its condition");
    }

    {   /* ---- the gesture, on the real UI */
        static snap_t orig, t1, t2;
        uint32_t seed;
        panel = PANEL_DEFAULT;
        layers_init();
        host_tracks_init();
        for (i = 0; i < NPART; i++) {
            set_engine_of(&trk[i], TRK_DEF[i][0]);
            apply_preset_to(&trk[i], TRK_DEF[i][1]);
            trk[i].engine = trk[i].eng_req;
        }
        TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;
        knobs(64, 64, 64, 0, 1);
        grids.last = 0xFF;
        song.sel = TRK_DRUM;
        go_home();
        frame();
        setup(16, 2);
        snap(TDRUM, &orig);
        drum_page = 0;
        tap(B_SEQ);
        ck(on_drum_page() && drum_page == 0u, "TRACKS, the drum track, SEQ tapped: DRUMS (grid)");
        tap(B_SEQ);
        tap(B_SEQ);
        ck(on_drum_page() && drum_page == 2u, "SEQ tapped twice more: kit, then MAP");
        frames(70);
        for (i = 0; i < 4u; i++) {
            encs[panel.enc[EN_K3]] = 5;
            frames(4);                                    /* (64 ms apart: no acceleration) */
        }
        snap(TDRUM, &t1);
        ck(grids.k[2] == 84u && !same(TDRUM, &orig) && !kept_bad(TDRUM, &orig), "KNOB 3 DENSITY right: the kick, snare and hat follow, the rest stays");
        ui.force = 1;
        frame();
        ppm("live-map");
        frames(70);
        encs[panel.enc[EN_K1]] = 9;
        frame();
        snap(TDRUM, &c);
        encs[panel.enc[EN_K2]] = -20;
        frame();
        snap(TDRUM, &t2);
        ck(grids.k[0] == 73u && grids.k[1] == 44u && !same(TDRUM, &t1), "KNOB 1 X, KNOB 2 Y: another groove");
        press(B_EDIT);
        frames(10);
        edges_btn |= BT(B_OCTDN); fm1_in.buttons |= BT(B_OCTDN); frame(); fm1_in.buttons &= ~BT(B_OCTDN); frame();
        ck(same(TDRUM, &c), "EDIT + OCT-: the last touch (KNOB 2) undone, exactly");
        edges_btn |= BT(B_OCTUP); fm1_in.buttons |= BT(B_OCTUP); frame(); fm1_in.buttons &= ~BT(B_OCTUP); frame();
        ck(same(TDRUM, &t2), "EDIT + OCT+: redo, exactly");
        release(B_EDIT);
        ck(on_drum_page() && drum_page == 2u, "EDIT let go: still the MAP");
        /* one touch, one undo: KNOB 3 turned over many frames, undone at once */
        frames(70);
        snap(TDRUM, &t1);
        for (i = 0; i < 12u; i++) {
            encs[panel.enc[EN_K3]] = i < 6u ? 4 : -2;
            frame();
            frame();
        }
        ck(!same(TDRUM, &t1), "KNOB 3 turned right and back over 24 frames: the pattern followed");
        press(B_EDIT);
        frames(10);
        edges_btn |= BT(B_OCTDN); fm1_in.buttons |= BT(B_OCTDN); frame(); fm1_in.buttons &= ~BT(B_OCTDN); frame();
        ck(same(TDRUM, &t1), "EDIT + OCT-: the whole touch undone at once, exactly (conditions, locks)");
        edges_btn |= BT(B_OCTUP); fm1_in.buttons |= BT(B_OCTUP); frame(); fm1_in.buttons &= ~BT(B_OCTUP); frame();
        release(B_EDIT);
        /* CHAOS: a touch draws, the same touch keeps the draw, another touch draws anew */
        frames(70);
        encs[panel.enc[EN_K4]] = 100;
        frames(4);
        seed = grids.seed;
        encs[panel.enc[EN_K4]] = -1;
        frames(4);
        encs[panel.enc[EN_K4]] = 1;
        frames(4);
        ck(grids.k[3] == 100u && grids.seed == seed, "KNOB 4 CHAOS: one touch, one draw");
        snap(TDRUM, &t1);
        frames(70);
        encs[panel.enc[EN_K4]] = -1;
        frames(4);
        encs[panel.enc[EN_K4]] = 1;
        frames(4);
        ck(grids.seed != seed && grids.k[3] == 100u, "CHAOS touched again: a new draw");
        encs[panel.enc[EN_K1]] = 200;
        frame();
        ck(grids.k[0] == 127u, "the knobs stop at 127");
        ui.force = 1;
        transport_req = 1;
        frames(20);
        ui.force = 1;
        frame();
        ppm("live-map-play");
        transport_req = 2;
        frames(2);
        tap(B_SEQ);
        ck(on_drum_page() && drum_page == 0u, "SEQ tapped: back to the grid");
    }

    printf(bad ? "grids: %d FAILED\n" : "grids: all ok\n", bad);
    return bad != 0;
}
