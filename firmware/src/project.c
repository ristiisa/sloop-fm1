/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Projects: four slots in .noinit RAM (the song sections A..D), so they survive resets and UBOOT
 * entry. With FELUCCA_FLASH every save also goes to flash through storage.c, and an empty RAM slot
 * is filled from flash on load. The working project is also kept in flash by itself (autosave, when
 * the transport is stopped and nothing sounds) and comes back at power-on: SLOOP starts where you
 * left it.
 *
 * Formats: 5 ("FUN5", written, SLOOP 2.4): format 4 plus, per track, the nudge of each step (micro), NLOCK
 * parameter locks, the steps' fill conditions (2 bits each) and P_TFLT, P_STRUM, P_VLEAD (just before P_E0).
 * SLOOP 2.5 writes format 5 byte for byte as SLOOP 2.4 does (a project moves between the two), holding SLOOP
 * 2.4's parameters (core.h P_NP_V24, G_NG_V24); what 2.5 adds goes into an extension record (below: the
 * step conditions, P_AACC..P_CRATE, G_EVOL..G_PROG), kept in flash with its project (storage.c st_save_x)
 * and, for the song sections, beside the slots in RAM. No extension (a project of 2.4, or of 2.4 since):
 * those take their defaults and the conditions come from the fill bits (FILL, !FILL). A lock of a 2.5
 * parameter is stored with an id past P_NP_V24 (2.4 frees such a lock).
 * Read and converted: 4 ("FUN4", SLOOP 2.0 .. 2.3: PROJ_NP_V4 parameters, today's G_COUNT, 10-byte
 * steps with levels and ratchets, the drum track's 16 lanes; no nudge, no lock), 3 ("FUN3", SLOOP 1.x: 8-byte
 * steps, the drum track's notes become its lanes, the swings x 0.8 for the MPC scale), 2 ("FUN2") and 1
 * ("FUN1"), which held PROJ_NP_V2 parameters per track, mapped by count as user presets are (the first
 * PROJ_NP_V2 - 8 are P_LEVEL.. in order, the last 8 P_E0..P_E7; the parameters added since take their
 * defaults). Their engine bytes are kept: formats 1 and 2 had engines 0..7 (ANALOG .. WHEEL), and the
 * engines added since were appended, no index moved; the drum track's byte (it has no engine) becomes 0.
 *
 * Built on the host too (tests/project_test.c, -DPROJ_HOST): the part above the #ifndef
 * PROJ_HOST needs core.h, params.c (TP), drums.c (the lanes), the engines and trk_def_engine (ui.c). */
#define PROJ_MAGIC 0x46554E35u                 /* "FUN5": format 4 + per-step nudge, parameter locks, fill conditions (SLOOP 2.4) */
#define PROJ_MAGIC_V4 0x46554E34u              /* "FUN4": four tracks, P_COUNT parameters each, 10-byte steps; read only */
#define PROJ_MAGIC_V3 0x46554E33u              /* "FUN3": SLOOP 1.x; read only */
#define PROJ_MAGIC_V2 0x46554E32u              /* "FUN2": four tracks, PROJ_NP_V2 parameters; read only */
#define PROJ_MAGIC_V1 0x46554E31u              /* "FUN1": one instrument; loads into track 1 */
#define PROJ_NP_V5 P_NP_V24                    /* P_COUNT of format 5 (P_E0 was 53: P_E0_V24) */
#define PROJ_NG_V5 G_NG_V24                    /* G_COUNT of formats 4 and 5 */
#define PROJ_NP_V4 58u                         /* P_COUNT of format 4 (P_E0 was 50) */
#define PROJ_NP_V3 57u                         /* P_COUNT of format 3 (P_E0 was 49) */
#define PROJ_NG_V3 27u                         /* G_COUNT of formats 1..3 */
#define PROJ_NP_V2 53u                         /* P_COUNT of formats 1 and 2 (P_E0 was 45) */
#define PROJ_NG_V2 27u                         /* G_COUNT of formats 1 and 2 */
typedef struct {                               /* one track; the drum track ignores engine / preset */
    int16_t p[PROJ_NP_V5];                     /* P_LEVEL..P_VLEAD, P_E0..P_E7 */
    uint8_t engine, preset;
    union {
        step_t step[NSTEP];
        dstep_t dstep[NSTEP];                  /* (the drum track: 16 lanes, the same size) */
    };
    int8_t micro[NSTEP];                       /* SLOOP 2.4: each step's nudge (core.h) */
    plock_t lock[NLOCK];                       /* and its parameter locks (step LOCK_FREE = none) */
    uint8_t fill[NSTEP / 4];                   /* and its fill condition, 2 bits a step (FC_*) */
} proj_trk_t;
typedef struct {
    uint32_t magic, size;
    int16_t g[PROJ_NG_V5];
    uint8_t sel, rsv[3];                       /* the selected track */
    proj_trk_t t[NTRK];
    uint32_t sum;
} project_t;
typedef struct {                               /* a track of format 4 (SLOOP 2.0 .. 2.3), read only */
    int16_t p[PROJ_NP_V4];
    uint8_t engine, preset;
    union {
        step_t step[NSTEP];
        dstep_t dstep[NSTEP];
    };
} proj_trk_v4_t;
typedef struct {                               /* format 4, read only */
    uint32_t magic, size;
    int16_t g[PROJ_NG_V5];
    uint8_t sel, rsv[3];
    proj_trk_v4_t t[NTRK];
    uint32_t sum;
} project_v4_t;
typedef struct { uint8_t note[4], n, time, flags, vel; } step8_t;   /* the steps of formats 1..3 */
typedef struct {                               /* a track of format 3, read only */
    int16_t p[PROJ_NP_V3];
    uint8_t engine, preset;
    step8_t step[NSTEP];
} proj_trk_v3_t;
typedef struct {                               /* format 3 (SLOOP 1.x), read only */
    uint32_t magic, size;
    int16_t g[PROJ_NG_V3];
    uint8_t sel, rsv[3];
    proj_trk_v3_t t[NTRK];
    uint32_t sum;
} project_v3_t;
typedef struct {                               /* a track of formats 1 and 2, read only */
    int16_t p[PROJ_NP_V2];
    uint8_t engine, preset;
    step8_t step[NSTEP];
} proj_trk_v2_t;
typedef struct {                               /* format 2 (until 0.9), read only */
    uint32_t magic, size;
    int16_t g[PROJ_NG_V2];
    uint8_t sel, rsv[3];
    proj_trk_v2_t t[NTRK];
    uint32_t sum;
} project_v2_t;
typedef struct {                               /* format 1 (until 0.5 beta), read only */
    uint32_t magic, size;
    int16_t g[PROJ_NG_V2];
    proj_trk_v2_t t;
    uint32_t sum;
} project_v1_t;
_Static_assert(sizeof(project_v2_t) == 2552u && sizeof(project_v1_t) == 688u && sizeof(project_v3_t) == 2584u &&
               sizeof(project_v4_t) == 3112u, "formats 1 / 2 / 3 / 4 as they were stored");
_Static_assert(sizeof(project_t) == 3840u, "format 5 as SLOOP 2.4 stores it (one flash object: storage.c ST_PAYLOAD_MAX)");
project_t proj_slot[4] __attribute__((section(".noinit")));

static uint32_t proj_hash(const void *p, uint32_t n)   /* FNV-1a over n bytes */
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t i, s = 0x811C9DC5u;
    for (i = 0; i < n; i++)
        s = (s ^ b[i]) * 16777619u;
    return s;
}
static uint32_t proj_sum(const project_t *p) { return proj_hash(p, sizeof *p - 4u); }
static int proj_ok(const project_t *q) { return q->magic == PROJ_MAGIC && q->size == sizeof *q && q->sum == proj_sum(q); }

/* ---- old formats -> format 5 */
/* an old step into a synth step (no level, no ratchet) */
static void step_from8(step_t *d, const step8_t *s)
{
    memcpy(d->note, s->note, 4);
    d->n = s->n;
    d->time = s->time;
    d->flags = s->flags;
    d->vel = s->vel;
    d->lvl = d->rat = 0;
}
/* an old drum step (GM notes) into the drum track's lanes; its velocity / accent -> their level */
static void dstep_from8(dstep_t *d, const step8_t *s)
{
    uint32_t i, lvl = (s->flags & SF_ACCENT) || s->vel > 115u ? LV_HARD : !s->vel ? LV_NORM : vel_lvl(s->vel);
    memset(d, 0, sizeof *d);
    if (s->time != ST_NOTE)
        return;
    for (i = 0; i < s->n && i < 4u; i++)
        dstep_set(d, lane_of_note(s->note[i] & 127u), lvl, 0);
}
static int16_t swing_from_v3(int32_t v) { return (int16_t)clamp((v * 4 + 2) / 5, 0, 100); }   /* /250 -> /200 */

/* the globals of formats 1..3 (G_* unchanged since; any added later: their defaults) */
static void proj_g_from_old(int16_t *g, const int16_t *g2)
{
    uint32_t i;
    for (i = 0; i < PROJ_NG_V5; i++)
        g[i] = i < PROJ_NG_V3 ? g2[i] : GP[i].def;
    g[G_SWING] = swing_from_v3(g[G_SWING]);
}

/* no nudge, no lock, no condition (formats 1..4) */
static void proj_trk_plain(proj_trk_t *d)
{
    uint32_t k;
    memset(d->micro, 0, sizeof d->micro);
    memset(d->fill, 0, sizeof d->fill);
    for (k = 0; k < NLOCK; k++) {
        d->lock[k].step = LOCK_FREE;
        d->lock[k].param = 0;
        d->lock[k].val = 0;
    }
}

/* a track of format 3 -> today's (by id up to P_SLDEPTH; P_E0.. moved) */
static void proj_trk_from_v3(proj_trk_t *d, const proj_trk_v3_t *s, int drum)
{
    uint32_t k, nc = PROJ_NP_V3 - 8u;
    proj_trk_plain(d);
    for (k = 0; k < P_E0_V24; k++)
        d->p[k] = k < nc ? s->p[k] : TP[k].def;
    for (k = 0; k < 8u; k++)
        d->p[P_E0_V24 + k] = s->p[nc + k];
    d->p[P_SSWING] = swing_from_v3(d->p[P_SSWING]);
    d->p[P_ASWING] = swing_from_v3(d->p[P_ASWING]);
    d->engine = drum ? 0u : s->engine;
    d->preset = drum ? 0u : s->preset;
    for (k = 0; k < NSTEP; k++) {
        if (drum)
            dstep_from8(&d->dstep[k], &s->step[k]);
        else
            step_from8(&d->step[k], &s->step[k]);
    }
}

/* a track of formats 1 and 2 -> format 3 (mapped by count, see the top) */
static void proj_trk_v2_to_v3(proj_trk_v3_t *d, const proj_trk_v2_t *s, int drum)
{
    uint32_t k, nc = PROJ_NP_V2 - 8u;
    for (k = 0; k < PROJ_NP_V3 - 8u; k++)
        d->p[k] = k < nc ? s->p[k] : TP[k].def;
    for (k = 0; k < 8u; k++)
        d->p[PROJ_NP_V3 - 8u + k] = s->p[nc + k];
    d->engine = drum ? 0u : s->engine;          /* (indices 0..7 as they were) */
    d->preset = drum ? 0u : s->preset;
    memcpy(d->step, s->step, sizeof d->step);
}

/* a format 4 project (n bytes in *v4) -> slot q as format 5: the same, no nudge, no lock, no condition, no FILTER */
static int proj_from_v4(project_t *q, const project_v4_t *v4, int n)
{
    uint32_t i;
    if (n != (int)sizeof *v4 || v4->magic != PROJ_MAGIC_V4 || v4->size != sizeof *v4 ||
        v4->sum != proj_hash(v4, sizeof *v4 - 4u))
        return 0;
    memset(q, 0, sizeof *q);
    q->magic = PROJ_MAGIC;
    q->size = sizeof *q;
    memcpy(q->g, v4->g, sizeof q->g);
    q->sel = v4->sel;
    for (i = 0; i < NTRK; i++) {
        uint32_t k;
        for (k = 0; k < P_E0_V24; k++)                 /* by id up to P_CHORD, the parameters added since: */
            q->t[i].p[k] = k < PROJ_NP_V4 - 8u ? v4->t[i].p[k] : TP[k].def;   /* their defaults; P_E0.. moved */
        for (k = 0; k < 8u; k++)
            q->t[i].p[P_E0_V24 + k] = v4->t[i].p[PROJ_NP_V4 - 8u + k];
        q->t[i].engine = v4->t[i].engine;
        q->t[i].preset = v4->t[i].preset;
        memcpy(q->t[i].step, v4->t[i].step, sizeof q->t[i].step);
        proj_trk_plain(&q->t[i]);
    }
    q->sum = proj_sum(q);
    return 1;
}

/* a format 3 project -> slot q as format 5 */
static void proj_from_v3_ok(project_t *q, const project_v3_t *v3)
{
    uint32_t i;
    memset(q, 0, sizeof *q);
    q->magic = PROJ_MAGIC;
    q->size = sizeof *q;
    proj_g_from_old(q->g, v3->g);
    q->sel = v3->sel;
    for (i = 0; i < NTRK; i++)
        proj_trk_from_v3(&q->t[i], &v3->t[i], i == TRK_DRUM);
    q->sum = proj_sum(q);
}
static int proj_from_v3(project_t *q, const project_v3_t *v3, int n)
{
    if (n != (int)sizeof *v3 || v3->magic != PROJ_MAGIC_V3 || v3->size != sizeof *v3 ||
        v3->sum != proj_hash(v3, sizeof *v3 - 4u))
        return 0;
    proj_from_v3_ok(q, v3);
    return 1;
}

static project_v3_t proj_v3_tmp;               /* (formats 1, 2: through format 3) */
/* a format 2 project (n bytes in *v2) -> slot q as format 5 */
static int proj_from_v2(project_t *q, const project_v2_t *v2, int n)
{
    project_v3_t *v3 = &proj_v3_tmp;
    uint32_t i;
    if (n != (int)sizeof *v2 || v2->magic != PROJ_MAGIC_V2 || v2->size != sizeof *v2 ||
        v2->sum != proj_hash(v2, sizeof *v2 - 4u))
        return 0;
    memset(v3, 0, sizeof *v3);
    memcpy(v3->g, v2->g, sizeof v3->g);
    v3->sel = v2->sel;
    for (i = 0; i < NTRK; i++)
        proj_trk_v2_to_v3(&v3->t[i], &v2->t[i], i == TRK_DRUM);
    proj_from_v3_ok(q, v3);
    return 1;
}

/* a format 1 project (n bytes in *v1) -> slot q as format 5: the instrument becomes track 1,
 * tracks 2..4 start empty (their sounds as at power-on) */
static int proj_from_v1(project_t *q, const project_v1_t *v1, int n)
{
    project_v3_t *v3 = &proj_v3_tmp;
    uint32_t i;
    if (n != (int)sizeof *v1 || v1->magic != PROJ_MAGIC_V1 || v1->size != sizeof *v1 ||
        v1->sum != proj_hash(v1, sizeof *v1 - 4u))
        return 0;
    memset(v3, 0, sizeof *v3);
    memcpy(v3->g, v1->g, sizeof v3->g);
    proj_trk_v2_to_v3(&v3->t[0], &v1->t, 0);
    proj_from_v3_ok(q, v3);
    for (i = 1; i < NTRK; i++) {               /* the other tracks: their defaults, no steps */
        uint32_t k;
        for (k = 0; k < PROJ_NP_V5; k++)
            q->t[i].p[k] = k >= P_E0_V24 ? ENGINES[trk_def_engine(i)]->edit[k - P_E0_V24].def : TP[k].def;
        q->t[i].engine = (uint8_t)trk_def_engine(i);
        q->t[i].preset = 0xFF;                 /* 0xFF: its default preset (project_load) */
        memset(q->t[i].step, 0, sizeof q->t[i].step);
        if (i != TRK_DRUM)
            for (k = 0; k < NSTEP; k++)
                q->t[i].step[k].time = ST_REST;
        proj_trk_plain(&q->t[i]);
    }
    q->sum = proj_sum(q);
    return 1;
}

/* n bytes of a stored project (any format) -> slot q as format 5; 0 = not a project */
static int proj_import(project_t *q, const void *b, int n)
{
    if (n == (int)sizeof *q && proj_ok((const project_t *)b)) {
        memcpy(q, b, sizeof *q);
        return 1;
    }
    return proj_from_v4(q, (const project_v4_t *)b, n) || proj_from_v3(q, (const project_v3_t *)b, n) ||
           proj_from_v2(q, (const project_v2_t *)b, n) || proj_from_v1(q, (const project_v1_t *)b, n);
}

/* ---- the extension record (SLOOP 2.5): what format 5 has no room for. PX_BYTES, as stored in flash (in the
 * rest of its project's header page: storage.c st_save_x), in RAM beside each song section and in a backup:
 *   0 PX_VER, 1 the per-track parameters a track (P_NX), 2 the globals (G_NX), 3 0
 *   4 per track P_AACC..P_CRATE, then G_EVOL..G_PROG, a byte each (their ranges fit int8)
 *   .. the step conditions of every track (seq.c CN_*), 5 bits each, track by track, LSB first
 * Another version, or none: defaults (the conditions: the project's fill bits). */
#define PX_VER 1u
#define PX_COND (NTRK * NSTEP * 5u / 8u)
#define PX_BYTES (4u + NTRK * P_NX + G_NX + PX_COND)
_Static_assert(PX_BYTES <= 224u, "the extension record fits a header page (storage.c ST_X_MAX)");
_Static_assert(CN_COUNT <= 32u, "a condition in 5 bits");
/* the song sections' extension records (proj_slot's). Not in .noinit (no room): after a warm reset a slot kept in
 * RAM takes its extension from flash (persist_boot) */
static uint8_t proj_xslot[4][PX_BYTES];

static uint32_t px_cond(const uint8_t *c, uint32_t i)  /* condition i (track * NSTEP + step) of the packed ones */
{
    uint32_t b = i * 5u, w = c[b >> 3];
    if ((b & 7u) > 3u)
        w |= (uint32_t)c[(b >> 3) + 1u] << 8;
    return (w >> (b & 7u)) & 31u;
}
static void px_cond_set(uint8_t *c, uint32_t i, uint32_t v)
{
    uint32_t b = i * 5u, w = c[b >> 3] | ((b & 7u) > 3u ? (uint32_t)c[(b >> 3) + 1u] << 8 : 0u);
    w = (w & ~(31u << (b & 7u))) | (v & 31u) << (b & 7u);
    c[b >> 3] = (uint8_t)w;
    if ((b & 7u) > 3u)
        c[(b >> 3) + 1u] = (uint8_t)(w >> 8);
}
static int px_ok(const uint8_t *x) { return x[0] == PX_VER && x[1] == P_NX && x[2] == G_NX; }

static void px_capture(uint8_t *x)             /* the working project's extension */
{
    uint32_t i, k;
    uint8_t *c = x + 4u + NTRK * P_NX + G_NX;
    memset(x, 0, PX_BYTES);
    x[0] = PX_VER;
    x[1] = P_NX;
    x[2] = G_NX;
    for (i = 0; i < NTRK; i++) {
        for (k = 0; k < P_NX; k++)
            x[4u + i * P_NX + k] = (uint8_t)lock_base(&trk[i], P_E0_V24 + k);   /* (a lock in force: the base) */
        for (k = 0; k < NSTEP; k++)
            px_cond_set(c, i * NSTEP + k, trk[i].cond[k]);
    }
    for (k = 0; k < G_NX; k++)
        x[4u + NTRK * P_NX + k] = (uint8_t)song.g[G_NG_V24 + k];
}

/* a backup's extension object (editor.c, objects 9..13): the sum of its project, then the record */
#define PX_KEYED (4u + PX_BYTES)
static uint32_t px_keyed(const project_t *p, const uint8_t *x, uint8_t *b)   /* its length, 0: nothing to keep */
{
    if (!proj_ok(p) || !px_ok(x))
        return 0;
    memcpy(b, &p->sum, 4);
    memcpy(b + 4, x, PX_BYTES);
    return PX_KEYED;
}
static const uint8_t *px_unkey(const project_t *p, const uint8_t *b, uint32_t n)   /* its record when it is p's, else 0 */
{
    uint32_t key;
    if (n != PX_KEYED || !px_ok(b + 4) || !proj_ok(p))
        return 0;
    memcpy(&key, b, 4);
    return key == p->sum ? b + 4 : 0;
}

/* a lock's parameter in the format (SLOOP 2.4's ids; a 2.5 parameter: past P_NP_V24) and back (P_COUNT: none) */
static uint32_t px_lock_out(uint32_t id)
{
    return id < P_E0_V24 ? id : id >= P_E0 ? id - P_E0 + P_E0_V24 : id - P_E0_V24 + PROJ_NP_V5;
}
static uint32_t px_lock_in(uint32_t id)
{
    return id < P_E0_V24 ? id : id < PROJ_NP_V5 ? id - P_E0_V24 + P_E0 : id < PROJ_NP_V5 + P_NX ? id - PROJ_NP_V5 + P_E0_V24
                                                                                                 : P_COUNT;
}

/* ---- the working project <-> a project_t and its extension */
static void proj_capture(project_t *p, uint8_t *x)   /* what is playing now, as a project (x: its extension, or 0) */
{
    uint32_t i, k;
    memset(p, 0, sizeof *p);
    p->magic = PROJ_MAGIC;
    p->size = sizeof *p;
    for (i = 0; i < PROJ_NG_V5; i++)
        p->g[i] = song.g[i];
    p->sel = song.sel;
    for (i = 0; i < NTRK; i++) {
        const track_t *t = &trk[i];
        proj_trk_t *d = &p->t[i];
        for (k = 0; k < P_E0_V24; k++)                 /* (a lock in force: the track's own value) */
            d->p[k] = lock_base(t, k);
        for (k = 0; k < 8u; k++)
            d->p[P_E0_V24 + k] = lock_base(t, P_E0 + k);
        d->engine = t->eng_req;
        d->preset = t->preset;
        memcpy(d->step, t->step, sizeof t->step);
        memcpy(d->micro, t->micro, sizeof t->micro);
        for (k = 0; k < NLOCK; k++) {
            d->lock[k] = t->lock[k];
            if (t->lock[k].step != LOCK_FREE)
                d->lock[k].param = (uint8_t)px_lock_out(t->lock[k].param);
        }
        for (k = 0; k < NSTEP; k++)                    /* FILL / !FILL as SLOOP 2.4's fill bits too */
            d->fill[k / 4u] |= (uint8_t)((t->cond[k] == CN_FILL ? FC_FILL : t->cond[k] == CN_NFILL ? FC_NOFILL : FC_NORM)
                                         << (2u * (k % 4u)));
    }
    p->sum = proj_sum(p);
    if (x)
        px_capture(x);
}

/* a project's tracks (and its globals, all: a load; or only the drum level / reverb: a song section) into the
 * working one, every value back inside its range; x: its extension (0, or not valid: defaults). The audio ISR
 * must not run meanwhile (the song sections: called from it; a load: IRQ off) */
static void proj_apply(const project_t *p, const uint8_t *x, int all)
{
    uint32_t i, k;
    const uint8_t *c;
    if (x && !px_ok(x))
        x = 0;
    c = x ? x + 4u + NTRK * P_NX + G_NX : 0;
    for (i = 0; i < G_COUNT; i++)
        if (all ? i != G_SLOT && i != G_LOAD && i != G_SAVE && i != G_SYNC && i != G_MIDI && i != G_ROUTE : i == G_DRLVL || i == G_DRREV)
            song.g[i] = (int16_t)clamp(i < PROJ_NG_V5 ? p->g[i] : x ? (int8_t)x[4u + NTRK * P_NX + i - PROJ_NG_V5] : GP[i].def,
                                       GP[i].min, GP[i].max);
    for (k = 0; k < NTRK; k++) {
        track_t *t = &trk[k];
        const proj_trk_t *s = &p->t[k];
        uint32_t e = k < NPART ? s->engine % NENGINES : 0u;
        t->eng_req = (uint8_t)e;
        t->user = 0;                                    /* (no user preset slot is saved) */
        t->lk_n = 0;                                    /* (the locks in force: the values come from the project) */
        t->tu_arm = 0;                                  /* TURN: the undo and the register of this pattern */
        for (i = 0; i < P_COUNT; i++) {                 /* every value back inside its range */
            const param_desc_t *d = k == TRK_DRUM && i == P_E0 ? &DRUM_KIT_DESC :   /* the drum kit */
                                    i >= P_E0 && i <= P_E7 ? &ENGINES[e]->edit[i - P_E0] : &TP[i];
            int32_t v = i < P_E0_V24 ? s->p[i] : i >= P_E0 ? s->p[i - P_E0 + P_E0_V24] :
                        x ? (int8_t)x[4u + k * P_NX + i - P_E0_V24] : TP[i].def;
            t->p[i] = (int16_t)clamp(v, d->min, d->max);
        }
        t->preset = (uint8_t)(ENGINES[e]->npresets ? (s->preset == 0xFFu ? 0u : s->preset) % ENGINES[e]->npresets : 0u);
        memcpy(t->step, s->step, sizeof t->step);
        for (i = 0; i < NSTEP; i++)                     /* the nudges and locks, each inside its range */
            t->micro[i] = (int8_t)clamp(s->micro[i], MICRO_MIN, MICRO_MAX);
        for (i = 0; i < NSTEP; i++) {                   /* the conditions: the extension's, else the fill bits */
            uint32_t f = (s->fill[i / 4u] >> (2u * (i % 4u))) & 3u, cn = c ? px_cond(c, k * NSTEP + i) : CN_ALWAYS;
            t->cond[i] = (uint8_t)(c ? (cn < CN_COUNT ? cn : CN_ALWAYS) : f == FC_FILL ? CN_FILL : f == FC_NOFILL ? CN_NFILL : CN_ALWAYS);
        }
        for (i = 0; i < NLOCK; i++) {
            const plock_t *l = &s->lock[i];
            uint32_t id = l->step < NSTEP ? px_lock_in(l->param) : P_COUNT;
            if (id < P_COUNT && p_lockable(id)) {
                const param_desc_t *d = k == TRK_DRUM && id == P_E0 ? &DRUM_KIT_DESC :
                                        id >= P_E0 && id <= P_E7 ? &ENGINES[e]->edit[id - P_E0] : &TP[id];
                t->lock[i].step = l->step;
                t->lock[i].param = (uint8_t)id;
                t->lock[i].val = (int16_t)clamp(l->val, d->min, d->max);
            } else {
                t->lock[i].step = LOCK_FREE;            /* no such step or parameter: the slot is free */
                t->lock[i].param = 0;
                t->lock[i].val = 0;
            }
        }
        if (k != TRK_DRUM)
            for (i = 0; i < NSTEP; i++) {
                step_t *st = &t->step[i];
                uint32_t j;
                if (st->n > 4u)
                    st->n = 4;
                if (st->time > ST_REST)
                    st->time = ST_REST;
                for (j = 0; j < 4u; j++)
                    st->note[j] &= 127u;
            }
        fm6_track_loaded(t);                            /* FM6: the project keeps PTCH, not the patch: its slot's */
    }
}

#ifndef PROJ_HOST
#if FELUCCA_ARRANGER
#include "arranger_scene.c"
#endif
static uint8_t sec_dirty, song_dirty;           /* live sections / the song: in RAM, not yet in flash */
#if FELUCCA_FLASH
/* slot from flash into RAM (format 5, or an old one converted) */
static union {
    project_t v5;
    project_v4_t v4;
    project_v3_t v3;
    project_v2_t v2;
    project_v1_t v1;
} proj_tmp;
static uint8_t proj_tmp_x[PX_BYTES];            /* .. and its extension */
/* object obj (a project) into q, its extension into x (none: zeroes, not valid); 0 = not a project */
static int proj_load_obj(uint32_t obj, project_t *q, uint8_t *x)
{
    uint32_t xn;
    int n = st_load_x(obj, &proj_tmp, sizeof proj_tmp, proj_tmp_x, sizeof proj_tmp_x, &xn);
    memset(x, 0, PX_BYTES);
    if (!proj_import(q, &proj_tmp, n))
        return 0;
    if (n == (int)sizeof *q && xn == PX_BYTES)       /* (an old format converted: no extension) */
        memcpy(x, proj_tmp_x, PX_BYTES);
    return 1;
}
static void proj_fetch(uint32_t slot)
{
    if (!proj_load_obj(OBJ_PROJECT0 + (slot & 3u), &proj_slot[slot & 3u], proj_xslot[slot & 3u]))
        proj_slot[slot & 3u].magic = 0;
}
/* a project and its extension into object obj (a valid extension only) */
static int proj_save_obj(uint32_t obj, const project_t *p, const uint8_t *x)
{
    return st_save_x(obj, p, sizeof *p, x, px_ok(x) ? PX_BYTES : 0u);
}
#include "fm6_bank.c"                           /* the FM6 patch bank (eng_fm6.c PTCH B1..B27): staged in proj_tmp */
#endif

static void project_save(uint32_t slot)
{
    project_t *p = &proj_slot[slot & 3u];
#if FELUCCA_ARRANGER
    if (song.playing || transport_req) { ui_message("STOP BEFORE SAVE"); return; }
#endif
    proj_capture(p, proj_xslot[slot & 3u]);
#if FELUCCA_FLASH
    if (flash_ok) {
        ui_message(proj_save_obj(OBJ_PROJECT0 + (slot & 3u), p, proj_xslot[slot & 3u]) ? "SAVE ERROR" : "SAVED");
        return;
    }
#endif
    ui_message("SAVED (RAM)");
}

/* a project (and its extension x) into the working one: the transport stops, everything sounding is released */
static void project_apply(const project_t *p, const uint8_t *x)
{
    uint32_t k;
    transport_req = 2;
    panic_req = (1u << NTRK) - 1u;
    fm1_irq_off();                                      /* the audio ISR must not see half a project */
    proj_apply(p, x, 1);
    song.sel = (uint8_t)(p->sel < NTRK ? p->sel : 0u);
    fm1_irq_on();
    for (k = 0; k < NPART; k++)                         /* a format 1 project: the default sounds of tracks 2, 3 */
        if (p->t[k].preset == 0xFFu) {
            apply_preset_to(&trk[k], TRK_DEF[k][1]);
            steps_clear(&trk[k]);
        }
    sync_reload = 1;
    ui.force = 1;
}

static void project_load(uint32_t slot)
{
    project_t *p = &proj_slot[slot & 3u];
#if FELUCCA_ARRANGER
    if (song.playing || transport_req) { ui_message("STOP BEFORE LOAD"); return; }
#endif
#if FELUCCA_FLASH
    if (flash_ok && !proj_ok(p))
        proj_fetch(slot);
#endif
    if (!proj_ok(p)) {
        ui_message("EMPTY SLOT");
        return;
    }
    project_apply(p, proj_xslot[slot & 3u]);
    ui_message("LOADED");
}

/* ---- the working project, kept in flash by itself: saved when it changed, the transport is stopped,
 * nothing sounds and the panel was not touched for AUTOSAVE_IDLE (a flash erase stops the audio for
 * ~50 ms: never while something plays); loaded at power-on (autosave_resume) */
#define AUTOSAVE_IDLE 2500u                    /* ms without input */
#define AUTOSAVE_GAP 20000u                    /* ms between two saves at least */
static project_t autosave_buf __attribute__((section(".pool")));
static uint8_t autosave_x[PX_BYTES];             /* .. its extension */
static uint32_t autosave_hash, autosave_ms, autosave_checked;
static uint32_t autosave_sum(void) { return autosave_buf.sum ^ proj_hash(autosave_x, PX_BYTES) * 31u; }

static int audio_quiet(void)
{
    uint32_t p, i;
    for (p = 0; p < NPART; p++)
        for (i = 0; i < NVOICE; i++)
            if (trk[p].v[i].active)
                return 0;
    for (i = 0; i < NDRUM; i++)
        if (drums.v[i].active)
            return 0;
    return 1;
}

static void autosave_tick(void)                /* main loop */
{
#if FELUCCA_FLASH
    uint32_t h, now = fm1_ms;
    if (!flash_ok || song.playing || transport_req || rec_wait || ft_on || ui.menu ||
        now - ui_input_ms < AUTOSAVE_IDLE || now - autosave_ms < AUTOSAVE_GAP || now - autosave_checked < 1000u)
        return;
    autosave_checked = now;
    proj_capture(&autosave_buf, autosave_x);
    h = autosave_sum();
    if (h == autosave_hash || !audio_quiet())
        return;
    if (proj_save_obj(OBJ_AUTOSAVE, &autosave_buf, autosave_x) == 0)
        autosave_hash = h;
    autosave_ms = fm1_ms;
#endif
}

static void autosave_resume(void)              /* power-on: the project as it was left (felucca_init) */
{
#if FELUCCA_FLASH
    project_t *q = &autosave_buf;
    int n;
    if (!flash_ok || !proj_load_obj(OBJ_AUTOSAVE, q, autosave_x))
        return;
    autosave_hash = autosave_sum();
    proj_apply(q, autosave_x, 1);
    song.sel = (uint8_t)(q->sel < NTRK ? q->sel : 0u);
    for (n = 0; n < NPART; n++)
        trk[n].engine = trk[n].eng_req;        /* (nothing sounds yet: no fade) */
#endif
}

/* settings + learned panel table: one flash object. The flash copy wins at
 * boot (the .noinit copies are garbage after a power-off). */
typedef struct {
    uint32_t magic, palette, lowcut, zoom;
    panel_t panel;
#if FELUCCA_ARRANGER
    arr_config_t arrangement;
#endif
    uint32_t lights;                               /* SLOOP 2.3: the backlight (panel.c lights_word); appended,
                                                    * so 2.2 still reads its part (st_load cuts at its size) */
} persist_t;
#define PERSIST_SIZE_V22 __builtin_offsetof(persist_t, lights)   /* the settings as 2.2 wrote them (no lights) */
_Static_assert(sizeof(persist_t) == PERSIST_SIZE_V22 + 4u, "lights: the last word, no padding before it");
#if FELUCCA_ARRANGER
#define PERSIST_MAGIC 0x50455233u                  /* "PER3": includes the song order */
#else
#define PERSIST_MAGIC 0x50455232u
#endif
#if FELUCCA_FLASH
static persist_t persist_saved;
#endif

static void persist_boot(void)                    /* before settings_init / panel_init */
{
#if FELUCCA_ARRANGER
    arr_defaults(&arrangement);
#endif
#if FELUCCA_FLASH
    persist_t p;
    uint32_t f = irq_save();
    flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;       /* the expected 1 MiB part, else stay RAM-only */
    irq_restore(f);
    if (!flash_ok)
        return;
    fl_plain_window_init();                        /* flash above 0x93000 reads as plaintext through XIP
                                                    * (user sample sets are played from there) */
    {
        uint32_t k;
        for (k = 0; k < SMP_USER_SLOTS; k++)
            smp_user_scan(k);
    }
    {
        int n = st_load(OBJ_SETTINGS, &p, sizeof p);
        if (n == (int)PERSIST_SIZE_V22 && p.magic == PERSIST_MAGIC)
            p.lights = 0;                          /* from 2.2: backlight off */
        if (((n == (int)sizeof p || n == (int)PERSIST_SIZE_V22) && p.magic == PERSIST_MAGIC)
#if FELUCCA_ARRANGER
            || (n == (int)(16u + sizeof(panel_t)) && p.magic == 0x50455232u)
#endif
            ) {
            settings.magic = SETTINGS_MAGIC;
            settings.palette = p.palette;
            settings.lowcut = p.lowcut;
            settings.zoom = p.zoom;
            if (p.panel.magic == PANEL_MAGIC)
                panel = p.panel;
            if (p.magic == PERSIST_MAGIC)
                lights_from_word(p.lights);
            else
                p.lights = 0;
#if FELUCCA_ARRANGER
            if (p.magic == PERSIST_MAGIC && arr_valid(&p.arrangement, 15u))
                arrangement = p.arrangement;
            else
                p.arrangement = arrangement;
#endif
            persist_saved = p;
        } else if (n == (int)(8u + sizeof(panel_t)) && p.magic == 0x50455231u) {   /* "PER1": palette, panel */
            const uint32_t *w = (const uint32_t *)&p;
            panel_t old;
            memcpy(&old, w + 2, sizeof old);
            settings.magic = SETTINGS_MAGIC;
            settings.palette = w[1];
            settings.lowcut = 0;
            settings.zoom = 0;
            if (old.magic == PANEL_MAGIC)
                panel = old;
        }
    }
    {   /* projects: fill empty RAM slots from flash, so the slot list is right after power-on. A slot
         * still valid in RAM (a warm reset: an update, UPDATE MODE, a crash) may never have reached
         * flash (a live section stored while playing): marked to be written when quiet. Its extension
         * (not kept over a reset): flash's when the project there is the same, else none (defaults) */
        uint32_t i;
        for (i = 0; i < 4u; i++)
            if (!proj_ok(&proj_slot[i])) {
                proj_fetch(i);
            } else {
                uint32_t xn;
                int n = st_load_x(OBJ_PROJECT0 + i, &proj_tmp, sizeof proj_tmp, proj_tmp_x, sizeof proj_tmp_x, &xn);
                memset(proj_xslot[i], 0, PX_BYTES);
                if (n != (int)sizeof proj_slot[i] || memcmp(&proj_tmp.v5, &proj_slot[i], sizeof proj_slot[i]))
                    sec_dirty |= (uint8_t)(1u << i);
                else if (xn == PX_BYTES)
                    memcpy(proj_xslot[i], proj_tmp_x, PX_BYTES);
            }
    }
    up_boot();                                     /* user presets */
    fm6_bank_boot();                               /* the FM6 patch bank (fm6_bank.c) */
#endif
}

static int project_used(uint32_t slot) { return proj_ok(&proj_slot[slot & 3u]); }

static void persist_fill(persist_t *p)              /* the settings as they are now */
{
    memset(p, 0, sizeof *p);
    p->magic = PERSIST_MAGIC;
    p->palette = settings.palette;
    p->lowcut = settings.lowcut;
    p->zoom = settings.zoom;
    p->panel = panel;
    p->lights = lights_word();
#if FELUCCA_ARRANGER
    p->arrangement = arrangement;
#endif
}

static void settings_save(void)
{
#if FELUCCA_FLASH
    persist_t p;
    if (!flash_ok)
        return;
    persist_fill(&p);
    if (!memcmp(&p, &persist_saved, sizeof p))
        return;                                    /* unchanged: no erase cycle */
    if (st_save(OBJ_SETTINGS, &p, sizeof p) == 0)
        persist_saved = p;
#endif
}

#if FELUCCA_FLASH
_Static_assert(sizeof(project_t) <= ST_PAYLOAD_MAX, "project does not fit one flash sector");

/* ---- backup restore (editor.c BK_PUT): each object checked as a load checks it, then written through the
 * same A/B commit as a save. rc: 0 ok, 2 not a valid object, 3 stop the song first, 4 flash */
static int panel_valid(const panel_t *q)           /* a permutation of the buttons and of the knobs */
{
    uint32_t i, b = 0, e = 0;
    if (q->magic != PANEL_MAGIC)
        return 0;
    for (i = 0; i < NB; i++) {
        if (q->btn[i] >= 14u || (b >> q->btn[i]) & 1u)
            return 0;
        b |= 1u << q->btn[i];
    }
    for (i = 0; i < NE; i++) {
        if (q->enc[i] >= 7u || (e >> q->enc[i]) & 1u || (q->dir[i] != 1 && q->dir[i] != -1))
            return 0;
        e |= 1u << q->enc[i];
    }
    return 1;
}

static uint32_t settings_restore(const void *raw, uint32_t n)
{
    persist_t p;
    if (n != sizeof p && n != PERSIST_SIZE_V22)
        return 2;
    memset(&p, 0, sizeof p);
    memcpy(&p, raw, n);
    if (p.magic != PERSIST_MAGIC || p.palette >= NPALETTES || p.lowcut > 1u || p.zoom > 1u || !panel_valid(&p.panel))
        return 2;
#if FELUCCA_ARRANGER
    if (!arr_valid(&p.arrangement, 15u))
        return 2;
#endif
    if (!flash_ok || st_save(OBJ_SETTINGS, &p, sizeof p))
        return 4;
    persist_saved = p;
    settings.palette = p.palette;
    settings.lowcut = p.lowcut;
    settings.zoom = p.zoom;
    panel = p.panel;
#if FELUCCA_ARRANGER
    arrangement = p.arrangement;
#endif
    lights_from_word(p.lights);
    song.g[G_SYNC] = (int16_t)lights_sync;
    song.g[G_MIDI] = (int16_t)lights_mout;
    song.g[G_ROUTE] = (int16_t)lights_min;
    palette_set(settings.palette);
    fx_lowcut = (uint8_t)(settings.lowcut != 0);
    ui.force = 1;
    return 0;
}

/* slot 0..3 (n 0: empty), or 4: the working project (loaded now). A project comes back without its extension
 * (defaults: a backup of SLOOP 2.4); the extension's own object follows it (project_restore_x) */
static uint32_t project_restore(uint32_t slot, const void *raw, uint32_t n)
{
    if (song.playing || transport_req)
        return 3;
    if (slot < 4u && !n) {
        if (!flash_ok || st_save(OBJ_PROJECT0 + slot, raw, 0))
            return 4;
        memset(&proj_slot[slot], 0, sizeof proj_slot[slot]);
        memset(proj_xslot[slot], 0, PX_BYTES);
        sec_dirty &= (uint8_t)~(1u << slot);
        return 0;
    }
    if (!proj_import(&autosave_buf, raw, (int)n))
        return 2;
    memset(autosave_x, 0, PX_BYTES);
    if (slot == 4u) {
        project_apply(&autosave_buf, 0);
        return 0;
    }
    if (!flash_ok || st_save(OBJ_PROJECT0 + slot, &autosave_buf, sizeof autosave_buf))
        return 4;
    memcpy(&proj_slot[slot], &autosave_buf, sizeof proj_slot[slot]);
    memset(proj_xslot[slot], 0, PX_BYTES);
    sec_dirty &= (uint8_t)~(1u << slot);
    return 0;
}
/* the extension of slot 0..3, or 4: of the working project, from a backup (px_keyed): only onto that project
 * (restored just before it); n 0: nothing to do */
static uint32_t project_restore_x(uint32_t slot, const uint8_t *raw, uint32_t n)
{
    const project_t *p = slot < 4u ? &proj_slot[slot] : &autosave_buf;
    const uint8_t *x;
    if (song.playing || transport_req)
        return 3;
    if (!n)
        return 0;
    if (!(x = px_unkey(p, raw, n)))
        return 2;
    if (slot == 4u) {
        project_apply(p, x);
        return 0;
    }
    if (!flash_ok || proj_save_obj(OBJ_PROJECT0 + slot, p, x))
        return 4;
    memcpy(proj_xslot[slot], x, PX_BYTES);
    return 0;
}
/* a backup's extension object of slot 0..3 (0 bytes: an empty slot, or none) into b */
static uint32_t project_backup_x(uint32_t slot, uint8_t *b) { return px_keyed(&proj_slot[slot & 3u], proj_xslot[slot & 3u], b); }
#endif
#if FELUCCA_ARRANGER
static void arrangement_save(void)
{
    if (song.playing || transport_req) { ui_message("STOP BEFORE SAVE"); return; }
    settings_save();
#if FELUCCA_FLASH
    if (flash_ok) {
        ui_message(memcmp(&persist_saved.arrangement, &arrangement, sizeof arrangement) ? "SAVE ERROR" : "SONG SAVED");
        return;
    }
#endif
    ui_message("SONG IN RAM ONLY");
}

/* ---- live sections (SAVE + key, ui_layers.c). A section is a project slot (A..D = 1..4): stored into RAM
 * at once (playing too), written to flash once the transport is stopped and nothing sounds (an erase
 * stops the audio for ~50 ms); a song recorded with SONG REC is saved the same way. */
static void section_store(uint32_t s)
{
    s &= 3u;
    fm1_irq_off();                                      /* (the audio ISR may be applying a section) */
    proj_capture(&proj_slot[s], proj_xslot[s]);
    live_sec = (int8_t)s;
    fm1_irq_on();
    sec_dirty |= (uint8_t)(1u << s);
}
static void section_load(uint32_t s)                    /* stopped: the section is the loop now */
{
    s &= 3u;
    project_apply(&proj_slot[s], proj_xslot[s]);
    live_sec = (int8_t)s;
}
static void sections_write(void)                        /* the dirty sections and song into flash */
{
    uint32_t i;
#if FELUCCA_FLASH
    if (flash_ok)
        for (i = 0; i < 4u; i++)
            if (((sec_dirty >> i) & 1u) && proj_save_obj(OBJ_PROJECT0 + i, &proj_slot[i], proj_xslot[i]) == 0)
                sec_dirty &= (uint8_t)~(1u << i);       /* (a failed write stays dirty: tried again later) */
    if (!flash_ok)
#endif
        sec_dirty = 0;
    (void)i;
    if (song_dirty) {
        song_dirty = 0;
        settings_save();
    }
}
/* before an intentional reset (an update, UPDATE MODE, UBOOT from the host): the audio is stopped, so
 * whatever is only in RAM goes to flash now: the live sections, the song, the working project */
static void persist_flush_now(void)
{
    sections_write();
#if FELUCCA_FLASH
    if (flash_ok && !arrangement_clock.running) {     /* (a song playing: the tracks hold a section) */
        proj_capture(&autosave_buf, autosave_x);
        if (autosave_sum() != autosave_hash && proj_save_obj(OBJ_AUTOSAVE, &autosave_buf, autosave_x) == 0)
            autosave_hash = autosave_sum();
    }
#endif
}
static void sections_flush(void)                        /* main loop */
{
    static uint32_t tried;
    if (srec_done) {
        song_dirty = srec_done != 0xFFu;
        if (song_dirty) {
            char b[8];
            fmt_int(b, srec_done);
            ui_say("SONG PARTS ", b);
        } else {
            ui_message("NO SONG");
        }
        srec_done = 0;
    }
    if ((uint32_t)song.g[G_SYNC] != lights_sync) {      /* GLO > SYSTEM > SYNC: kept with the settings */
        lights_sync = (uint8_t)song.g[G_SYNC];
        settings_later = 1;
    }
    if ((uint32_t)(song.g[G_MIDI] != 0) != lights_mout) {   /* GLO > SYSTEM > MIDI: the same */
        lights_mout = (uint8_t)(song.g[G_MIDI] != 0);
        settings_later = 1;
    }
    if ((uint32_t)(song.g[G_ROUTE] != 0) != lights_min) {   /* GLO > SYSTEM > IN: the same */
        lights_min = (uint8_t)(song.g[G_ROUTE] != 0);
        settings_later = 1;
    }
    if (settings_later) {                               /* the menu closed while playing */
        settings_later = 0;
        song_dirty = 1;                                 /* (settings_save when quiet, with the song) */
    }
    if ((!sec_dirty && !song_dirty) || song.playing || transport_req || !audio_quiet() || fm1_ms - ui_input_ms < 1500u ||
        fm1_ms - tried < 5000u)
        return;
    tried = fm1_ms;                                     /* (a failed write: again in 5 s, not every frame) */
    sections_write();
    if (sec_dirty)
        ui_message("SAVE ERROR: RETRYING");
}
#endif
#endif /* PROJ_HOST */
