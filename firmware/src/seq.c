/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Keyboard and its layers, scale and chords, arpeggiator, note repeat, sequencer, recording and
 * transport. Runs in the audio ISR, once per CTL-sample block (events_block), and ends in
 * trk_note_on / trk_note_off / drum_on: engines never see where a note came from.
 *
 * Time: the transport clock of fx.c (clk_beat, clk_pos, in units of a sample at 1 BPM). Every track
 * reads its step from it (trk_grid): the step is the clock divided by the track's DIV, swung, modulo
 * its LEN. So the tracks, the click, the arp, the rolls and the song arranger never drift apart, a
 * tempo or DIV change plays at most one step a block, and a polymeter (any LEN) stays in phase.
 *
 * Four tracks: tracks 1..3 are synth parts (steps of up to 4 notes), track 4 the drum track (steps
 * of 16 lanes, one per white key). Each step note / lane has a level (ghost .. hard) and a ratchet
 * (x1..x4 hits in its step). The keys play the selected track; MIDI channels 1..3 play parts 1..3,
 * the DRUMS channel (GLO > DRUMS, default 10) the drum track, any other channel the selected track.
 *
 * Layers: a function button held turns the keys into something else (TE style: hold + touch):
 *   FX   punch-in effects (punch.c)       EDIT  erase that note / sound (while held, as it plays)
 *        (black keys: FILL)
 *   ARP  note repeat (roll) at G_ROLL     SEQ   steps 1..16 (the UI: ui_layers.c)
 *   SCL  the key of the song (the UI)     GLO   mute / solo / tap tempo (the UI)
 * On the drum track OCT- / OCT+ held play (and record) ghost / hard hits. */
static const uint16_t SCALE_MASK[] = {
    0xFFF,                                   /* CHR */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),   /* MAJ */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),   /* MIN */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),   /* DOR */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10),   /* MIX */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9),                          /* PEN */
    (1 << 0) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 10),                         /* MPEN */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 11),   /* HARM */
    (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10),   /* PHRY */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9) | (1 << 11),   /* LYD */
    (1 << 0) | (1 << 1) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 8) | (1 << 10),   /* LOC */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),   /* MEL (ascending) */
    (1 << 0) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 10),              /* BLUES (minor) */
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 8) | (1 << 10),              /* WHOLE */
    (1 << 0) | (1 << 1) | (1 << 3) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9) | (1 << 10), /* DIMHW */
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 8) | (1 << 9) | (1 << 11), /* DIMWH */
};
#define NSCALES (sizeof SCALE_MASK / sizeof SCALE_MASK[0])
#define SEQ_NONE 0xFFFFFFFFu

#define KB_SILENT 255u
static uint32_t kb_prev;
/* per key: what its press started, so its release ends the same (whatever the layer or track is now) */
enum { KS_NONE, KS_NOTE, KS_DRUM, KS_ROLL, KS_ERASE, KS_FX, KS_UI, KS_FILL };
static uint8_t kb_kind[27], kb_trk[27], kb_n[27], kb_nt[27][4];
static uint8_t last_note = 60;
static uint8_t pen_n = 1, pen_note[4] = {60};   /* the last chord / note played: the SEQ layer writes it */
static uint8_t pen_lane;                       /* the last drum lane played: the SEQ layer's lane */
static volatile uint8_t transport_req;   /* 1 start, 2 stop (from the UI) */
static volatile uint8_t panic_req;       /* bit per track: release every sounding note (preset / engine change) */

static uint32_t trk_index(const track_t *t) { return (uint32_t)(t - trk); }

static uint32_t trk_midi_ch(uint32_t i)    /* MIDI channel 0..15 of track i (keys -> MIDI out) */
{
    if (i < NPART)
        return i;
    return song.g[G_DRCH] ? (uint32_t)song.g[G_DRCH] - 1u : 9u;
}

static uint32_t scale_mask(const track_t *t)
{
    return SCALE_MASK[clamp(t->p[P_SCALE], 0, NSCALES - 1)];
}

/* ---------------------------------------------------------- layers --- */
enum { LY_PLAY, LY_FX, LY_ERASE, LY_ROLL, LY_STEP, LY_SCALE, LY_MIX, LY_SONG, LY_COUNT };
static uint32_t ly_bit[LY_COUNT];        /* the button (fm1_in.buttons bit) of each layer: the UI sets them */
static uint32_t dyn_bit[2];              /* OCT- / OCT+: ghost / hard on the drum track */
/* a layer locked open (its button held + HOME tapped: ui_input.c), LY_PLAY = none: the keys and knobs
 * stay in it with the button let go, as if it were held */
static volatile uint8_t ly_lock = LY_PLAY;
static uint32_t layer_buttons(void) { return fm1_in.buttons | (ly_lock != LY_PLAY ? ly_bit[ly_lock % LY_COUNT] : 0u); }
/* the layer the keys are in: the held function button (FX, EDIT, ARP, SEQ, SCL, GLO in that order),
 * else the locked one */
static uint32_t layer_now(void)
{
    uint32_t b = layer_buttons(), l;
    for (l = LY_FX; l < LY_COUNT; l++)
        if (b & ly_bit[l])
            return l;
    return LY_PLAY;
}
/* the keys of the layers the UI handles (steps, key, mix): key k down / up, in order */
#define LKQ 16u
static volatile uint16_t lk_q[LKQ];
static volatile uint32_t lk_w, lk_r;
static void lk_push(uint32_t layer, uint32_t k, uint32_t down)
{
    if (lk_w - lk_r < LKQ) {
        lk_q[lk_w % LKQ] = (uint16_t)(layer << 8 | down << 7 | k);
        RING_PUBLISH();
        lk_w++;
    }
}

/* ------------------------------------------------------------- keys --- */
/* key k -> note on a synth part (KB_SILENT: none). WHITE (and chord mode): the white keys walk the
 * scale from C4 = the root, the black keys are silent; SNAP: every key, rounded down into the scale */
static uint32_t kb_map(const track_t *t, uint32_t k)
{
    static const int8_t DEGREE[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};
    int32_t n = 53 + (int32_t)k;
    if (is_drum(t))
        return LANE_NOTE[lane_of_key(k)];
    if (ENGINES[t->eng_req % NENGINES] == &ENG_SAMPLE && drum_set() >= 0 &&   /* (the engine it switches to) */
        (uint32_t)t->p[P_E0] % SMP_NSETS == (uint32_t)drum_set())   /* GM KIT: lowest key = kick (C2), no scale */
        return (uint32_t)clamp(36 + 12 * song.octave + (int32_t)k, 0, 127);
#if FELUCCA_SLICE
    if (ENGINES[t->eng_req % NENGINES] == &ENG_SLICE)   /* SLICE: lowest key = slice 0 (C4 + ROOT), no scale */
        return (uint32_t)clamp(SLC_BASE + t->p[P_ROOT] + 12 * song.octave + (int32_t)k, 0, 127);
#endif
    if (t->p[P_QUANT] == 1 && !t->p[P_CHORD]) {  /* SNAP: every key, rounded down to the scale (the old ON) */
        uint32_t mask = scale_mask(t), guard = 12;
        n += 12 * song.octave + t->p[P_TRANS];
        while (guard-- && !((mask >> (uint32_t)((n - t->p[P_ROOT] + 120) % 12)) & 1u))
            n--;
        return (uint32_t)clamp(n, 0, 127);
    }
    if (t->p[P_QUANT] == 2 || t->p[P_CHORD]) {   /* WHITE: white keys walk the scale, black keys are silent */
        uint32_t mask = t->p[P_CHORD] && !t->p[P_SCALE] ? SCALE_MASK[2] : scale_mask(t), i;
        int32_t count = 0, degree = DEGREE[n % 12], oct;
        if (degree < 0)
            return KB_SILENT;
        /* C4 is the root. Walk scale degrees on successive white keys, including
         * below C4; scales with 5, 6, 8 or 12 notes still have no duplicated degrees. */
        degree += (n / 12 - 5) * 7;
        for (i = 0; i < 12u; i++)
            count += (mask >> i) & 1u;
        oct = degree / count;
        degree %= count;
        if (degree < 0) {
            degree += count;
            oct--;
        }
        for (i = 0; i < 12u; i++)
            if ((mask >> i) & 1u) {
                if (!degree)
                    break;
                degree--;
            }
        n = 60 + t->p[P_ROOT] + 12 * oct + (int32_t)i;
    }
    return (uint32_t)clamp(n + 12 * song.octave + t->p[P_TRANS], 0, 127);
}

/* chord mode (P_CHORD): the chord of the scale built on note n (in the scale; CHR: minor), into c[];
 * the notes it holds (<= 4, the most a step keeps) */
static const int8_t CHORD_DEG[6][4] = {
    {0, -1, -1, -1},                     /* OFF */
    {0, 2, 4, -1},                       /* TRIAD: 1 3 5 */
    {0, 2, 4, 6},                        /* 7TH: 1 3 5 7 */
    {0, 2, 6, 8},                        /* 9TH: 1 3 7 9 (the lo-fi / R&B voicing) */
    {0, 3, 4, -1},                       /* SUS4: 1 4 5 */
    {0, -1, -1, -1},                     /* POWER: 1 5 8 (semitones, below) */
};
static uint32_t chord_notes(const track_t *t, uint32_t n, uint8_t *c)
{
    uint32_t type = (uint32_t)clamp(t->p[P_CHORD], 0, 5), mask = t->p[P_SCALE] ? scale_mask(t) : SCALE_MASK[2];
    uint32_t k = 0, j;
    if (type == 5u) {
        static const uint8_t PW[3] = {0, 7, 12};
        for (j = 0; j < 3u; j++)
            if (n + PW[j] < 128u)
                c[k++] = (uint8_t)(n + PW[j]);
        return k;
    }
    for (j = 0; j < 4u && CHORD_DEG[type][j] >= 0; j++) {
        int32_t m = (int32_t)n, d = CHORD_DEG[type][j], guard = 48;
        while (d > 0 && guard--) {                       /* d scale degrees up */
            m++;
            if ((mask >> (uint32_t)((m - t->p[P_ROOT] + 120) % 12)) & 1u)
                d--;
        }
        if (m < 128)
            c[k++] = (uint8_t)m;
    }
    return k;
}

/* ------------------------------------------------------------- grid --- */
/* units an odd step starts late: the track's + the global SWING (MPC: 0 = 50 %, 100 = 75 %) */
static uint32_t swing_units(int32_t pct, uint32_t u)
{
    return (uint32_t)clamp(pct, 0, 100) * u / 200u;
}
/* the clock on a grid of den steps a beat, odd steps sw late: the step, units into it, its length */
static uint32_t grid_at(uint32_t den, uint32_t sw, uint32_t *into, uint32_t *len)
{
    uint32_t u = BEAT_U / den, abs = clk_beat * den + clk_pos / u, frac = clk_pos % u;
    if (abs & 1u) {                                      /* an odd step: sw late */
        if (frac < sw) {
            abs--;
            frac += u;
            *len = u + sw;
        } else {
            frac -= sw;
            *len = u - sw;
        }
    } else {
        *len = u + sw;
    }
    *into = frac;
    return abs;
}
static uint32_t trk_grid(const track_t *t, uint32_t *into, uint32_t *len)
{
    uint32_t den = DIV_DEN[(uint32_t)t->p[P_SDIV] % 6u];
    return grid_at(den, swing_units(t->p[P_SSWING] + song.g[G_SWING], BEAT_U / den), into, len);
}
static uint32_t trk_len(const track_t *t) { return t->p[P_SLEN] > 0 ? (uint32_t)t->p[P_SLEN] : 1u; }

/* ------------------------------------------------------- conditions --- */
/* Each step has a condition (track_t cond[], SEQ + a step held + KNOB 4; the drum track: one for all the
 * lanes of the step). ALWAYS plays as ever; a step whose condition fails is a rest (no note, no tie, no
 * ratchet). An empty step has none (ALWAYS):
 *   12 % .. 88 %   a chance, drawn each time the step comes round
 *   a:b            on pass a of every b of the track's pattern (passes since PLAY or a section, each track
 *                  on its own length)
 *   FILL / !FILL   only while FILL is held (FX + a black key) / only while it is not
 *   1ST / !1ST     only on the first pass / on every pass but the first */
enum { CN_ALWAYS, CN_P12, CN_P25, CN_P50, CN_P75, CN_P88, CN_1_2, CN_2_2, CN_1_3, CN_2_3, CN_3_3, CN_1_4, CN_2_4, CN_3_4,
       CN_4_4, CN_FILL, CN_NFILL, CN_FIRST, CN_NFIRST, CN_COUNT };
static const char *const N_COND[CN_COUNT] = {"ALWAYS", "12%", "25%", "50%", "75%", "88%", "1:2", "2:2", "1:3", "2:3",
                                             "3:3", "1:4", "2:4", "3:4", "4:4", "FILL", "!FILL", "1ST", "!1ST"};
static const uint8_t CN_CHANCE[5] = {32, 64, 128, 192, 224};        /* x / 256 */
static const uint8_t CN_CYCLE[9] = {0x12, 0x22, 0x13, 0x23, 0x33, 0x14, 0x24, 0x34, 0x44};   /* a << 4 | b */
static uint32_t fill_keys;               /* FILL: the black keys held in the FX layer (key_down) */

static int step_sounds(const track_t *t, uint32_t k)   /* step k holds notes (synth) / a hit (drums) */
{
    return is_drum(t) ? dstep_mask(&t->dstep[k]) != 0u : t->step[k].time == ST_NOTE && t->step[k].n;
}

static int cond_ok(const track_t *t, uint32_t c)
{
    uint32_t p = t->seq_pass ? t->seq_pass - 1u : 0u;   /* (the first pass: 0) */
    if (c >= CN_COUNT)
        return 1;
    if (c >= CN_P12 && c <= CN_P88)
        return (rng() & 255u) < CN_CHANCE[c - CN_P12];
    if (c >= CN_1_2 && c <= CN_4_4)
        return p % (CN_CYCLE[c - CN_1_2] & 15u) == (CN_CYCLE[c - CN_1_2] >> 4) - 1u;
    switch (c) {
    case CN_FILL:
        return fill_keys != 0u;
    case CN_NFILL:
        return !fill_keys;
    case CN_FIRST:
        return !p;
    case CN_NFIRST:
        return p != 0u;
    default:
        return 1;
    }
}

/* -------------------------------------------------- parameter locks --- */
/* A step can hold its own value of a sound parameter (P-LOCK: ui_layers.c): while the step plays (and
 * the TIE steps after it) its track plays with that value, then with its own again. The locks are a
 * pool of the project (plk[], up to PLK_MAX, PLK_STEP a step), edited by the UI with the IRQ off; every
 * change bumps plk_gen and the steps playing read theirs again. The ISR swaps the locked values into p[]
 * only while it renders a block (mix_block: plk_block_in .. plk_block_out), the track's own kept in
 * lk_base: the UI, the editor and a saved project see the track's own values only, and a knob turned
 * while a lock plays changes the track's own value. Locks follow their steps (shift, x2, undo, erase,
 * clear) and go with them. */
static plk_t plk[PLK_MAX] __attribute__((section(".pool")));
static volatile uint8_t plk_gen;              /* bumped by every change of plk[] */
static uint8_t plk_seen;                      /* the plk_gen the locks playing were read from */
#define PLK_NONE 0xFFu

static uint32_t plk_ts(const track_t *t, uint32_t idx) { return trk_index(t) << 6 | (idx & 63u); }

/* what a step can lock: the sound (ENV, LFO, FX sends, SLICER, EDIT, glide, detune, pan), not the
 * pattern, the arp, the key, the voice mode, the level or MUTE; the drum track: its SLICER */
static int plk_lockable(const track_t *t, uint32_t id)
{
    if (is_drum(t))
        return id >= P_SLCR && id <= P_SLDEPTH;
    return (id >= P_ATK && id <= P_ED_SHP) || (id >= P_LRATE && id <= P_LD_AMP) || (id >= P_DIST && id <= P_REV) ||
           (id >= P_SLCR && id <= P_SLDEPTH) || id == P_GLIDE || id == P_DETUNE || id == P_PAN ||
           (id >= P_E0 && id <= P_E7);
}
/* the range of a lock (EDIT: the engine the track asked for, as its p[]) */
static const param_desc_t *plk_desc(const track_t *t, uint32_t id)
{
    return id >= P_E0 && id <= P_E7 ? &ENGINES[t->eng_req % NENGINES]->edit[id - P_E0] : &TP[id];
}

static int plk_find(uint32_t ts, uint32_t id)  /* the entry of lock id on step ts, -1 none */
{
    uint32_t i;
    for (i = 0; i < PLK_MAX; i++)
        if (plk[i].id == id + 1u && plk[i].ts == ts)
            return (int)i;
    return -1;
}
static uint32_t plk_count(const track_t *t, uint32_t idx)   /* the locks of step idx */
{
    uint32_t i, n = 0, ts = plk_ts(t, idx);
    for (i = 0; i < PLK_MAX; i++)
        n += plk[i].id && plk[i].ts == ts;
    return n;
}
static uint32_t plk_marks(const track_t *t, uint32_t bank)   /* bit per step of the bank (16) with locks */
{
    uint32_t i, m = 0, k = trk_index(t);
    for (i = 0; i < PLK_MAX; i++)
        if (plk[i].id && plk[i].ts >> 6 == k && (plk[i].ts & 63u) / 16u == bank)
            m |= 1u << (plk[i].ts & 15u);
    return m;
}
static int plk_get(const track_t *t, uint32_t idx, uint32_t id, int16_t *v)
{
    int i = plk_find(plk_ts(t, idx), id);
    if (i >= 0)
        *v = plk[i].v;
    return i >= 0;
}
/* lock id of step idx at v (into its range); 0: no room (PLK_STEP on the step, or the pool full) */
static int plk_set(const track_t *t, uint32_t idx, uint32_t id, int32_t v)
{
    const param_desc_t *d = plk_desc(t, id);
    int k = plk_find(plk_ts(t, idx), id);
    if (k < 0) {
        if (plk_count(t, idx) >= PLK_STEP)
            return 0;
        for (k = 0; k < PLK_MAX && plk[k].id; k++)
            ;
        if (k == PLK_MAX)
            return 0;
        plk[k].ts = (uint8_t)plk_ts(t, idx);
    }
    plk[k].v = (int16_t)clamp(v, d->min, d->max);
    plk[k].id = (uint8_t)(id + 1u);                 /* (last: a new entry is whole when it counts) */
    plk_gen++;
    return 1;
}
static uint32_t plk_clear_step(const track_t *t, uint32_t idx)   /* the locks of step idx go; how many */
{
    uint32_t i, n = 0, ts = plk_ts(t, idx);
    for (i = 0; i < PLK_MAX; i++)
        if (plk[i].id && plk[i].ts == ts) {
            plk[i].id = 0;
            n++;
        }
    if (n)
        plk_gen++;
    return n;
}
static void plk_clear_track(const track_t *t)
{
    uint32_t i, k = trk_index(t);
    for (i = 0; i < PLK_MAX; i++)
        if (plk[i].ts >> 6 == k)
            plk[i].id = 0;
    plk_gen++;
}
/* EDIT SHIFT: the locks of steps 0..len-1 one step later (d > 0) / earlier, round, as the steps */
static void plk_rotate(const track_t *t, uint32_t len, int32_t d)
{
    uint32_t i, k = trk_index(t);
    for (i = 0; i < PLK_MAX; i++) {
        uint32_t s = plk[i].ts & 63u;
        if (plk[i].id && plk[i].ts >> 6 == k && s < len)
            plk[i].ts = (uint8_t)(k << 6 | (d > 0 ? (s + 1u) % len : (s + len - 1u) % len));
    }
    plk_gen++;
}
/* LENGTH x2: the locks of steps 0..len-1 again on len.. (theirs go first); 0: the pool was full */
static int plk_double(const track_t *t, uint32_t len)
{
    uint32_t i, k = trk_index(t);
    int ok = 1;
    for (i = len; i < 2u * len && i < NSTEP; i++)
        plk_clear_step(t, i);
    for (i = 0; i < PLK_MAX; i++)                   /* (the copies land on steps >= len: never copied again) */
        if (plk[i].id && plk[i].ts >> 6 == k && (plk[i].ts & 63u) < len && (plk[i].ts & 63u) + len < NSTEP)
            ok &= plk_set(t, (plk[i].ts & 63u) + len, plk[i].id - 1u, plk[i].v);
    return ok;
}
/* the locks of t into buf (PLK_MAX entries, the rest free), and back (what does not fit is lost) */
static void plk_save(const track_t *t, plk_t *buf)
{
    uint32_t i, n = 0, k = trk_index(t);
    memset(buf, 0, PLK_MAX * sizeof *buf);
    for (i = 0; i < PLK_MAX; i++)
        if (plk[i].id && plk[i].ts >> 6 == k)
            buf[n++] = plk[i];
}
static void plk_restore(const track_t *t, const plk_t *buf)
{
    uint32_t i, j = 0;
    plk_clear_track(t);
    for (i = 0; i < PLK_MAX && buf[i].id; i++) {
        while (j < PLK_MAX && plk[j].id)
            j++;
        if (j == PLK_MAX)
            break;
        plk[j] = buf[i];
    }
}

/* ---- playing them (audio ISR) */
static void plk_in(track_t *t)                  /* the locks playing into p[], the track's own kept */
{
    uint32_t i;
    if (t->lk_on)
        return;
    for (i = 0; i < t->lk_n; i++) {
        const param_desc_t *d = plk_desc(t, t->lk_id[i]);   /* (another engine since: its range) */
        t->lk_base[i] = t->p[t->lk_id[i]];
        t->p[t->lk_id[i]] = (int16_t)clamp(t->lk_v[i], d->min, d->max);
    }
    t->lk_on = 1;
}
static void plk_out(track_t *t)                 /* the track's own values back */
{
    uint32_t i = t->lk_n;
    if (!t->lk_on)
        return;
    while (i--)
        t->p[t->lk_id[i]] = t->lk_base[i];
    t->lk_on = 0;
}
/* step idx plays: its locks from now on (none: the track's own values) */
static void plk_load(track_t *t, uint32_t idx)
{
    uint32_t i, n = 0, ts = plk_ts(t, idx), on = t->lk_on;
    plk_out(t);
    for (i = 0; i < PLK_MAX && n < PLK_STEP; i++)
        if (plk[i].id && plk[i].id <= P_COUNT && plk[i].ts == ts) {
            t->lk_id[n] = (uint8_t)(plk[i].id - 1u);
            t->lk_v[n++] = plk[i].v;
        }
    t->lk_n = (uint8_t)n;
    t->lk_step = (uint8_t)idx;
    if (on)
        plk_in(t);
}
static void plk_drop(track_t *t)                /* stop, a section, a load: no lock plays */
{
    plk_out(t);
    t->lk_n = 0;
    t->lk_step = PLK_NONE;
}
static void plk_block_in(void)                  /* mix_block, before the block's events */
{
    uint32_t i;
    if (plk_seen != plk_gen) {                   /* the locks changed: the steps playing read theirs again */
        plk_seen = plk_gen;
        for (i = 0; i < NTRK; i++)
            if (song.playing && trk[i].lk_step < NSTEP)
                plk_load(&trk[i], trk[i].lk_step);
    }
    for (i = 0; i < NTRK; i++)
        plk_in(&trk[i]);
}
static void plk_block_out(void)                 /* mix_block, after the block */
{
    uint32_t i;
    for (i = 0; i < NTRK; i++)
        plk_out(&trk[i]);
}
/* p[] of t as the track's own values (a lock playing in it: its base; proj_capture in the ISR) */
static void plk_own(const track_t *t, int16_t *p)
{
    uint32_t i = t->lk_n;
    if (t->lk_on)
        while (i--)
            p[t->lk_id[i]] = t->lk_base[i];
}

/* ------------------------------------------------------------- undo --- */
/* One step back (and forward again) for the pattern of one track: what it was before the last
 * recording pass, erase, step edit, tool or clear (a session: one mark). EDIT + OCT- / OCT+. */
static struct {
    uint8_t valid, undone, trk;
    int16_t len;
    uint32_t sess;
    step_t st[NSTEP];
    uint8_t cond[NSTEP];
} undo;
static plk_t undo_lk[PLK_MAX] __attribute__((section(".pool")));   /* .. and its locks */
static uint32_t undo_sess = 1;           /* UI sessions (seq.c: recording passes use the track's pass) */
static void undo_mark(const track_t *t, uint32_t sess)
{
    uint32_t i = trk_index(t);
    if (undo.valid && !undo.undone && undo.trk == i && undo.sess == sess)
        return;                                          /* (this session is marked already) */
    memcpy(undo.st, t->step, sizeof undo.st);
    memcpy(undo.cond, t->cond, sizeof undo.cond);
    plk_save(t, undo_lk);
    undo.len = t->p[P_SLEN];
    undo.trk = (uint8_t)i;
    undo.sess = sess;
    undo.valid = 1;
    undo.undone = 0;
}
#define UNDO_REC(t) (((t)->pass << 2) | 1u)      /* a recording pass of track t */
static uint32_t undo_erase_sess;

/* ----------------------------------------------------------- mutate --- */
/* EDIT + KNOB 4: the pattern varied a little, a pass a detent; turned back, the passes are undone
 * exactly, the last first. Synth: a note a degree of the scale up / down, a level, a ratchet; rarely a
 * note added near its neighbours (in the scale) or one taken away. Drums: ghost hits on the snares and
 * hats the pattern uses, a hit a step later / earlier, a level, a ratchet on a hat; the kicks on the
 * beats stay. A pass changes at most MUT_STEPS steps, all within LEN; a step never gets more notes
 * (a new one: one), a TIE always follows its note, the notes keep within an octave of each other (or
 * the span they had). */
#define MUT_STEPS 4u                     /* steps a pass changes at most (two changes of up to 2 steps) */
#define MUT_DEPTH 16u                    /* passes kept to turn back */
static struct {
    uint8_t n, trk;                      /* passes kept, their track */
    uint32_t sum;                        /* the pattern after the last one (anything else changed it: they go) */
    uint8_t k[MUT_DEPTH];                /* steps each pass changed */
    uint8_t idx[MUT_DEPTH][MUT_STEPS];
    step_t was[MUT_DEPTH][MUT_STEPS];    /* what they held before */
} mut;
static uint32_t mut_lanes;               /* drums: the lanes the pattern uses (ghosts go only there) */
static uint32_t mut_off;                 /* drums: a hit moves to step (i + mut_off) % LEN */
#define MUT_KICKS (1u << LANE_KICK | 1u << LANE_KICK2)
#define MUT_GHOSTS (1u << LANE_SNARE | 1u << LANE_CLAP | 1u << LANE_RIM | 1u << LANE_SNARE2 | \
                    1u << LANE_HAT | 1u << LANE_PEDAL | 1u << LANE_SHAKER)
#define MUT_HATS (1u << LANE_HAT | 1u << LANE_PEDAL | 1u << LANE_RIDE | 1u << LANE_SHAKER)
static const uint8_t MUT_LV[4] = {LV_GHOST, LV_SOFT, LV_NORM, LV_HARD};   /* softest .. hardest */

static uint32_t pattern_sum(const track_t *t)   /* FNV-1a of the steps, the length and the track */
{
    const uint8_t *p = (const uint8_t *)t->step;
    uint32_t h = 2166136261u ^ trk_index(t) ^ (uint32_t)(uint16_t)t->p[P_SLEN] << 8, i;
    for (i = 0; i < sizeof t->step; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}
static uint32_t mutate_depth(const track_t *t)  /* the passes there are to turn back */
{
    return mut.n && mut.trk == trk_index(t) && mut.sum == pattern_sum(t) ? mut.n : 0u;
}
static int mut_kept(uint32_t idx)                /* step idx changed in this pass already */
{
    uint32_t j;
    for (j = 0; j < mut.k[mut.n]; j++)
        if (mut.idx[mut.n][j] == idx)
            return 1;
    return 0;
}
static void mut_keep(const track_t *t, uint32_t idx)   /* step idx changes: what it held, for the way back */
{
    uint32_t p = mut.n;
    if (!mut_kept(idx) && mut.k[p] < MUT_STEPS) {
        mut.idx[p][mut.k[p]] = (uint8_t)idx;
        mut.was[p][mut.k[p]++] = t->step[idx];
    }
}
/* a random place (step idx, slot: note / lane) where ok() holds, on a step this pass has not changed;
 * 0 = none (so a pass never changes a step twice: no change undoes another) */
static int mut_pick(const track_t *t, int (*ok)(const track_t *, uint32_t, uint32_t), uint32_t *idx, uint32_t *slot)
{
    uint32_t len = trk_len(t), slots = is_drum(t) ? DRUM_LANES : 4u, n = 0, i, k, r;
    for (i = 0; i < len; i++)
        for (k = 0; k < slots && !mut_kept(i); k++)
            n += (uint32_t)ok(t, i, k);
    if (!n)
        return 0;
    r = rng() % n;
    for (i = 0; i < len; i++)
        for (k = 0; k < slots && !mut_kept(i); k++)
            if (ok(t, i, k) && !r--) {
                *idx = i;
                *slot = k;
                return 1;
            }
    return 0;
}
static uint32_t mut_level(uint32_t lv)            /* a level one softer or louder */
{
    uint32_t r = lv == LV_GHOST ? 0u : lv == LV_SOFT ? 1u : lv == LV_NORM ? 2u : 3u;
    return MUT_LV[r == 0u ? 1u : r == 3u ? 2u : (rng() & 1u) ? r + 1u : r - 1u];
}

/* synth parts: the places */
static int mut_tied(const track_t *t, uint32_t i) { return t->step[(i + 1u) % trk_len(t)].time == ST_TIE; }
static int ms_note(const track_t *t, uint32_t i, uint32_t k) { return t->step[i].time == ST_NOTE && k < t->step[i].n && k < 4u; }
static int ms_rat(const track_t *t, uint32_t i, uint32_t k) { return ms_note(t, i, k) && ((t->step[i].rat >> (2u * k)) & 3u); }
static int ms_norat(const track_t *t, uint32_t i, uint32_t k)   /* (a TIE after it or a slide: no ratchet, it holds on) */
{
    return ms_note(t, i, k) && !((t->step[i].rat >> (2u * k)) & 3u) && !mut_tied(t, i) && !(t->step[i].flags & SF_SLIDE);
}
static int ms_empty(const track_t *t, uint32_t i, uint32_t k) { return !k && t->step[i].time != ST_TIE && !ms_note(t, i, 0); }
static int ms_single(const track_t *t, uint32_t i, uint32_t k) { return !k && ms_note(t, i, 0) && t->step[i].n == 1u && !mut_tied(t, i); }
static int32_t scale_step(const track_t *t, int32_t n, int32_t d)   /* the next note of the scale up (d > 0) / down */
{
    uint32_t mask = scale_mask(t), guard = 12;
    do
        n += d;
    while (--guard && !((mask >> (uint32_t)((n - t->p[P_ROOT] + 120) % 12)) & 1u));
    return n;
}
/* where a note may go: within an octave of the others, or the span they have */
static void mut_span(const track_t *t, int32_t *lo, int32_t *hi)
{
    uint32_t len = trk_len(t), i, k;
    int32_t a = 127, b = 0;
    for (i = 0; i < len; i++)
        for (k = 0; ms_note(t, i, k); k++) {
            a = t->step[i].note[k] < a ? t->step[i].note[k] : a;
            b = t->step[i].note[k] > b ? t->step[i].note[k] : b;
        }
    *lo = b - a < 12 ? b - 12 : a;
    *hi = b - a < 12 ? a + 12 : b;
    *lo = clamp(*lo, 0, 127);
    *hi = clamp(*hi, 0, 127);
}
static int mut_fits(const step_t *s, int32_t n, int32_t lo, int32_t hi)   /* note n may go into s */
{
    uint32_t k;
    if (n < lo || n > hi)
        return 0;
    for (k = 0; k < s->n && k < 4u; k++)
        if (s->note[k] == n)
            return 0;
    return 1;
}
static int mut_synth(track_t *t)                  /* one change of a synth part: 1, 0 = none to make */
{
    uint32_t i, k, sh, len = trk_len(t), c = rng() % 16u;
    int32_t lo, hi, n, d;
    step_t *s;
    if (c < 12u) {                                /* a note: its pitch (9 in 16) or its level */
        if (!mut_pick(t, ms_note, &i, &k))
            return 0;
        s = &t->step[i];
        sh = 2u * k;
        if (c >= 9u) {                            /* a level */
            mut_keep(t, i);
            s->lvl = (uint8_t)((s->lvl & ~(3u << sh)) | mut_level((s->lvl >> sh) & 3u) << sh);
            return 1;
        }
        mut_span(t, &lo, &hi);                    /* a note a degree up / down */
        d = (rng() & 1u) ? 1 : -1;
        if (!mut_fits(s, n = scale_step(t, s->note[k], d), lo, hi) && !mut_fits(s, n = scale_step(t, s->note[k], -d), lo, hi))
            return 0;
        mut_keep(t, i);
        s->note[k] = (uint8_t)n;
        return 1;
    }
    if (c < 14u) {                                /* a ratchet (x2) added, or one taken away */
        int add = (rng() & 1u) != 0;
        if (!mut_pick(t, add ? ms_norat : ms_rat, &i, &k) && !mut_pick(t, (add = !add) ? ms_norat : ms_rat, &i, &k))
            return 0;
        s = &t->step[i];
        sh = 2u * k;
        mut_keep(t, i);
        s->rat = (uint8_t)((s->rat & ~(3u << sh)) | (uint32_t)add << sh);
        return 1;
    }
    if (c < 15u) {                                /* a note on an empty step, near the notes before / after */
        const step_t *a = 0, *b = 0;
        uint32_t j;
        if (!mut_pick(t, ms_empty, &i, &k))
            return 0;
        for (j = 1; j < len && !a; j++)
            if (ms_note(t, (i + len - j) % len, 0))
                a = &t->step[(i + len - j) % len];
        for (j = 1; j < len && !b; j++)
            if (ms_note(t, (i + j) % len, 0))
                b = &t->step[(i + j) % len];
        if (!a || !b)
            return 0;
        if (rng() & 1u)
            a = b;
        n = a->note[rng() % (a->n < 4u ? a->n : 4u)];
        d = (int32_t)(rng() % 3u) - 1;
        mut_span(t, &lo, &hi);
        if ((n = scale_step(t, d ? n : n + 1, d ? d : -1)) < lo || n > hi)   /* (d = 0: n, into the scale) */
            return 0;
        s = &t->step[i];
        mut_keep(t, i);
        memset(s, 0, sizeof *s);
        s->note[0] = (uint8_t)n;
        s->n = 1;
        s->time = ST_NOTE;
        s->vel = a->vel;
        s->lvl = (uint8_t)((rng() & 1u) ? LV_SOFT : LV_NORM);
        return 1;
    }
    for (i = 0, n = 0; i < len; i++)              /* a single note taken away (not one of the last two) */
        n += ms_note(t, i, 0);
    if (n < 3 || !mut_pick(t, ms_single, &i, &k))
        return 0;
    mut_keep(t, i);
    memset(&t->step[i], 0, sizeof t->step[i]);
    t->step[i].time = ST_REST;
    return 1;
}

/* the drum track: the places */
static int md_hit(const track_t *t, uint32_t i, uint32_t l) { return dstep_has(&t->dstep[i], l); }
static int md_ghost_add(const track_t *t, uint32_t i, uint32_t l)   /* off the beat, a snare / hat the pattern uses */
{
    return (((MUT_GHOSTS & mut_lanes) >> l) & 1u) && i % 4u && !md_hit(t, i, l);
}
static int md_ghost(const track_t *t, uint32_t i, uint32_t l)
{
    return ((MUT_GHOSTS >> l) & 1u) && md_hit(t, i, l) && dstep_lvl(&t->dstep[i], l) == LV_GHOST;
}
static int md_move(const track_t *t, uint32_t i, uint32_t l)        /* off the beat, to a step without it */
{
    uint32_t j = (i + mut_off) % trk_len(t);
    return i % 4u && md_hit(t, i, l) && !md_hit(t, j, l) && !mut_kept(j);
}
static int md_level(const track_t *t, uint32_t i, uint32_t l) { return !((MUT_KICKS >> l) & 1u) && md_hit(t, i, l); }
static int md_rat(const track_t *t, uint32_t i, uint32_t l) { return md_hit(t, i, l) && dstep_rat(&t->dstep[i], l); }
static int md_hat(const track_t *t, uint32_t i, uint32_t l)
{
    return ((MUT_HATS >> l) & 1u) && md_hit(t, i, l) && !dstep_rat(&t->dstep[i], l);
}
static int mut_drum(track_t *t)                   /* one change of the drum track: 1, 0 = none to make */
{
    uint32_t i, l, len = trk_len(t), c = rng() % 16u;
    dstep_t *s;
    if (c < 4u) {                                 /* a ghost hit */
        if (!mut_pick(t, md_ghost_add, &i, &l))
            return 0;
        mut_keep(t, i);
        dstep_set(&t->dstep[i], l, LV_GHOST, 0);
        return 1;
    }
    if (c < 8u) {                                 /* a ghost hit taken away */
        if (!mut_pick(t, md_ghost, &i, &l))
            return 0;
        mut_keep(t, i);
        dstep_clr(&t->dstep[i], l);
        return 1;
    }
    if (c < 11u) {                                /* a hit a step later / earlier */
        uint32_t j;
        mut_off = (rng() & 1u) ? 1u : len - 1u;
        if (!mut_pick(t, md_move, &i, &l))
            return 0;
        j = (i + mut_off) % len;
        s = &t->dstep[i];
        mut_keep(t, i);
        mut_keep(t, j);
        dstep_set(&t->dstep[j], l, dstep_lvl(s, l), dstep_rat(s, l));
        dstep_clr(s, l);
        return 1;
    }
    if (c < 14u) {                                /* a level (not the kicks) */
        if (!mut_pick(t, md_level, &i, &l))
            return 0;
        s = &t->dstep[i];
        mut_keep(t, i);
        dstep_set(s, l, mut_level(dstep_lvl(s, l)), dstep_rat(s, l));
        return 1;
    }
    {                                             /* a ratchet (x2) on a hat, or one taken away */
        int add = (rng() & 1u) != 0;
        if (!mut_pick(t, add ? md_hat : md_rat, &i, &l) && !mut_pick(t, (add = !add) ? md_hat : md_rat, &i, &l))
            return 0;
        s = &t->dstep[i];
        mut_keep(t, i);
        dstep_set(s, l, dstep_lvl(s, l), (uint32_t)add);
        return 1;
    }
}

/* a pass on track t (one or two changes): the passes there are to turn back, 0 = nothing to change */
static uint32_t mutate(track_t *t)
{
    uint32_t p, i, tries, ops = 1u + ((rng() & 3u) == 0u);
    if (!mutate_depth(t))
        mut.n = 0;
    if (mut.n == MUT_DEPTH) {                     /* full: the oldest goes */
        for (p = 1; p < MUT_DEPTH; p++) {
            mut.k[p - 1u] = mut.k[p];
            memcpy(mut.idx[p - 1u], mut.idx[p], sizeof mut.idx[p]);
            memcpy(mut.was[p - 1u], mut.was[p], sizeof mut.was[p]);
        }
        mut.n--;
    }
    mut.trk = (uint8_t)trk_index(t);
    p = mut.n;
    mut.k[p] = 0;
    mut_lanes = 0;
    if (is_drum(t))
        for (i = 0; i < trk_len(t); i++)
            mut_lanes |= dstep_mask(&t->dstep[i]);
    for (tries = 0; ops && tries < 16u && mut.k[p] + 2u <= MUT_STEPS; tries++)
        if (is_drum(t) ? mut_drum(t) : mut_synth(t))
            ops--;
    if (!mut.k[p])
        return 0;
    mut.sum = pattern_sum(t);
    return ++mut.n;
}
static int mutate_back(track_t *t)                /* the last pass undone: 1, 0 = none (or the pattern changed since) */
{
    uint32_t p, j;
    if (!mutate_depth(t)) {
        mut.n = 0;
        return 0;
    }
    p = --mut.n;
    for (j = 0; j < mut.k[p]; j++)
        t->step[mut.idx[p][j]] = mut.was[p][j];
    mut.sum = pattern_sum(t);
    return 1;
}

/* ----------------------------------------------------------- turing --- */
/* TURN (SEQ > PATTERN 2, 0..100 %): while the transport plays, each step is rewritten with that chance
 * as it comes round, before it plays, and stays so: TURN back to 0 keeps what you hear (after the
 * Music Thing Turing Machine and Mutable Instruments Marbles' deja vu). Synth parts: a step with notes
 * gets another note of the scale in the register (the span of the pattern's notes when TURN went up, an
 * octave at least): a random bit goes into the track's 16-bit shift register, its low byte is the place
 * in the register (the Turing Machine's 8-bit DAC). A chord keeps its size: CHORD on, the chord of the
 * new note, else its shape moved there. Empty steps, ties and rests stay; levels, ratchets, conditions
 * and locks stay with the step. The drum track: a sound the pattern used (not a kick on a beat) is drawn
 * again on the step, a hit with the share of the steps it had: it comes, goes, or (a hit again) moves to
 * the next sound of its kind the pattern used. Not while recording the track; O(1) a step (audio ISR).
 * Undo: TURN going up from 0 keeps the pattern before it as the undo step (EDIT + OCT-); down and up
 * again on the pattern it left keeps that one; after another edit (or a recording, a load) up takes
 * the pattern then. The rewrites are no undo steps. */
static uint32_t tu_sess, tu_sum;               /* the undo session of the last TURN snapshot, the pattern left then */
static uint8_t tu_cnt[DRUM_LANES];             /* the drum track: the hits of each sound in it then */
static uint32_t tu_lanes;                      /* .. the sounds it used */
static const uint8_t TU_KIN[DRUM_LANES] = {1, 0, 3, 7, 5, 6, 13, 8, 2, 10, 14, 12, 11, 4, 15, 9};   /* next of its kind */

/* the UI, each frame: TURN up on a track (not recording it): the undo step (once) and the register from
 * it; down (or recording): off */
static void turing_arm(void)
{
    uint32_t i, k, j, len;
    for (i = 0; i < NTRK; i++) {
        track_t *t = &trk[i];
        int32_t a = 127, b = 0;
        if (!t->p[P_TURN] || ((song.rec >> i) & 1u)) {
            if (t->tu_arm && undo.trk == i)
                tu_sum = pattern_sum(t);              /* (as it was left: up again on it keeps the undo step) */
            t->tu_arm = 0;
            continue;
        }
        if (t->tu_arm)
            continue;
        fm1_irq_off();
        if (!undo.valid || undo.undone || undo.trk != i || undo.sess != tu_sess || pattern_sum(t) != tu_sum)
            undo_mark(t, tu_sess = (undo_sess += 4u) | 3u);
        len = (uint32_t)clamp(undo.len, 1, NSTEP);
        if (is_drum(t)) {
            tu_lanes = 0;
            memset(tu_cnt, 0, sizeof tu_cnt);
            for (k = 0; k < len; k++) {
                uint32_t m = dstep_mask((const dstep_t *)&undo.st[k]);
                tu_lanes |= m;
                for (j = 0; m; j++, m >>= 1)
                    tu_cnt[j] = (uint8_t)(tu_cnt[j] + (m & 1u));
            }
        } else {
            for (k = 0; k < len; k++)
                for (j = 0; undo.st[k].time == ST_NOTE && j < undo.st[k].n && j < 4u; j++) {
                    a = undo.st[k].note[j] < a ? undo.st[k].note[j] : a;
                    b = undo.st[k].note[j] > b ? undo.st[k].note[j] : b;
                }
            if (b - a < 12) {                         /* an octave at least, around them */
                a -= (12 - (b - a)) / 2;
                b = a + 12;
            }
            a = clamp(a, 0, 115);
            t->tu_lo = (uint8_t)a;
            t->tu_hi = (uint8_t)clamp(b, a + 12, 127);
        }
        t->tu_reg = (uint16_t)rng();
        t->tu_arm = 1;
        fm1_irq_on();
    }
}

/* step idx of t comes round (audio ISR, TURN up): rewritten with its chance (out of line: the ISR stays as it was) */
static __attribute__((noinline)) void turing_step(track_t *t, uint32_t idx)
{
    uint32_t k, n;
    if (!t->tu_arm || ((song.rec >> trk_index(t)) & 1u) || rng() % 100u >= (uint32_t)t->p[P_TURN])
        return;
    if (is_drum(t)) {
        dstep_t *s = &t->dstep[idx];
        uint32_t m = tu_lanes & ~(idx % 4u ? 0u : MUT_KICKS), l = 0, j;
        for (n = 0, k = m; k; k &= k - 1u)
            n++;
        if (!n)
            return;
        n = rng() % n;
        while (!((m >> l) & 1u) || n--)            /* the n-th of them */
            l++;
        if (rng() % trk_len(t) >= tu_cnt[l]) {
            dstep_clr(s, l);                        /* a rest */
        } else if (!dstep_has(s, l)) {
            dstep_set(s, l, idx % 4u ? LV_SOFT : LV_NORM, 0);
        } else {                                    /* a hit again: to the next of its kind */
            for (j = TU_KIN[l]; j != l && (!((m >> j) & 1u) || dstep_has(s, j)); j = TU_KIN[j])
                ;
            if (j != l) {
                dstep_set(s, j, dstep_lvl(s, l), dstep_rat(s, l));
                dstep_clr(s, l);
            }
        }
        return;
    }
    {
        step_t *s = &t->step[idx];
        int32_t lo = t->tu_lo, hi = t->tu_hi, r, x;
        uint8_t c[4];
        if (s->time != ST_NOTE || !s->n)
            return;
        t->tu_reg = (uint16_t)(t->tu_reg << 1 | (rng() & 1u));
        r = scale_step(t, lo + (int32_t)((t->tu_reg & 255u) * (uint32_t)(hi - lo + 1) >> 8) + 1, -1);   /* into the scale */
        if (r < lo)
            r = scale_step(t, r, 1);
        if (r == s->note[0] && (r = scale_step(t, r, 1)) > hi)   /* never the note it had */
            r = scale_step(t, scale_step(t, r, -1), -1);
        n = s->n < 4u ? s->n : 4u;
        if (n > 1u && t->p[P_CHORD] && chord_notes(t, (uint32_t)r, c) >= n) {
            for (k = 0; k < n; k++)
                s->note[k] = c[k];
            return;
        }
        c[0] = (uint8_t)r;
        for (k = 1; k < n; k++) {                   /* the chord's shape, on the new note, in the scale */
            uint32_t j;
            x = scale_step(t, clamp(s->note[k] + r - s->note[0], 0, 126) + 1, -1);
            for (j = 0; j < k; j++)
                if (c[j] == x) {
                    x = scale_step(t, x, 1);
                    j = (uint32_t)-1;
                }
            c[k] = (uint8_t)clamp(x, 0, 127);
        }
        for (k = 0; k < n; k++)
            s->note[k] = c[k];
    }
}

#include "dice.c"                       /* DICE: a new pattern in a style (EDIT + PRESETS) */
#include "grids.c"                       /* GRIDS: the drum map of Grids (the drum screen's MAP page) */

/* ------------------------------------------------------- recording --- */
/* key to ear, in samples: the key's debounce (~3 ms) and the audio out buffer (HALF_FRAMES to
 * 2 x HALF_FRAMES, ~9 ms on average). A note played in time with what the player hears reaches
 * the sequencer this much later than the sound it was played to: recording takes it back. */
#define REC_LAT 512u

/* the step a note played now goes into (as heard: REC_LAT earlier): the one playing, or the next one
 * when it is past the middle of the playing one; *later: it has not played yet (it must not sound twice) */
static uint32_t rec_target(const track_t *t, uint32_t *later)
{
    uint32_t into, slen, abs = trk_grid(t, &into, &slen), half = slen / 2u, lat = REC_LAT * (uint32_t)song.g[G_BPM];
    if (into > half + (lat < half ? lat : half))
        abs++;
    *later = abs != t->seq_abs;
    return abs;
}

/* note into synth step idx (overdub: a step that holds notes gets this one added, a chord of up to 4;
 * when full, the last note is replaced; MONO / LEGATO / UNISON parts keep one note per step) */
static void step_add(track_t *t, uint32_t idx, uint32_t note, uint32_t vel, uint32_t lvl, uint32_t rat)
{
    step_t *s = &t->step[idx];
    uint32_t k;
    if (s->time != ST_NOTE || !s->n)
        t->cond[idx] = CN_ALWAYS;                   /* a new step: no condition */
    if (s->time != ST_NOTE || !s->n || t->p[P_VOICE] != V_POLY) {
        s->n = 0;                                   /* a fresh step */
        s->flags = 0;
        s->vel = 0;
        s->lvl = 0;
        s->rat = 0;
    }
    for (k = 0; k < s->n && s->note[k] != note; k++)
        ;
    if (k == s->n) {
        if (s->n < 4u)
            s->n++;
        k = s->n - 1u;
        s->note[k] = (uint8_t)note;
    }
    s->lvl = (uint8_t)((s->lvl & ~(3u << (2u * k))) | (lvl & 3u) << (2u * k));
    s->rat = (uint8_t)((s->rat & ~(3u << (2u * k))) | (rat & 3u) << (2u * k));
    s->time = ST_NOTE;
    if (vel > s->vel)
        s->vel = (uint8_t)vel;
}

/* live recording into a synth part: the nearest step (rec_target). Held on: each further step the
 * sequencer enters while the note is held becomes a TIE (rec_hold), up to the pattern length and
 * never over a step with notes (overdub keeps them); a release before the middle of the last one
 * puts that step back (rec_release), so a short note stays one step. A note recorded into another
 * step ends the hold before (the step model ties the notes of one step only). */
static void rec_note(track_t *t, uint32_t note, uint32_t vel, uint32_t rat, int hold)
{
    uint32_t len = trk_len(t), later, abs = rec_target(t, &later), idx = abs % len, k;
    undo_mark(t, UNDO_REC(t));
    step_add(t, idx, note, vel, vel_lvl(vel), rat);
    t->seq_active = 1;
    if (later) {                                    /* it sounds now: the step must not trigger it again */
        if (t->rskip_abs != abs)
            t->rskip_n = 0;
        t->rskip_abs = abs;
        if (t->rskip_n < 4u)
            t->rskip[t->rskip_n++] = (uint8_t)note;
    }
    if (!hold)
        return;
    if (!t->rh_n || t->rh_start != idx) {           /* a new hold (one in another step ends) */
        t->rh_n = 0;
        t->rh_start = (uint8_t)idx;
        t->rh_ties = 0;
    }
    for (k = 0; k < t->rh_n && t->rh_note[k] != note; k++)
        ;
    if (k == t->rh_n && t->rh_n < 4u)
        t->rh_note[t->rh_n++] = (uint8_t)note;      /* a chord: held until its last key is up */
}

/* live recording into the drum track: lane, level, ratchet */
static void rec_hit(track_t *t, uint32_t lane, uint32_t lvl, uint32_t rat)
{
    uint32_t later, abs = rec_target(t, &later), idx = abs % trk_len(t);
    undo_mark(t, UNDO_REC(t));
    if (!dstep_mask(&t->dstep[idx]))
        t->cond[idx] = CN_ALWAYS;                   /* a new step: no condition */
    dstep_set(&t->dstep[idx], lane, lvl, rat);
    t->seq_active = 1;
    if (later) {
        if (t->rskip_abs != abs)
            t->rskip_lanes = 0;
        t->rskip_abs = abs;
        t->rskip_lanes |= (uint16_t)(1u << lane);
    }
}

/* the sequencer enters step idx (before playing it): a recorded note still held ties into it */
static void rec_hold(track_t *t, uint32_t idx, uint32_t len, uint32_t abs)
{
    step_t *s;
    if (!t->rh_n)
        return;
    if (!((song.rec >> trk_index(t)) & 1u) || t->rh_ties + 1u >= len) {
        t->rh_n = 0;                                /* disarmed, or the whole pattern is this note */
        return;
    }
    if (idx == t->rh_start)
        return;                                     /* (recorded ahead into the step now starting) */
    s = &t->step[idx];
    if (s->time == ST_NOTE && s->n) {
        t->rh_n = 0;                                /* a step with notes: the hold ends before it */
        return;
    }
    t->rh_bak = *s;
    t->rh_last = (uint8_t)idx;
    t->rh_last_abs = abs;
    t->rh_ties++;
    memset(s, 0, sizeof *s);
    s->time = ST_TIE;
}

/* a key of a recorded note is up: the hold ends with the last one */
static void rec_release(track_t *t, uint32_t note)
{
    uint32_t i, k = 0, into, slen, half, lat;
    for (i = 0; i < t->rh_n; i++)
        if (t->rh_note[i] != note)
            t->rh_note[k++] = t->rh_note[i];
    if (k == t->rh_n || (t->rh_n = (uint8_t)k))
        return;                                     /* not one of them, or others still held */
    if (!t->rh_ties || trk_grid(t, &into, &slen) != t->rh_last_abs)
        return;
    half = slen / 2u;
    lat = REC_LAT * (uint32_t)song.g[G_BPM];
    if (into <= half + (lat < half ? lat : half) && t->step[t->rh_last].time == ST_TIE)
        t->step[t->rh_last] = t->rh_bak;            /* released early in it: not held into this step */
}

/* LIVE recording, no click needed (REC: ui_input.c rec_toggle).
 *  playing: REC records the selected track at once (song.rec), quantised, overdub.
 *  stopped: REC arms (rec_wait). The first note played on the selected track:
 *   - a project with notes: starts the transport, that note is step 1, recording on.
 *     PLAY while armed starts the transport and the recording together.
 *   - an empty project: a FREE TAKE (ft_on). Play freely, as long as you like: no tempo, no
 *     grid. REC on the next downbeat closes the loop: its length sets the tempo (1, 2 or 4
 *     bars, the nearest the current tempo; within 3 % of it the tempo is kept), the notes are
 *     quantised to 1/16 into it with their lengths, and the loop plays on. PLAY drops the take.
 *  The REC screen (ui_studio.c) sets how (settings of the FM-1, panel.c lights_word):
 *   KNOB 1 MODE (an empty project): FREE (the free take above) or TEMPO (record at the tempo set,
 *          as in a project with notes);
 *   KNOB 3 START (TEMPO, or a project with notes): NOTE (the first note starts the loop, as above)
 *          or COUNT (PLAY clicks one bar, 4 beats, then the loop and the recording start; notes
 *          played meanwhile only sound). */
static volatile uint8_t rec_wait;             /* 1: armed, waits for a note */
static uint8_t rec_tempo;                     /* REC screen MODE: 0 FREE, 1 TEMPO (an empty project) */
static uint8_t rec_count;                     /* REC screen START: 0 NOTE, 1 COUNT (one bar of clicks) */
static volatile uint8_t ci_on;                /* the count-in runs (armed, COUNT, PLAY) */
static volatile uint8_t ci_beat;              /* its beats clicked so far - 1 (the UI shows 4 - ci_beat) */
static uint32_t ci_u;                         /* clock units since it started */
static volatile uint8_t rec_go;               /* recording just started (the UI says so) */
static void seq_start(void);
static void rec_begin(void)
{
    song.rec = (uint8_t)(1u << (song.sel % NTRK));
    rec_wait = 0;
    rec_go = 1;
}

#define FT_MAX 192u
#define FT_BLOCKS (24u * FS / CTL)            /* 24 s: 4 bars at 40 BPM, the longest loop */
#define FT_OPEN 0xFFFFu                       /* the key is still down */
typedef struct { uint16_t t, d; uint8_t note, vel; } ft_ev_t;   /* in blocks from the first note; drums: lane, level */
static ft_ev_t ft_ev[FT_MAX];
static uint32_t ft_n;
static volatile uint8_t ft_on;                /* a free take runs */
static uint8_t ft_trk;
static volatile uint32_t ft_t;                /* blocks since its first note */
static volatile uint32_t ft_btn_mask;         /* the REC button (the UI sets it): closes the take */
static volatile uint32_t ft_drop_mask;        /* the PLAY button: drops it */
static uint32_t ft_btn_prev;
static volatile uint8_t ft_bars;              /* the loop just closed: bars (the UI says so), 0xFF dropped */
static volatile uint32_t ft_close_ms;         /* when (the UI drops the press that closed it) */
static volatile uint8_t ft_closed;            /* (ft_close_ms is set) */

static int track_empty(const track_t *t)
{
    uint32_t k;
    for (k = 0; k < NSTEP; k++)
        if (step_sounds(t, k))
            return 0;
    return 1;
}
static int project_empty(void)
{
    uint32_t i;
    for (i = 0; i < NTRK; i++)
        if (!track_empty(&trk[i]))
            return 0;
    return 1;
}
static void steps_clear(track_t *t)           /* an empty pattern (synth: REST steps, drums: no lane) */
{
    uint32_t k;
    memset(t->step, 0, sizeof t->step);
    memset(t->cond, 0, sizeof t->cond);
    if (!is_drum(t))
        for (k = 0; k < NSTEP; k++)
            t->step[k].time = ST_REST;
    plk_clear_track(t);                       /* (and their locks) */
}

/* tempo x 10 of a loop of T blocks holding n bars of 4/4 */
static uint32_t ft_bpm10(uint32_t T, uint32_t n)
{
    uint32_t den = T * CTL;                       /* (n * 2400 * FS < 2^32 up to n = 4) */
    return den ? (n * 2400u * FS + den / 2u) / den : 0u;
}

/* the bars a loop of T blocks holds (1, 2 or 4: the tempo the nearest the current one, in
 * 40..240), 0 none; *bpm its tempo */
static uint32_t ft_fit(uint32_t T, uint32_t *bpm)
{
    static const uint8_t BARS[3] = {1, 2, 4};
    uint32_t i, best = 0, cur = (uint32_t)song.g[G_BPM] * 10u, err = 0xFFFFFFFFu;
    for (i = 0; i < 3u; i++) {
        uint32_t b = ft_bpm10(T, BARS[i]), e;
        if (b < 400u || b > 2400u)
            continue;
        e = b > cur ? b * 1000u / cur : cur * 1000u / b;   /* the ratio, x 1000 */
        if (e < err) {
            err = e;
            best = BARS[i];
            *bpm = (b + 5u) / 10u;
            if (e <= 1030u)
                *bpm = cur / 10u;                           /* played to the tempo set: keep it */
        }
    }
    return best;
}

static void ft_start(track_t *t)
{
    ft_on = 1;
    ft_trk = (uint8_t)trk_index(t);
    ft_t = 0;
    ft_n = 0;
    rec_wait = 0;
}

static void ft_note_on(uint32_t note, uint32_t vel)
{
    if (ft_n < FT_MAX && ft_t < FT_BLOCKS) {
        ft_ev[ft_n].t = (uint16_t)ft_t;
        ft_ev[ft_n].d = FT_OPEN;
        ft_ev[ft_n].note = (uint8_t)note;
        ft_ev[ft_n].vel = (uint8_t)vel;
        ft_n++;
    }
}

static void ft_note_off(uint32_t note)
{
    uint32_t i = ft_n;
    while (i--)
        if (ft_ev[i].note == note && ft_ev[i].d == FT_OPEN) {
            ft_ev[i].d = (uint16_t)(ft_t - ft_ev[i].t);
            return;
        }
}

/* REC on the downbeat: the loop is the time from the first note to now */
static void ft_close(void)
{
    track_t *t = &trk[ft_trk % NTRK];
    uint32_t T = ft_t, bars, bpm = 0, len, i, k;
    ft_on = 0;
    ft_close_ms = fm1_ms;
    ft_closed = 1;
    bars = ft_n && T >= FS / CTL / 2u ? ft_fit(T, &bpm) : 0u;
    if (!bars) {
        ft_bars = 0xFF;                               /* nothing played, or no tempo fits */
        return;
    }
    len = 16u * bars;
    undo_mark(t, (undo_sess += 4u) | 3u);             /* (undo: back to the empty project) */
    steps_clear(t);                                   /* (no tie left over from an old pattern) */
    for (i = 0; i < NTRK; i++) {                      /* the project is empty: one loop length */
        trk[i].p[P_SLEN] = (int16_t)len;
        trk[i].p[P_SDIV] = 2;                         /* 1/16 */
    }
    for (i = 0; i < ft_n; i++) {                      /* the notes, to the nearest step */
        uint32_t idx = (ft_ev[i].t * len * 2u + T) / (2u * T) % len;
        if (is_drum(t))
            dstep_set(&t->dstep[idx], ft_ev[i].note & 15u, ft_ev[i].vel & 3u, 0);
        else
            step_add(t, idx, ft_ev[i].note, ft_ev[i].vel, vel_lvl(ft_ev[i].vel), 0);
    }
    if (!is_drum(t))
        for (i = 0; i < ft_n; i++) {                  /* their lengths: TIE steps, up to the next note */
            uint32_t d = ft_ev[i].d == FT_OPEN || ft_ev[i].t + ft_ev[i].d > T ? T - ft_ev[i].t : ft_ev[i].d;
            uint32_t idx = (ft_ev[i].t * len * 2u + T) / (2u * T) % len;
            uint32_t steps = (d * len * 2u + T) / (2u * T);
            for (k = 1; k < steps && k < len; k++) {
                step_t *s = &t->step[(idx + k) % len];
                if (s->time == ST_NOTE && s->n)
                    break;
                memset(s, 0, sizeof *s);
                s->time = ST_TIE;
            }
        }
    t->seq_active = 1;
    song.g[G_BPM] = (int16_t)bpm;
    ft_bars = (uint8_t)bars;
#if FELUCCA_ARRANGER
    arrangement_enabled = 0;
#endif
    seq_start();                                      /* this is the downbeat: the loop plays */
}

/* the UI: a REC / PLAY press is this one's (it runs the take, or just closed it): not the UI's */
static int ft_owns_press(void)
{
    return ft_on || (ft_closed && (uint32_t)(fm1_ms - ft_close_ms) < 300u);
}

/* a free take, once per block: REC closes it, PLAY drops it (their press, timed here, not by the UI) */
static void ft_block(void)
{
    uint32_t b = fm1_in.buttons & (ft_btn_mask | ft_drop_mask), press = b & ~ft_btn_prev;
    ft_btn_prev = b;
    if (!ft_on)
        return;
    ft_t++;
    if (press & ft_drop_mask) {
        ft_on = 0;                                    /* PLAY: dropped, nothing changes */
        ft_bars = 0xFF;
        ft_close_ms = fm1_ms;
        ft_closed = 1;
    } else if ((press & ft_btn_mask) || ft_t >= FT_BLOCKS) {
        ft_close();
    }
}

/* ------------------------------------------------------------ erase --- */
/* EDIT + key: that note / sound goes from the selected pattern. Playing: from the step playing and
 * every step the sequencer enters while the key is held (MPC style); stopped: from every step. */
static uint8_t er_trk;
static uint16_t er_lanes;                     /* drums: lanes held */
static uint32_t er_notes[4];                  /* synth: notes held (bit per MIDI note) */
static volatile uint8_t er_flash;             /* something was erased (the UI flashes) */

static int er_has(uint32_t note) { return (er_notes[(note >> 5) & 3u] >> (note & 31u)) & 1u; }
static void erase_step(track_t *t, uint32_t idx)
{
    if (is_drum(t)) {
        dstep_t *s = &t->dstep[idx];
        uint32_t l, m = dstep_mask(s) & er_lanes, any = m;
        for (l = 0; m; l++, m >>= 1)
            if (m & 1u) {
                dstep_clr(s, l);
                er_flash = 1;
            }
        if (!dstep_mask(s))
            t->cond[idx] = CN_ALWAYS;
        if (any && !dstep_mask(s))
            plk_clear_step(t, idx);                 /* (an empty step: its locks go with it) */
    } else {
        step_t *s = &t->step[idx];
        uint32_t i, k = 0, lv = 0, rt = 0;
        if (s->time != ST_NOTE)
            return;
        for (i = 0; i < s->n; i++)
            if (!er_has(s->note[i])) {
                lv |= ((s->lvl >> (2u * i)) & 3u) << (2u * k);
                rt |= ((s->rat >> (2u * i)) & 3u) << (2u * k);
                s->note[k++] = s->note[i];
            }
        if (k == s->n)
            return;
        er_flash = 1;
        s->n = (uint8_t)k;
        s->lvl = (uint8_t)lv;
        s->rat = (uint8_t)rt;
        if (!k) {
            s->time = ST_REST;
            s->flags = 0;
            s->vel = 0;
            t->cond[idx] = CN_ALWAYS;
            plk_clear_step(t, idx);
        }
    }
}
static void erase_now(track_t *t)             /* a key just went down */
{
    uint32_t i, len = trk_len(t);
    undo_mark(t, undo_erase_sess);
    if (song.playing && t->seq_abs != SEQ_NONE) {
        erase_step(t, t->seq_idx % len);
    } else {
        for (i = 0; i < len; i++)
            erase_step(t, i);
    }
}
static int erasing(const track_t *t) { return trk_index(t) == er_trk && (er_lanes || er_notes[0] || er_notes[1] || er_notes[2] || er_notes[3]); }

/* ------------------------------------------------------------- arp --- */
enum { AS_NOTE, AS_BAR, AS_FREE };              /* arp SYNC (params.c N_ASYNC): when the order starts over */

static void arp_add(track_t *t, uint32_t note)
{
    uint32_t i;
    if (t->p[P_AHOLD] && t->arp_phys == 0u)
        t->nheld = 0;                               /* new chord replaces the latched one */
    t->arp_phys++;                                  /* every key-down: arp_remove counts every key-up */
    for (i = 0; i < t->nheld; i++)
        if (t->held[i] == note)
            return;                                 /* repeated note-on: not a new note */
    if (t->nheld < 16u)
        t->held[t->nheld++] = (uint8_t)note;
    if (t->nheld == 1u) {
        t->arp_new = 1;                             /* the first note: now (or on the grid just ahead) */
        if (t->p[P_ASYNC] != AS_FREE)
            t->arp_idx = 0;                         /* the order from its start (FREE: it goes on) */
    }
}

static void arp_remove(track_t *t, uint32_t note)
{
    uint32_t i, k = 0;
    if (t->arp_phys)
        t->arp_phys--;
    if (t->p[P_AHOLD])
        return;
    for (i = 0; i < t->nheld; i++)
        if (t->held[i] != note)
            t->held[k++] = t->held[i];
    t->nheld = (uint8_t)k;
}

/* arp MODE (params.c N_AMODE) */
enum { A_OFF, A_UP, A_DN, A_UPDN, A_RND, A_ORD, A_UPDN2, A_CONV, A_DIVG, A_THMB, A_PNKY, A_DRNK, A_SHUF, A_OCTI,
       A_CHRD };
/* arp RAT (params.c N_ARAT): X1..X4 a ratchet, UP2..DN4 STRUM (arp_strum) */
enum { AR_X1, AR_X2, AR_X3, AR_X4, AR_UP2, AR_UP3, AR_UP4, AR_DN2, AR_DN3, AR_DN4 };

/* the held notes (sorted, or as played: ORD), one octave after the other over OCT octaves; OCTI: each
 * note in its octaves before the next (at least two) */
static uint32_t arp_list(const track_t *t, uint32_t *list)
{
    uint32_t cnt = t->nheld, len = 0, i, j, o, mode = (uint32_t)t->p[P_AMODE], oct = (uint32_t)t->p[P_AOCT];
    for (i = 0; i < cnt; i++)
        list[i] = t->held[i];
    if (!t->p[P_AORDER] && mode != A_ORD)
        for (i = 1; i < cnt; i++)
            for (j = i; j > 0 && list[j - 1] > list[j]; j--) {
                uint32_t x = list[j];
                list[j] = list[j - 1];
                list[j - 1] = x;
            }
    if (mode == A_OCTI) {
        uint32_t base[16];
        oct = oct < 2u ? 2u : oct;
        for (i = 0; i < cnt; i++)
            base[i] = list[i];
        for (i = 0; i < cnt; i++)
            for (o = 0; o < oct && len < 64u; o++)
                list[len++] = (uint32_t)clamp((int32_t)base[i] + 12 * (int32_t)o, 0, 127);
        return len;
    }
    for (o = 0; o < oct; o++)
        for (i = 0; i < cnt && len < 64u; i++)
            list[len++] = (uint32_t)clamp((int32_t)list[i] + 12 * (int32_t)o, 0, 127);
    return len;
}

/* a random number for the arp: fresh (DEJA 0), else the next one drawn from the seed of the step */
static uint32_t arp_rnd(track_t *t)
{
    uint32_t h;
    if (!t->p[P_ADEJA])
        return rng();
    h = (t->arp_ds + 1u) * 0x9E3779B1u + ++t->arp_dk * 0x85EBCA77u;
    h = (h ^ (h >> 15)) * 0x2C1B3C6Du;
    h = (h ^ (h >> 12)) * 0x297A2D39u;
    return h ^ (h >> 15);
}

/* SHUF: a random place not yet played this round; a new round never starts with the last one */
static uint32_t arp_shuffle(track_t *t, uint32_t len, uint32_t first)
{
    uint32_t m[2], n = 0, i, r, avoid = 64u;
    m[0] = len >= 32u ? 0xFFFFFFFFu : (1u << len) - 1u;
    m[1] = len >= 64u ? 0xFFFFFFFFu : len > 32u ? (1u << (len - 32u)) - 1u : 0u;
    if (first)
        t->arp_bag[0] = t->arp_bag[1] = 0;
    t->arp_bag[0] &= m[0];
    t->arp_bag[1] &= m[1];
    if (!t->arp_bag[0] && !t->arp_bag[1]) {
        t->arp_bag[0] = m[0];
        t->arp_bag[1] = m[1];
        if (!first && len > 1u)
            avoid = t->arp_walk;
    }
    for (i = 0; i < len; i++)
        n += i != avoid && ((t->arp_bag[i >> 5] >> (i & 31u)) & 1u);
    r = arp_rnd(t) % n;
    for (i = 0; i < len; i++)
        if (i != avoid && ((t->arp_bag[i >> 5] >> (i & 31u)) & 1u) && !r--)
            break;
    t->arp_bag[i >> 5] &= ~(1u << (i & 31u));
    return i;
}

/* SHIFT: the degrees the note at place idx of the order moves. A cycle is per places of the order: the
 * list once (UPDN, UPDN+, THMB, PNKY: their period; RND, DRNK, SHUF: as many picks), CHRD: one step.
 * Each cycle moves SHIFT degrees more, after CYC cycles the notes are as held again; place 0 (the order
 * started over: a new chord, SYNC BAR) is cycle 0 */
static int32_t arp_cycle(track_t *t, uint32_t idx, uint32_t per)
{
    if (!idx) {
        t->arp_c0 = 0;
        t->arp_cyc = 0;
    } else if (idx - t->arp_c0 >= per) {
        t->arp_c0 = idx;
        t->arp_cyc = (uint8_t)(t->arp_cyc + 1u < (uint32_t)t->p[P_ACYC] ? t->arp_cyc + 1u : 0u);
    }
    return t->p[P_ASHIFT] * (int32_t)t->arp_cyc;
}

/* note n moved d degrees of the track's scale (CHR: semitones; a note off the scale steps onto it),
 * folded by octaves into 0..127 */
static uint32_t arp_deg(const track_t *t, uint32_t n, int32_t d)
{
    uint32_t mask = scale_mask(t), deg = 0, i;
    int32_t s = d < 0 ? -1 : 1, k = d * s - 1, m;
    if (!d)
        return n;
    for (i = 0; i < 12u; i++)
        deg += (mask >> i) & 1u;
    m = scale_step(t, (int32_t)n, s);
    for (i = (uint32_t)k % deg; i > 0; i--)
        m = scale_step(t, m, s);
    m += s * 12 * (k / (int32_t)deg);            /* (the octaves last: scale_step stays near 0..127) */
    while (m > 127)
        m -= 12;
    while (m < 0)
        m += 12;
    return (uint32_t)m;
}

/* the next note of the arp, by MODE; ROT: the order from a later place; SHIFT: moved by its cycle */
static uint32_t arp_next(track_t *t)
{
    uint32_t list[64], len = arp_list(t, list), first = !t->arp_idx, i, j, k, cyc = len;
    i = t->arp_idx++ + (uint32_t)t->p[P_AROT];
    switch (t->p[P_AMODE]) {
    case A_DN:
        j = len - 1u - i % len;
        break;
    case A_UPDN:                                 /* the ends once: C E G E */
        cyc = len > 1u ? 2u * len - 2u : 1u;
        k = i % cyc;
        j = k < len ? k : cyc - k;
        break;
    case A_UPDN2:                                /* the ends twice: C E G G E C */
        cyc = 2u * len;
        k = i % cyc;
        j = k < len ? k : 2u * len - 1u - k;
        break;
    case A_RND:
        j = arp_rnd(t) % len;
        break;
    case A_CONV:                                 /* outside in: C B E G */
    case A_DIVG:                                 /* inside out: G E B C */
        k = t->p[P_AMODE] == A_CONV ? i % len : len - 1u - i % len;
        j = k & 1u ? len - 1u - k / 2u : k / 2u;
        break;
    case A_THMB:                                 /* the lowest between the others: C E C G C B */
    case A_PNKY:                                 /* the highest between the others: B C B E B G */
        if (len < 2u) {
            j = 0;
            cyc = 1;
            break;
        }
        cyc = 2u * len - 2u;
        k = i % cyc;
        j = t->p[P_AMODE] == A_THMB ? (k & 1u ? 1u + k / 2u : 0u) : (k & 1u ? k / 2u : len - 1u);
        break;
    case A_DRNK:                                 /* a random walk: one place up or down */
        j = t->arp_walk;
        if (first || len < 2u)
            j = i % len;
        else if (j >= len - 1u)
            j = len - 2u;
        else if (!j || (arp_rnd(t) & 1u))
            j++;
        else
            j--;
        break;
    case A_SHUF:                                 /* every place once a round, in a new order each round */
        j = arp_shuffle(t, len, first);
        break;
    default:                                     /* UP, ORD (as played), OCTI */
        j = i % len;
        break;
    }
    t->arp_walk = (uint8_t)j;
    return arp_deg(t, list[j], arp_cycle(t, t->arp_idx - 1u, cyc));
}

/* CHRD: every held note at once, an octave higher each step over OCT octaves (up to 8 notes); SHIFT:
 * each step a cycle */
static uint32_t arp_chord(track_t *t, uint8_t *out)
{
    uint32_t i, n = 0, o = (t->arp_idx + (uint32_t)t->p[P_AROT]) % (uint32_t)t->p[P_AOCT];
    int32_t d = arp_cycle(t, t->arp_idx++, 1u);
    for (i = 0; i < t->nheld && n < 8u; i++)
        out[n++] = (uint8_t)arp_deg(t, (uint32_t)clamp((int32_t)t->held[i] + 12 * (int32_t)o, 0, 127), d);
    return n;
}

/* ACC: the velocity of the arp step at grid place pos; OFF: as the keys play (100) */
static uint32_t arp_vel(track_t *t, uint32_t pos)
{
    uint32_t a = (uint32_t)t->p[P_AACC], hit;
    if (!a)
        return 100;
    if (a < 4u)
        hit = pos % (a + 1u) == 0u;                 /* 1IN2 .. 1IN4 */
    else if (a == 4u)
        hit = (0x49u >> (pos & 7u)) & 1u;           /* 3-3-2: X..X..X. */
    else
        hit = (arp_rnd(t) & 3u) == 0u;
    return lvl_vel(hit ? LV_HARD : LV_SOFT, 100);
}

/* RHYM (params.c N_ARHYM): gates of 16 places, bit 0 the first; OFF: every place */
static const uint16_t ARP_RHYM[16] = {
    0xFFFF, 0x1111, 0x5555, 0x4444,                 /* OFF, QRTR X..., 8TH X., OFFB ..X. */
    0xDDDD, 0xBBBB, 0x9249, 0x4949,                 /* GALOP X.XX, SKIP XX.X, DOT8 X.., TRES X..X..X. */
    0x6D6D, 0x1449, 0x4914, 0x1489,                 /* CINQ X.XX.XX., SON32 X..X..X...X.X..., SON23 ..X.X...X..X..X., RUMBA X..X...X..X.X... */
    0x2449, 0x1451, 0x0C49, 0x4449,                 /* BOSSA X..X..X...X..X.., SHIKO X...X.X...X.X..., SOUK X..X..X...XX...., GAHU X..X..X...X...X. */
};

/* HITS of STEPS (an euclidean rhythm, its first hit on place 0) and RHYM: place pos plays */
static int arp_hit(const track_t *t, uint32_t pos)
{
    uint32_t k = (uint32_t)t->p[P_AHITS], n = (uint32_t)t->p[P_ASTEPS];
    return (k >= n || (pos % n) * k % n < k) && ((ARP_RHYM[(uint32_t)t->p[P_ARHYM] & 15u] >> (pos & 15u)) & 1u);
}

static void arp_sound(track_t *t, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < t->arp_n; i++)
        trk_note_on(t, t->arp_ch[i], t->arp_vel);
    t->arp_snd = 1;
    t->arp_off = len * (uint32_t)t->p[P_AGATE] / 128u;
}

static void arp_release(track_t *t)
{
    uint32_t i;
    if (t->arp_snd)
        for (i = 0; i < t->arp_n; i++)
            trk_note_off(t, t->arp_ch[i]);
    t->arp_snd = 0;
}

/* RAT UP2..DN4 (STRUM), n = 2..4. One note: a run of n hits as RAT Xn, the step's note then the next
 * ones of the list (DN: the ones before), wrapping; the order goes on from the step's note as at X1.
 * CHRD: the notes start one after another, low to high (DN: high to low), the last one at (n - 1) / n
 * of the gate; each sounds to the gate's end */
static void arp_strum(track_t *t, uint32_t rat, uint32_t slen)
{
    uint32_t n = 2u + (rat - AR_UP2) % 3u, dn = rat >= AR_DN2, m = t->arp_n, i, j, list[64], len;
    if (t->p[P_AMODE] == A_CHRD) {
        for (i = 1; i < m; i++)
            for (j = i; j > 0 && (dn ? t->arp_ch[j - 1] < t->arp_ch[j] : t->arp_ch[j - 1] > t->arp_ch[j]); j--) {
                uint8_t x = t->arp_ch[j];
                t->arp_ch[j] = t->arp_ch[j - 1];
                t->arp_ch[j - 1] = x;
            }
        t->arp_str = 2;
        t->arp_sl = m > 1u ? slen * (uint32_t)t->p[P_AGATE] / 128u * (n - 1u) / (n * (m - 1u)) : 0u;
        t->arp_n = 1;
        arp_sound(t, slen);
    } else {
        int32_t d = t->p[P_ASHIFT] * (int32_t)t->arp_cyc;   /* (SHIFT: the run moves with the step's note) */
        len = arp_list(t, list);
        j = t->arp_walk;
        for (i = 1, m = n; i < n; i++)
            t->arp_ch[i] = (uint8_t)arp_deg(t, list[(dn ? j + len - i % len : j + i) % len], d);
        t->arp_str = 1;
        t->arp_sl = slen / n;
        arp_sound(t, t->arp_sl);
    }
    t->arp_m = (uint8_t)m;
    t->arp_rat = (uint8_t)(m - 1u);
    t->arp_sub = t->arp_sl;
}

/* the arp, once per block: on the transport's grid of RATE (with its SWING) while playing, from the
 * first key while stopped. A new chord starts at once unless the grid is just ahead. Each step: HITS
 * of STEPS, RHYM and PROB decide if it plays, ACC its velocity, RAT how many times (or a run, a
 * strum); DEJA: its random choices from the seed of its grid place, which changes by chance. While
 * recording, each step it plays is recorded (what you hear; a run or a strum: its notes as a chord) */
static void arp_tick(track_t *t, uint32_t adv)
{
    uint32_t den = DIV_DEN[(uint32_t)t->p[P_ARATE] % 6u], u = BEAT_U / den, into, slen, abs = 0, fire = 0, pos, rat, r, i;
    uint8_t *seed;
    if (t->arp_snd) {
        if (t->arp_off <= adv)
            arp_release(t);
        else
            t->arp_off -= adv;
    }
    if (!t->p[P_AMODE] || !t->nheld) {
        if (!t->nheld)
            arp_release(t);
        t->arp_new = 0;
        t->arp_rat = 0;
        return;
    }
    if (song.playing) {
        abs = grid_at(den, swing_units(t->p[P_ASWING], u), &into, &slen);
        if (t->arp_new) {
            t->arp_new = 0;
            t->arp_abs = into * 4u >= slen * 3u ? abs : abs - 1u;   /* the last quarter: the grid plays it */
        }
        if (abs + 1u == t->arp_abs)
            abs = t->arp_abs;                       /* ARP SWG turned up inside an odd step: no replay */
        fire = abs != t->arp_abs;
        t->arp_abs = abs;
        pos = abs;
    } else {
        if (t->arp_new) {
            t->arp_new = 0;
            t->arp_pos = u;
            t->arp_step = 0;
        }
        slen = u;
        t->arp_pos += adv;
        if (t->arp_pos >= u) {
            t->arp_pos = t->arp_pos - u < u ? t->arp_pos - u : 0;
            fire = 1;
        }
        pos = t->arp_step;
    }
    if (!fire) {
        if (t->arp_rat && t->arp_n) {               /* the ratchet's next hit */
            if (t->arp_sub > adv) {
                t->arp_sub -= adv;
            } else {
                t->arp_sub = t->arp_sub + t->arp_sl > adv ? t->arp_sub + t->arp_sl - adv : 1u;
                t->arp_rat--;
                if (t->arp_str == 2u) {             /* a strummed chord: its next note, held */
                    if (t->arp_snd)
                        trk_note_on(t, t->arp_ch[t->arp_n++], t->arp_vel);
                    else
                        t->arp_rat = 0;             /* (the gate ended first) */
                } else {
                    arp_release(t);
                    if (t->arp_str)
                        t->arp_ch[0] = t->arp_ch[t->arp_m - 1u - t->arp_rat];   /* a run: its next note */
                    arp_sound(t, t->arp_sl);
                }
            }
        }
        return;
    }
    if (!song.playing)
        t->arp_step++;
    else if (t->p[P_ASYNC] == AS_BAR && !(pos % (4u * den)))
        t->arp_idx = 0;                             /* SYNC BAR: the order from its start on each bar */
    arp_release(t);
    t->arp_n = 0;
    t->arp_rat = 0;
    seed = &t->arp_dv[pos & 15u];
    if (t->p[P_ADEJA] && rng() % 127u >= (uint32_t)t->p[P_ADEJA])
        *seed = (uint8_t)(rng() >> 24);             /* a new seed: (127 - DEJA) / 127 of the steps */
    t->arp_ds = *seed;
    t->arp_dk = (uint8_t)((pos & 15u) << 4);        /* draws from place x 16: equal seeds differ by place */
    if (!arp_hit(t, pos))
        return;
    r = arp_rnd(t);
    if (!t->p[P_ADEJA])
        *seed = (uint8_t)(r >> 24);                 /* DEJA 0: the loop takes fresh seeds */
    if ((r & 127u) > (uint32_t)t->p[P_APROB])
        return;
    if (t->p[P_AMODE] == A_CHRD) {
        t->arp_n = (uint8_t)arp_chord(t, t->arp_ch);
    } else {
        t->arp_ch[0] = (uint8_t)arp_next(t);
        t->arp_n = 1;
    }
    rat = (uint32_t)t->p[P_ARAT];
    t->arp_vel = (uint8_t)arp_vel(t, pos);
    t->arp_str = 0;
    if (rat >= AR_UP2) {
        arp_strum(t, rat, slen);
        rat = 0;                                    /* recorded: the step's notes as a chord, x1 */
    } else {
        t->arp_rat = (uint8_t)rat;
        t->arp_sl = slen / (rat + 1u);
        t->arp_sub = t->arp_sl;
        arp_sound(t, t->arp_sl);
    }
    if (((song.rec >> trk_index(t)) & 1u) && song.playing)
        for (i = 0; i < (t->arp_str ? t->arp_m : t->arp_n) && i < 4u; i++)
            rec_note(t, t->arp_ch[i], t->arp_vel, rat, 0);
}

/* ------------------------------------------------------- note input --- */
/* armed and stopped, a note on the selected track: an empty project starts a free take, else the
 * note is the downbeat (the transport starts, recording on) */
static void arm_start(track_t *t)
{
    if (!rec_wait || t != TSEL || song.playing || ci_on)
        return;
    if (project_empty() && !rec_tempo) {
        ft_start(t);                              /* the first take sets the loop and the tempo */
    } else if (rec_count) {
        return;                                   /* COUNT: PLAY counts in; a note only sounds */
    } else {
#if FELUCCA_ARRANGER
        arrangement_enabled = 0;
#endif
        seq_start();                              /* the note is the downbeat */
        if (song.playing)
            rec_begin();
    }
}

static void drum_input(uint32_t lane, uint32_t lvl, uint32_t rat, int rec);
static void input_on(track_t *t, uint32_t note, uint32_t vel)
{
    if (is_drum(t)) {                             /* (a GM note on the drum track: its lane) */
        drum_input(lane_of_note(note), vel_lvl(vel), 0, 1);
        return;
    }
    last_note = (uint8_t)note;
    arm_start(t);
    if (ft_on && t == &trk[ft_trk % NTRK])
        ft_note_on(note, vel);
    if (t->p[P_AMODE]) {
        arp_add(t, note);                         /* (the arp records the notes it plays) */
        return;
    }
    if (((song.rec >> trk_index(t)) & 1u) && song.playing)
        rec_note(t, note, vel, 0, 1);
    trk_note_on(t, note, vel);
}

static void input_off(track_t *t, uint32_t note)
{
    if (is_drum(t)) {
        if (ft_on && ft_trk == TRK_DRUM)
            ft_note_off(lane_of_note(note));
        return;
    }
    if (ft_on && t == &trk[ft_trk % NTRK])
        ft_note_off(note);
    rec_release(t, note);
    arp_remove(t, note);                            /* both: the note may have started in the */
    trk_note_off(t, note);                          /* other mode (ARP switched while held) */
}

/* a drum hit from a key, MIDI or a roll: lane, level; rat: its ratchet when recorded (rolls); rec: it
 * may be recorded (a roll records one hit a step) */
static void drum_input(uint32_t lane, uint32_t lvl, uint32_t rat, int rec)
{
    track_t *t = TDRUM;
    lane &= 15u;
    pen_lane = (uint8_t)lane;
    arm_start(t);
    if (ft_on && ft_trk == TRK_DRUM)
        ft_note_on(lane, lvl);
    if (rec && ((song.rec >> TRK_DRUM) & 1u) && song.playing)
        rec_hit(t, lane, lvl, rat);
    trk_note_on(t, LANE_NOTE[lane], lvl_vel(lvl, 100));
}

/* -------------------------------------------------------------- roll --- */
/* ARP + key: note repeat. The key plays at G_ROLL (1/8, 1/16, 1/32, 32T, 1/64) on the transport's grid
 * (stopped: from the press) until it or ARP is up. Recorded, a roll faster than the track's steps
 * becomes ratchets (1/32 on 1/16 steps: x2, 32T: x3, 1/64: x4). */
static const uint8_t ROLL_DEN[5] = {2, 4, 8, 12, 16};
#define NROLL 4u
static struct {
    uint8_t on, key, trk, note, lvl;    /* note: the synth note, or the lane */
    uint32_t last;                      /* playing: the roll step last played; stopped: units since */
    uint32_t off;                       /* synth: units to its note-off, 0 = not sounding */
    uint32_t rec_abs;                   /* the step its last recorded hit went into */
} roll[NROLL];
static uint32_t roll_den(void) { return ROLL_DEN[(uint32_t)clamp(song.g[G_ROLL], 0, 4)]; }
/* the lanes / notes a roll plays live on track t: the pattern does not play them meanwhile (no flam
 * with what was recorded, nor with what this roll is recording) */
static uint32_t roll_lanes(const track_t *t)
{
    uint32_t r, m = 0;
    for (r = 0; r < NROLL; r++)
        if (roll[r].on && roll[r].trk == trk_index(t))
            m |= 1u << (roll[r].note & 15u);
    return m;
}
static int roll_has(const track_t *t, uint32_t note)
{
    uint32_t r;
    for (r = 0; r < NROLL; r++)
        if (roll[r].on && roll[r].trk == trk_index(t) && roll[r].note == note)
            return 1;
    return 0;
}

static void roll_hit(uint32_t r)
{
    track_t *t = &trk[roll[r].trk % NTRK];
    uint32_t u = BEAT_U / roll_den(), su = div_units((uint32_t)t->p[P_SDIV]);
    uint32_t hits = su / u, rat = hits > 4u ? 3u : hits > 1u ? hits - 1u : 0u;
    uint32_t armed = ((song.rec >> trk_index(t)) & 1u) && song.playing, later, abs = 0, rec = 0;
    if (armed) {                                     /* one recorded hit a step: the ratchet does the rest */
        abs = rec_target(t, &later);
        rec = abs != roll[r].rec_abs;
        roll[r].rec_abs = abs;
    }
    if (is_drum(t)) {
        drum_input(roll[r].note, roll[r].lvl, rat, rec || !armed);
        return;
    }
    arm_start(t);
    if (roll[r].off)
        trk_note_off(t, roll[r].note);
    trk_note_on(t, roll[r].note, lvl_vel(roll[r].lvl, 100));
    roll[r].off = u / 2u;
    if (rec)
        rec_note(t, roll[r].note, 100, rat, 0);
    if (ft_on && t == &trk[ft_trk % NTRK])
        ft_note_on(roll[r].note, 100), ft_note_off(roll[r].note);
}

static void roll_start(uint32_t k, track_t *t, uint32_t note, uint32_t lvl)
{
    uint32_t r, den = roll_den(), into, slen, abs;
    for (r = 0; r < NROLL && roll[r].on; r++)
        ;
    if (r == NROLL)
        return;
    roll[r].on = 1;
    roll[r].key = (uint8_t)k;
    roll[r].trk = (uint8_t)trk_index(t);
    roll[r].note = (uint8_t)note;
    roll[r].lvl = (uint8_t)lvl;
    roll[r].off = 0;
    roll[r].rec_abs = SEQ_NONE;
    if (song.playing) {
        abs = grid_at(den, 0, &into, &slen);
        roll[r].last = abs;
        if (into * 4u >= slen * 3u)
            return;                                  /* the grid is just ahead: it starts there */
    } else {
        roll[r].last = 0;
    }
    roll_hit(r);
}

static void roll_end(uint32_t r)
{
    if (roll[r].on && roll[r].off && roll[r].trk != TRK_DRUM)
        trk_note_off(&trk[roll[r].trk % NTRK], roll[r].note);
    roll[r].on = 0;
}

static void roll_block(uint32_t adv)
{
    uint32_t r, den = roll_den(), u = BEAT_U / den, into, slen;
    for (r = 0; r < NROLL; r++) {
        if (!roll[r].on)
            continue;
        if (roll[r].off) {
            if (roll[r].off <= adv) {
                trk_note_off(&trk[roll[r].trk % NTRK], roll[r].note);
                roll[r].off = 0;
            } else {
                roll[r].off -= adv;
            }
        }
        if (song.playing) {
            uint32_t abs = grid_at(den, 0, &into, &slen);
            if (abs != roll[r].last) {
                roll[r].last = abs;
                roll_hit(r);
            }
        } else {
            roll[r].last += adv;
            if (roll[r].last >= u) {
                roll[r].last = roll[r].last - u < u ? roll[r].last - u : 0;
                roll_hit(r);
            }
        }
    }
}

/* ---------------------------------------------------------- keyboard --- */
/* the level of a key on the drum track: OCT- held ghost, OCT+ held hard */
static uint32_t key_lvl(void)
{
    uint32_t b = fm1_in.buttons;
    return (b & dyn_bit[0]) ? LV_GHOST : (b & dyn_bit[1]) ? LV_HARD : LV_NORM;
}

static void key_down(uint32_t k)
{
    uint32_t layer = layer_now(), sel = song.sel % NTRK, i, mc;
    track_t *t = &trk[sel];
    kb_kind[k] = KS_NONE;
    kb_trk[k] = (uint8_t)sel;
    kb_n[k] = 0;
    switch (layer) {
    case LY_FX: {                                     /* FX held: the white keys pick a punch-in effect */
        int32_t fx = punch_key(k);
        kb_kind[k] = KS_FX;
        if (fx < 0) {                                 /* a black key: FILL while held (step conditions) */
            kb_kind[k] = KS_FILL;
            fill_keys |= 1u << k;
            return;
        }
        if (fx < (int32_t)PUNCH_NFX)
            punch_press(fx, k);                       /* (HOLD: while the key is held; LATCH: on / off) */
        return;
    }
    case LY_STEP:
    case LY_SCALE:
    case LY_MIX:
    case LY_SONG:                                     /* the UI's: steps, the key, the mix, the sections */
        kb_kind[k] = KS_UI;
        kb_nt[k][0] = (uint8_t)layer;                 /* (its key-up goes to the same layer) */
        lk_push(layer, k, 1);
        return;
    case LY_ERASE:
        kb_kind[k] = KS_ERASE;
        if (!erasing(t) || er_trk != sel)
            undo_erase_sess = (undo_sess += 4u) | 2u;  /* a new erase: one undo */
        if (er_trk != sel) {
            er_lanes = 0;
            er_notes[0] = er_notes[1] = er_notes[2] = er_notes[3] = 0;
            er_trk = (uint8_t)sel;
        }
        if (is_drum(t)) {
            kb_nt[k][0] = (uint8_t)lane_of_key(k);
            kb_n[k] = 1;
            er_lanes |= (uint16_t)(1u << kb_nt[k][0]);
        } else {
            uint32_t n = kb_map(t, k);
            if (n == KB_SILENT)
                return;
            kb_n[k] = (uint8_t)(t->p[P_CHORD] ? chord_notes(t, n, kb_nt[k]) : 1u);
            if (!t->p[P_CHORD])
                kb_nt[k][0] = (uint8_t)n;
            for (i = 0; i < kb_n[k]; i++)
                er_notes[(kb_nt[k][i] >> 5) & 3u] |= 1u << (kb_nt[k][i] & 31u);
        }
        erase_now(t);
        return;
    default:
        break;
    }
    if (is_drum(t)) {                                 /* the drum track: the key's lane */
        uint32_t lane = lane_of_key(k), lvl = key_lvl();
        kb_nt[k][0] = (uint8_t)lane;
        kb_n[k] = 1;
        if (layer == LY_ROLL) {
            kb_kind[k] = KS_ROLL;
            roll_start(k, t, lane, lvl);
            return;
        }
        kb_kind[k] = KS_DRUM;
        drum_input(lane, lvl, 0, 1);
        mc = trk_midi_ch(sel);
        midi_out_event(0x09u | (0x90u | mc) << 8 | (uint32_t)LANE_NOTE[lane] << 16 | lvl_vel(lvl, 100) << 24);
        return;
    }
    {
        uint32_t n = kb_map(t, k);
        if (n == KB_SILENT)
            return;
        if (layer == LY_ROLL) {
            kb_kind[k] = KS_ROLL;
            kb_nt[k][0] = (uint8_t)n;
            kb_n[k] = 1;
            roll_start(k, t, n, LV_NORM);
            return;
        }
        kb_kind[k] = KS_NOTE;
        if (t->p[P_CHORD]) {
            kb_n[k] = (uint8_t)chord_notes(t, n, kb_nt[k]);
        } else {
            kb_nt[k][0] = (uint8_t)n;
            kb_n[k] = 1;
        }
        mc = trk_midi_ch(sel);
        for (i = 0; i < kb_n[k]; i++) {
            input_on(t, kb_nt[k][i], 100);
            midi_out_event(0x09u | (0x90u | mc) << 8 | (uint32_t)kb_nt[k][i] << 16 | 100u << 24);
        }
        /* the pen of the SEQ layer: the keys down now (a chord), else this note */
        if (!(kb_prev & ~(1u << k)) || pen_n >= 4u)
            pen_n = 0;
        for (i = 0; i < kb_n[k] && pen_n < 4u; i++)
            pen_note[pen_n++] = kb_nt[k][i];
    }
}

static void key_up(uint32_t k)
{
    uint32_t i, mc, kind = kb_kind[k];
    track_t *t = &trk[kb_trk[k] % NTRK];
    kb_kind[k] = KS_NONE;
    switch (kind) {
    case KS_FX:
        if (punch.keybit == 1u << k) {                /* its key is up: the mix comes back */
            punch.keybit = 0;
            punch.req = -1;
        }
        return;
    case KS_FILL:
        fill_keys &= ~(1u << k);
        return;
    case KS_UI:
        lk_push(kb_nt[k][0], k, 0);
        return;
    case KS_ERASE:
        if (is_drum(t))
            er_lanes &= (uint16_t)~(1u << kb_nt[k][0]);
        else
            for (i = 0; i < kb_n[k]; i++)
                er_notes[(kb_nt[k][i] >> 5) & 3u] &= ~(1u << (kb_nt[k][i] & 31u));
        return;
    case KS_ROLL:
        for (i = 0; i < NROLL; i++)
            if (roll[i].on && roll[i].key == k)
                roll_end(i);
        return;
    case KS_NOTE:
        mc = trk_midi_ch(kb_trk[k] % NTRK);
        for (i = 0; i < kb_n[k]; i++) {
            input_off(t, kb_nt[k][i]);
            midi_out_event(0x08u | (0x80u | mc) << 8 | (uint32_t)kb_nt[k][i] << 16);
        }
        return;
    case KS_DRUM:
        if (ft_on && ft_trk == TRK_DRUM)
            ft_note_off(kb_nt[k][0]);
        mc = trk_midi_ch(TRK_DRUM);
        midi_out_event(0x08u | (0x80u | mc) << 8 | (uint32_t)LANE_NOTE[kb_nt[k][0] & 15u] << 16);
        return;
    default:
        return;
    }
}

static void keyboard_block(void)
{
    uint32_t cur = fm1_in.notes, ch = cur ^ kb_prev, k, r;
    if (!(layer_buttons() & ly_bit[LY_ROLL]))         /* ARP up (and not locked): the rolls end (the keys stay silent) */
        for (r = 0; r < NROLL; r++)
            if (roll[r].on)
                roll_end(r);
    if (!ch)
        return;
    for (k = 0; k < 27u; k++) {
        if (!((ch >> k) & 1u))
            continue;
        if ((cur >> k) & 1u)
            key_down(k);
        else
            key_up(k);
        kb_prev ^= 1u << k;                           /* (key_down sees the keys down before it) */
    }
}

/* LIVE: one record arm at most, on the selected track; selecting another track moves it there
 * (an arm follows too: it waits for a note on the track selected; a free take stays on its track) */
static void rec_follow(uint32_t sel)
{
    if (song.rec)
        song.rec = (uint8_t)(1u << (sel % NTRK));
}

/* -------------------------------------------------------- metronome --- */
/* A wood block on every beat, louder on the first of the bar, from the drum kit's voices
 * (not recorded, not muted with the drum track; GLO > DRUMS LVL sets its level).
 * GLO > GLOBAL CLICK: OFF (0, default: LIVE needs none), REC = while a track records,
 * ON = while playing. */
#define CLICK_REC 1
#define CLICK_ON 2
static uint32_t click_last = SEQ_NONE;          /* the beat it last played */
static void click_tick(void)
{
    uint32_t mode = (uint32_t)song.g[G_CLOCK];
    if (!song.playing || clk_beat == click_last)
        return;
    click_last = clk_beat;
    if (mode == CLICK_ON || (mode == CLICK_REC && song.rec))
        drum_on(clk_beat % 4u ? 76u : 77u, clk_beat % 4u ? 72u : 120u);
}

/* -------------------------------------------------------- sequencer --- */
#if FELUCCA_ARRANGER
/* ---- LIVE SECTIONS (SAVE held + key, ui_layers.c): a section asked for while playing starts on the
 * next bar, every track from its step 0 (as the song does). SONG REC writes the order you play into the
 * song chain (arrangement): each section with the bars it played, from the bar after the arm. */
static volatile int8_t live_req = -1;              /* section asked for (UI), applied on the next bar */
static volatile int8_t live_sec = -1;              /* the section playing: last jumped to, loaded or stored */
static uint32_t live_bar = 0xFFFFFFFFu;            /* clk_beat / 4 of the last bar seen */
static volatile uint8_t srec;                      /* SONG REC: 0 off, 1 armed (from the next bar), 2 recording */
static arr_entry_t srec_e[ARR_STEPS];
static volatile uint8_t srec_n;                    /* entries so far (the last one still growing) */
static volatile uint8_t srec_done;                 /* the chain was written: n parts (0xFF: nothing played) */

static void srec_finish(void)                      /* (audio ISR, or the UI with the IRQ off) */
{
    uint32_t i, n = 0;
    for (i = 0; i < srec_n; i++)
        if (srec_e[i].bars)
            srec_e[n++] = srec_e[i];
    if (n) {
        arrangement.count = (uint8_t)n;
        arrangement.loop = 0;
        for (i = 0; i < n; i++)
            arrangement.entry[i] = srec_e[i];
    }
    srec_done = (uint8_t)(n ? n : 0xFFu);
    srec = 0;
    srec_n = 0;
}
static void srec_add(uint32_t s)
{
    if (srec_n >= ARR_STEPS) {
        srec_finish();                              /* the chain is full: what was played so far */
        return;
    }
    srec_e[srec_n].scene = (uint8_t)s;
    srec_e[srec_n].bars = 0;
    srec_n++;
}
/* STOP (or SONG REC pressed again): the bar playing counts if it had begun */
static void srec_stop(void)
{
    if (srec == 2u && srec_n && srec_e[srec_n - 1u].bars < 64u &&
        ((!(clk_beat & 3u) && (clk_beat >> 2) != live_bar) || (clk_beat & 3u)))
        srec_e[srec_n - 1u].bars++;
    if (srec == 2u)
        srec_finish();
    srec = 0;
}
static void seq_reset_tracks(uint32_t pos);
static void live_block(void)                       /* once a block while playing a loop (not the song) */
{
    if ((clk_beat & 3u) || (clk_beat >> 2) == live_bar)
        return;
    live_bar = clk_beat >> 2;                       /* a new bar */
    if (srec == 2u && srec_n) {
        arr_entry_t *e = &srec_e[srec_n - 1u];
        if (e->bars < 64u) {
            e->bars++;
        } else {                                    /* (64 bars of one section: it goes on in the next entry) */
            srec_add(e->scene);
            if (srec == 2u)
                srec_e[srec_n - 1u].bars = 1;
        }
    }
    if (live_req >= 0) {
        uint32_t s = (uint32_t)live_req;
        live_req = -1;
        {                                           /* (the UI asked for a section it checked: no hash here) */
            arrangement_apply(s);
            song.rec = 0;                           /* (a take does not run on into another section) */
            live_sec = (int8_t)s;
            seq_reset_tracks(clk_pos);              /* on the bar: every track from its step 0 */
            live_bar = 0;
            if (srec == 2u)
                srec_add(s);
        }
    }
    if (srec == 1u && live_sec >= 0) {
        srec = 2;
        srec_n = 0;
        srec_add((uint32_t)live_sec);
    }
}
#endif

/* every track from its step 0, together, at pos units into beat 0 (a song section: the arranger's
 * remainder, so the new section starts exactly on its bar) */
static void seq_reset_tracks(uint32_t pos)
{
    uint32_t i;
    for (i = 0; i < NTRK; i++) {
        track_t *t = &trk[i];
        t->seq_abs = SEQ_NONE;
        t->seq_idx = 0;
        t->seq_pass = 0;
        t->seq_fail = 0;
        t->rskip_n = 0;
        t->rskip_lanes = 0;
        t->rskip_abs = SEQ_NONE;
        t->rh_n = 0;
        t->arp_new = t->nheld != 0;
        plk_drop(t);
    }
    for (i = 0; i < NROLL; i++)
        roll[i].last = SEQ_NONE - 1u;               /* (a roll held over the start: on the grid from here) */
    clk_beat = 0;
    clk_pos = pos;
#if FELUCCA_ARRANGER
    live_bar = 0xFFFFFFFFu;                        /* (bar 0 is a new bar: SONG REC can start on it) */
#endif
    click_last = SEQ_NONE;
    song.tick = 0;
    song.playing = 1;
    slicer_start(pos);                             /* slicer.c: its step 0 with the sequencer's */
}


static void song_backup(void);                     /* project.c: song mode keeps the loop you made */
static void song_restore(void);

static void seq_start(void)
{
#if FELUCCA_ARRANGER
    if (arrangement_enabled && !song.playing) {
        rec_wait = 0;                              /* song mode plays, it does not record */
        song_backup();
    }
    if (!arrangement_start()) return;
#endif
    seq_reset_tracks(0);
}

static void seq_release(track_t *t)
{
    uint32_t i;
    for (i = 0; i < t->seq_n; i++)
        trk_note_off(t, t->seq_notes[i]);
    t->seq_n = 0;
    t->seq_hold = 0;
    t->slide_glide = 0;                             /* live MONO / LEG keys must not glide after it */
}

static void seq_stop(void)
{
    uint32_t i;
#if FELUCCA_ARRANGER
    if (song.playing)
        srec_stop();                                /* SONG REC: the order played so far is the song */
    live_req = -1;
#endif
    song.playing = 0;
    punch_unlatch();                               /* STOP (the song's end, a load) ends a latched punch-in */
    for (i = 0; i < NTRK; i++) {
        seq_release(&trk[i]);
        plk_drop(&trk[i]);                         /* the tracks' own values */
        trk[i].rh_n = 0;                           /* a recorded note held over the stop: as far as it got */
    }
#if FELUCCA_ARRANGER
    if (arrangement_clock.running) {
        arrangement_clock.running = 0;
        song_restore();                            /* back to the loop you were making */
    }
#endif
}

/* the velocity of note i of synth step s */
static uint32_t step_vel(const step_t *s, uint32_t i)
{
    uint32_t base = (s->flags & SF_ACCENT) ? 127u : (s->vel ? s->vel : 96u);
    return lvl_vel((s->lvl >> (2u * i)) & 3u, base);
}

/* play one synth step: TIE extends, REST releases, NOTE (re)triggers; a SLIDE on the previous
 * step makes this one legato with a glide (acid style). skip: bit k = note k already sounds
 * from live recording (not triggered, not released here). len: the step's length (units). */
static void seq_step(track_t *t, const step_t *s, uint32_t slen, uint32_t skip)
{
    uint32_t i, j, gate = slen * (uint32_t)t->p[P_SGATE] / 128u;
    uint32_t slide_in = t->seq_hold && t->seq_n;
    uint32_t next_tie = t->step[(t->seq_idx + 1u) % trk_len(t)].time == ST_TIE;
    if (s->time == ST_TIE) {
        if (t->seq_n) {
            t->seq_off = gate + slen / 2u;
            t->seq_hold = (s->flags & SF_SLIDE) != 0 || next_tie;   /* chains hold at any GATE / swing */
        }
        return;
    }
    if (s->time == ST_REST || !s->n) {
        seq_release(t);
        return;
    }
    if (s->rat) {                                   /* ratchets: each hit its share of the step */
        uint32_t hits = 1u + ((s->rat >> 0) & 3u);
        for (i = 1; i < s->n; i++)
            if (1u + ((s->rat >> (2u * i)) & 3u) > hits)
                hits = 1u + ((s->rat >> (2u * i)) & 3u);
        gate /= hits;
    }
    t->slide_glide = (uint8_t)slide_in;
    if (!slide_in)
        seq_release(t);
    for (i = 0; i < s->n; i++)
        if (roll_has(t, s->note[i]))
            skip |= 1u << i;                        /* (a roll plays it) */
    for (i = 0; i < s->n; i++)
        if (!((skip >> i) & 1u))
            trk_note_on(t, s->note[i], step_vel(s, i));
    if (slide_in)                                   /* release what is not held over */
        for (i = 0; i < t->seq_n; i++) {
            for (j = 0; j < s->n && s->note[j] != t->seq_notes[i]; j++)
                ;
            if (j == s->n)
                trk_note_off(t, t->seq_notes[i]);
        }
    t->seq_n = 0;
    for (i = 0; i < s->n; i++)
        if (!((skip >> i) & 1u))
            t->seq_notes[t->seq_n++] = s->note[i];
    t->seq_off = gate;
    t->seq_hold = !s->rat && ((s->flags & SF_SLIDE) != 0 || next_tie);   /* next step a TIE: keep the notes to it */
}

/* play one drum step: each lane a hit (skip: lanes already played by live recording, or rolling) */
static void drum_step(track_t *t, const dstep_t *s, uint32_t skip)
{
    uint32_t l, m = dstep_mask(s) & ~skip & ~roll_lanes(t);
    for (l = 0; m; l++, m >>= 1)
        if (m & 1u)
            trk_note_on(t, LANE_NOTE[l], lvl_vel(dstep_lvl(s, l), 100));
}

/* ratchets: the further hits of the playing step's notes / lanes, each at its share of the step */
static void seq_ratchets(track_t *t, uint32_t into, uint32_t slen)
{
    uint32_t i;
    if (t->seq_fail)
        return;                                     /* (its condition failed: a rest) */
    if (is_drum(t)) {
        const dstep_t *s = &t->dstep[t->seq_idx % NSTEP];
        uint32_t m = dstep_mask(s) & ~roll_lanes(t);
        for (i = 0; m; i++, m >>= 1) {
            uint32_t hits = 1u + dstep_rat(s, i), h, done;
            if (!(m & 1u) || hits == 1u)
                continue;
            h = into * hits / slen;
            done = (t->rat_lanes >> (2u * i)) & 3u;
            if (h > done && h < hits) {
                t->rat_lanes = (t->rat_lanes & ~(3u << (2u * i))) | h << (2u * i);
                trk_note_on(t, LANE_NOTE[i], lvl_vel(dstep_lvl(s, i), 100));
            }
        }
        return;
    }
    {
        const step_t *s = &t->step[t->seq_idx % NSTEP];
        if (s->time != ST_NOTE || !s->rat)
            return;
        for (i = 0; i < s->n; i++) {
            uint32_t hits = 1u + ((s->rat >> (2u * i)) & 3u), h;
            if (hits == 1u || roll_has(t, s->note[i]))
                continue;
            h = into * hits / slen;
            if (h > t->rat_done[i] && h < hits) {
                uint32_t j;
                t->rat_done[i] = (uint8_t)h;
                trk_note_off(t, s->note[i]);
                trk_note_on(t, s->note[i], step_vel(s, i));
                t->seq_off = slen / hits * (uint32_t)t->p[P_SGATE] / 128u;
                for (j = 0; j < t->seq_n && t->seq_notes[j] != s->note[i]; j++)
                    ;
                if (j == t->seq_n && t->seq_n < 4u)
                    t->seq_notes[t->seq_n++] = s->note[i];   /* (its gate ends it) */
            }
        }
    }
}

static void seq_tick(track_t *t, uint32_t adv)
{
    uint32_t len = trk_len(t), into, slen, abs, idx;
    if (t->seq_n && !t->seq_hold) {
        if (t->seq_off <= adv)
            seq_release(t);
        else
            t->seq_off -= adv;
    }
    if (!song.playing)
        return;
    abs = trk_grid(t, &into, &slen);
    if (t->seq_abs != SEQ_NONE && abs + 1u == t->seq_abs)
        abs = t->seq_abs;                            /* SWING turned up inside a played odd step */
    if (abs != t->seq_abs) {                         /* a new step: one a block at most */
        t->seq_abs = abs;
        idx = abs % len;
        t->seq_idx = (uint16_t)idx;
        t->rat_done[0] = t->rat_done[1] = t->rat_done[2] = t->rat_done[3] = 0;
        t->rat_lanes = 0;
        if (!idx) {
            t->pass++;                               /* a new pass of the loop (recording: one undo) */
            t->seq_pass++;
        }
        if (erasing(t))
            erase_step(t, idx);                      /* EDIT + key held: gone as it passes */
        if (t->p[P_TURN])
            turing_step(t, idx);                     /* TURN: rewritten first, with its chance */
        t->seq_fail = t->cond[idx] && step_sounds(t, idx) && !cond_ok(t, t->cond[idx]);
        if (is_drum(t)) {
            uint32_t skip = t->rskip_abs == abs ? t->rskip_lanes : 0u;
            t->rskip_lanes = 0;
            plk_load(t, idx);
            if (!t->seq_fail)
                drum_step(t, &t->dstep[idx], skip);
        } else {
            const step_t *s = &t->step[idx];
            uint32_t skip = 0, i, k;
            rec_hold(t, idx, len, abs);
            if (t->rskip_n && t->rskip_abs == abs)
                for (i = 0; i < s->n; i++)
                    for (k = 0; k < t->rskip_n; k++)
                        if (s->note[i] == t->rskip[k])
                            skip |= 1u << i;
            t->rskip_n = 0;
            if (s->time != ST_TIE)
                plk_load(t, idx);                    /* (a TIE: the note's locks go on) */
            if (t->seq_fail)
                seq_release(t);                      /* its condition failed: a rest */
            else
                seq_step(t, s, slen, skip);
        }
    }
    seq_ratchets(t, into, slen);
}

/* MIDI in: the track a channel plays (0..15) */
static track_t *midi_track(uint32_t ch)
{
    if (song.g[G_DRCH] && ch + 1u == (uint32_t)song.g[G_DRCH])
        return TDRUM;
    return ch < NPART ? &trk[ch] : TSEL;
}

/* a channel that plays the selected track: its note-off goes to the track its note-on went to,
 * even when another track was selected in between (else that note would hang) */
static uint8_t midi_sel_on[16][128];                  /* per channel and note: track + 1, 0 = none */
static track_t *midi_route(uint32_t ch, uint32_t note, int on)
{
    track_t *t = midi_track(ch);
    if (ch < NPART || (song.g[G_DRCH] && ch + 1u == (uint32_t)song.g[G_DRCH]))
        return t;                                     /* a part's own channel, or the drum channel */
    if (on)
        midi_sel_on[ch & 15u][note & 127u] = (uint8_t)(song.sel + 1u);
    else if (midi_sel_on[ch & 15u][note & 127u]) {
        t = &trk[(midi_sel_on[ch & 15u][note & 127u] - 1u) % NTRK];
        midi_sel_on[ch & 15u][note & 127u] = 0;
    }
    return t;
}

/* MIDI clock in (GLO > SYSTEM > SYNC = USB or TRS; after Felucca 1.0's midi_clock.c, from contributions by
 * ChanceTheMaker and keremimo): 24 pulses a beat. While the clock runs, the sequencer advances by the
 * pulses (a pulse = BEAT_U / 24 units), interpolated up to the next one from the last interval but never
 * past it, so it follows the master's tempo changes and cannot drift; BPM shows the master's tempo
 * (the slicer, delay and arp follow it). START restarts from the top, CONTINUE carries on where it
 * stopped, STOP stops. With no pulse for 0.5 s, the internal tempo takes over (PLAY works as ever). */
#define MCLK_PULSE_U (BEAT_U / 24u)
static struct {
    uint32_t pos, done;          /* units: the master's position (pulses since START), ours */
    uint32_t last_ms, iv_ms;     /* the last pulse, the interval between pulses (smoothed) */
    uint32_t beat_ms;            /* when pulse 0 of the last 24 came: the tempo */
    uint8_t have, n24;           /* a pulse since START; pulses towards the next tempo reading */
    uint8_t alive;               /* pulses are coming (from the SYNC source) */
} mclk;

static int mclk_on(void)                          /* the clock drives the sequencer */
{
    return song.g[G_SYNC] && mclk.alive && fm1_ms - mclk.last_ms < 500u;
}

static void mclk_event(uint32_t st, uint32_t src)  /* a realtime message; src 1 USB, 2 TRS */
{
    uint32_t now = fm1_ms;
    if (!song.g[G_SYNC] || src != (uint32_t)song.g[G_SYNC])
        return;
    if (st == 0xFAu || st == 0xFBu) {              /* START: from the top; CONTINUE: on from where it stopped */
        mclk.pos = mclk.done = 0;
        mclk.have = 0;
        if (st == 0xFAu)
            transport_req = 1;
        else if (!song.playing)
            song.playing = 1;
        return;
    }
    if (st == 0xFCu) {                             /* STOP */
        transport_req = 2;
        return;
    }
    if (st != 0xF8u)
        return;
    if (mclk.alive && now - mclk.last_ms < 200u) { /* the interval, smoothed (a gap is not a tempo) */
        uint32_t iv = now - mclk.last_ms;
        mclk.iv_ms = mclk.iv_ms ? (mclk.iv_ms * 3u + iv + 2u) / 4u : iv;
    }
    if (!mclk.alive || now - mclk.last_ms >= 500u) {   /* (re)started: count a fresh beat */
        mclk.n24 = 0;
        mclk.beat_ms = now;
    } else if (++mclk.n24 == 24u) {                /* a beat: the tempo */
        uint32_t dt = now - mclk.beat_ms;
        mclk.n24 = 0;
        mclk.beat_ms = now;
        if (dt >= 250u && dt <= 1500u)             /* 40..240 BPM */
            song.g[G_BPM] = (int16_t)clamp((int32_t)((60000u + dt / 2u) / dt), 40, 240);
    }
    mclk.alive = 1;
    mclk.last_ms = now;
    if (song.playing || transport_req == 1u) {     /* (a START queued with it: the next block starts) */
        if (mclk.have)
            mclk.pos += MCLK_PULSE_U;
        mclk.have = 1;                             /* the first pulse after START is the downbeat */
    }
}

static uint32_t mclk_adv(uint32_t n)               /* units to advance this block (mclk_on) */
{
    uint32_t el, off = 0, tgt, adv, cap = n * 2u * (uint32_t)song.g[G_BPM];
    if (!mclk.have)
        return 0;                                  /* START seen: wait for the downbeat */
    el = fm1_ms - mclk.last_ms;
    if (mclk.iv_ms) {
        if (el > mclk.iv_ms)
            el = mclk.iv_ms;
        off = el * MCLK_PULSE_U / mclk.iv_ms;      /* (<= 200 x 110250: fits 32 bits) */
        if (off >= MCLK_PULSE_U)
            off = MCLK_PULSE_U - 1u;
    }
    tgt = mclk.pos + off;
    adv = (int32_t)(tgt - mclk.done) > 0 ? tgt - mclk.done : 0u;
    if (adv > cap)
        adv = cap;                                 /* behind: catch up at twice the tempo, no burst */
    mclk.done += adv;
    return adv;
}

/* everything that happens between two rendered blocks: transport, input, the steps of every
 * track at the clock, the click, the rolls and the arps; then the clock moves on by n samples */
static void events_block(uint32_t n)
{
    uint32_t i, pr, adv;
    if (transport_req == 1u) {
        transport_req = 0;
        if (ft_on) {
            ft_close();                             /* (PLAY from elsewhere: the editor) */
        } else if (ci_on) {
            ci_on = 0;                              /* PLAY again during the count-in: back to armed */
        } else if (rec_wait && rec_count && !song.playing && !mclk_on() &&
                   !(project_empty() && !rec_tempo)) {
            ci_on = 1;                              /* COUNT: one bar of clicks first (below) */
            ci_u = 0;
            ci_beat = 0;
            drum_on(77u, 120u);
        } else {
            seq_start();
            if (rec_wait && song.playing)
                rec_begin();                        /* PLAY while armed: record from the top */
        }
    } else if (transport_req == 2u) {
        seq_stop();
        transport_req = 0;
        song.rec = 0;                               /* STOP ends the take (and the wait) */
        rec_wait = 0;
        if (ft_on) {
            ft_on = 0;                              /* a free take: dropped */
            ft_bars = 0xFF;
        }
    }
    if (ci_on) {                                    /* the count-in: 4 beats at the tempo, then go */
        if (!rec_wait || song.playing) {
            ci_on = 0;                              /* (REC cancelled it, or it started some other way) */
        } else {
            uint32_t b;
            ci_u += n * (uint32_t)song.g[G_BPM];
            b = ci_u / BEAT_U;
            if (b >= 4u) {
                ci_on = 0;
                seq_start();
                if (song.playing)
                    rec_begin();
            } else if (b != ci_beat) {
                ci_beat = (uint8_t)b;
                drum_on(76u, 72u);
            }
        }
    }
    if (rec_wait && song.playing)
        rec_begin();                                /* started some other way: record now */
    adv = mclk_on() && song.playing ? mclk_adv(n) : n * (uint32_t)song.g[G_BPM];   /* (after a START) */
    ft_block();
#if FELUCCA_ARRANGER
    if (song.playing && arrangement_clock.running) {
        int scene = arr_next(&arrangement_clock, &arrangement, FS);
        if (scene == ARR_DONE) {
            arrangement_clock.running = 1;          /* (arr_next cleared it: seq_stop brings the loop back) */
            seq_stop();
        }
        else if (scene >= 0) {
            arrangement_apply((uint32_t)scene);
            seq_reset_tracks(arrangement_clock.phase);   /* (the remainder: exactly on the bar) */
        }
    } else if (song.playing) {
        live_block();
    }
#endif
    pr = panic_req;
    panic_req = 0;
    for (i = 0; i < NTRK; i++) {
        track_t *t = &trk[i];
        if ((pr >> i) & 1u) {
            trk_all_off(t);
            t->nheld = 0;
            t->arp_phys = 0;
            t->arp_n = t->arp_snd = t->arp_rat = 0;
        }
        if (i < NPART)
            engine_block(t);                          /* engine switch: fade, then switch (voice.c) */
        /* ARP turned off, or HOLD released with no key down: drop the latched chord */
        if ((t->armp && !t->p[P_AMODE]) || (t->aholdp && !t->p[P_AHOLD] && !t->arp_phys)) {
            t->nheld = 0;
            if (!t->p[P_AMODE])
                t->arp_phys = 0;
            arp_release(t);
            t->arp_rat = 0;
        }
        t->armp = t->p[P_AMODE];
        t->aholdp = t->p[P_AHOLD];
    }
    keyboard_block();
    while (mi_r != mi_w) {                            /* USB-MIDI (and TRS) in */
        uint32_t pkt = midi_in_q[mi_r % MQ], st = (pkt >> 8) & 0xF0u, ch = (pkt >> 8) & 0x0Fu;
        uint32_t d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        track_t *t;
        mi_r++;
        if ((pkt & 15u) == 0xFu) {                    /* clock / transport: cable 0 USB, 1 TRS */
            mclk_event((pkt >> 8) & 0xFFu, ((pkt >> 4) & 15u) ? 2u : 1u);
            continue;
        }
        if (st != 0x90u && st != 0x80u)
            continue;
        t = midi_route(ch, d1, st == 0x90u && d2);
        if (is_drum(t)) {
            if (st == 0x90u && d2)
                drum_input(lane_of_note(d1), vel_lvl(d2), 0, 1);
        } else if (st == 0x90u && d2) {
            input_on(t, d1, d2);
        } else {
            input_off(t, d1);
        }
    }
    for (i = 0; i < NTRK; i++)
        seq_tick(&trk[i], adv);
    click_tick();
    roll_block(adv);
    for (i = 0; i < NPART; i++)
        arp_tick(&trk[i], adv);
    if (song.playing) {
        song.tick++;
        clk_pos += adv;
        while (clk_pos >= BEAT_U) {
            clk_pos -= BEAT_U;
            clk_beat++;
        }
#if FELUCCA_ARRANGER
        arr_elapse(&arrangement_clock, adv, 1u);   /* (units: n x BPM, or the MIDI clock) */
#endif
    }
}
