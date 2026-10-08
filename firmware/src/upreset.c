/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* User presets (editor protocol v2, cmds 16-21; the SAVE > USER page): 32
 * slots in two storage.c objects (OBJ_UPRESET0/1, 0xDC000..0xDFFFF), 16
 * records each, mirrored in RAM so browsing never reads flash. A record:
 * engine, name, the instrument parameters, a 16-step pattern.
 *
 * A bank whose magic, record size or slot count differ reads as empty; so
 * does a record with another UP_VER. A record keeps np = the P_COUNT it was
 * stored with; when that differs it is mapped by count: its last 8 values
 * are P_E0..P_E7, the first np - 8 are P_LEVEL.. in order, missing ones take
 * their defaults. So common parameters may only be added just before P_E0
 * (else bump UP_VER).
 *
 * SLOOP 2.5 stores SLOOP 2.4's layout (np = P_NP_V24: P_LEVEL..P_VLEAD, P_E0..P_E7), and its own
 * parameters (P_AACC..P_CRATE, P_NX: core.h) a byte each after them, tagged in the last value
 * (UP_XTAG | P_NX); 2.4 reads the first np and leaves the rest. A record with no tag: their defaults.
 *
 * With -DUP_HOST (host test) only the part above #ifndef UP_HOST is built;
 * it needs nothing but core.h. */
#define UP_PER_BANK 16u
#define UP_PMAX 72u                              /* room for P_COUNT to grow */
#define UP_USED 0xA5u
#define UP_VER 1u
#define UP_BANK_MAGIC 0x31425055u                /* "UPB1" */
typedef struct {
    uint8_t used, ver, engine, np;               /* UP_USED, UP_VER, engine, P_COUNT when stored */
    char name[12];                               /* ASCII 32..126, 0-padded (no 0 when 12 long) */
    int16_t p[UP_PMAX];
    uint8_t note[16], flags[16];                 /* note 0 = rest; flags 1 accent, 2 slide, 4 tie */
} up_rec_t;
typedef struct {
    uint32_t magic;
    uint16_t rsize, nslot;
    up_rec_t r[UP_PER_BANK];
} up_bank_t;
_Static_assert(sizeof(up_rec_t) == 192, "user preset record layout");
#define UP_XTAG 0x5800                           /* p[UP_PMAX - 1]: 'X' << 8 | the bytes packed */
_Static_assert(P_NP_V24 + (P_NX + 1u) / 2u < UP_PMAX && P_COUNT < 128, "user preset record: P_COUNT");
static up_bank_t up_bank[UP_SLOTS / UP_PER_BANK];

static up_rec_t *up_rec(uint32_t k) { return &up_bank[k / UP_PER_BANK].r[k % UP_PER_BANK]; }

static int up_valid(const up_rec_t *r)
{
    return r->used == UP_USED && r->ver == UP_VER && r->engine < NENGINES && r->np >= 8u && r->np <= UP_PMAX &&
           r->name[0];
}

static int up_used(uint32_t k) { return k < UP_SLOTS && up_valid(up_rec(k)); }

static void up_bank_check(uint32_t b, int len)  /* after loading bank b (len bytes, -1 = none): wrong shape -> empty */
{
    up_bank_t *bk = &up_bank[b];
    if (len != (int)sizeof *bk || bk->magic != UP_BANK_MAGIC || bk->rsize != sizeof(up_rec_t) ||
        bk->nslot != UP_PER_BANK)
        memset(bk, 0, sizeof *bk);
}

/* the record's values in today's P_* order (mapped by count, see above); def = the defaults */
static void up_params(const up_rec_t *r, int16_t *out, const int16_t *def)
{
    uint32_t i, nc = r->np - 8u, nx = (uint16_t)r->p[UP_PMAX - 1u] & 0xFFu;
    for (i = 0; i < P_E0; i++)
        out[i] = i < nc ? r->p[i] : def[i];
    for (i = 0; i < 8u; i++)
        out[P_E0 + i] = r->p[nc + i];
    if (r->np == P_NP_V24 && ((uint16_t)r->p[UP_PMAX - 1u] & 0xFF00u) == UP_XTAG)   /* SLOOP 2.5's, packed */
        for (i = 0; i < P_NX && i < nx; i++)
            out[P_E0_V24 + i] = (int8_t)((uint16_t)r->p[P_NP_V24 + i / 2u] >> (8u * (i & 1u)));
}
/* today's values (P_COUNT) -> the record: SLOOP 2.4's layout, SLOOP 2.5's parameters packed after it */
static void up_rec_set(up_rec_t *r, const int16_t *v)
{
    uint32_t i;
    r->np = P_NP_V24;
    memset(r->p, 0, sizeof r->p);
    for (i = 0; i < P_E0_V24; i++)
        r->p[i] = v[i];
    for (i = 0; i < 8u; i++)
        r->p[P_E0_V24 + i] = v[P_E0 + i];
    for (i = 0; i < P_NX; i++) {
        int32_t x = v[P_E0_V24 + i];
        x = x < -128 ? -128 : x > 127 ? 127 : x;      /* (their ranges fit a byte; the editor's raw values may not) */
        r->p[P_NP_V24 + i / 2u] = (int16_t)((uint16_t)r->p[P_NP_V24 + i / 2u] | (uint16_t)(uint8_t)x << (8u * (i & 1u)));
    }
    r->p[UP_PMAX - 1u] = (int16_t)(UP_XTAG | P_NX);
}

static int up_name_ok(const uint8_t *s, uint32_t n)   /* 1..12 printable ASCII */
{
    uint32_t i;
    if (!n || n > 12u)
        return 0;
    for (i = 0; i < n; i++)
        if (s[i] < 32u || s[i] > 126u)
            return 0;
    return 1;
}

static void up_name(uint32_t k, char *b)       /* upper case, 0-terminated: b holds 13 */
{
    const up_rec_t *r = up_rec(k);
    uint32_t i;
    for (i = 0; i < 12u && r->name[i]; i++)
        b[i] = r->name[i] >= 'a' && r->name[i] <= 'z' ? (char)(r->name[i] - 32) : r->name[i];
    b[i] = 0;
}

static void up_pat_norm(uint8_t *note, uint8_t *flags)   /* tie: no note; rest: no flags */
{
    *note &= 127u;
    if (*flags & 4u) {
        *note = 0;
        *flags = 4;
    } else {
        *flags = *note ? (uint8_t)(*flags & (SF_ACCENT | SF_SLIDE)) : 0u;
    }
}

static void up_pat_from(up_rec_t *r, const step_t *st)   /* the first 16 steps -> the pattern */
{
    uint32_t i;
    for (i = 0; i < 16u; i++) {
        r->note[i] = st[i].time == ST_NOTE && st[i].n ? st[i].note[0] : 0u;
        r->flags[i] = st[i].time == ST_TIE ? 4u : st[i].flags;
        up_pat_norm(&r->note[i], &r->flags[i]);
    }
}

static int up_pat_empty(const up_rec_t *r)
{
    uint32_t i;
    for (i = 0; i < 16u; i++)
        if (r->note[i])
            return 0;
    return 1;
}

/* UP_PUT arguments: slot, engine, name, P_COUNT x v14, 16 x (note, flags) -> *r (values not yet
 * clamped); 0 ok, 1 bad arguments. *slot gets the slot byte when there is one. */
static int up_parse(const uint8_t *a, uint32_t na, up_rec_t *r, uint32_t *slot)
{
    uint32_t i, n, k;
    if (na < 3u)
        return 1;
    *slot = a[0];
    for (n = 0; 2u + n < na && a[2 + n]; n++)
        ;
    k = 3u + n;                                  /* after the name's 0 */
    if (a[0] >= UP_SLOTS || a[1] >= NENGINES || 2u + n >= na || !up_name_ok(a + 2, n) ||
        na < k + 2u * P_COUNT + 32u)
        return 1;
    memset(r, 0, sizeof *r);
    r->used = UP_USED;
    r->ver = UP_VER;
    r->engine = a[1];
    for (i = 0; i < n; i++)
        r->name[i] = (char)a[2 + i];
    {
        int16_t v[P_COUNT];
        for (i = 0; i < P_COUNT; i++, k += 2u)
            v[i] = (int16_t)((int32_t)((a[k] & 127u) | (a[k + 1] & 127u) << 7) - 8192);
        up_rec_set(r, v);
    }
    for (i = 0; i < 16u; i++, k += 2u) {
        r->note[i] = a[k];
        r->flags[i] = a[k + 1];
        up_pat_norm(&r->note[i], &r->flags[i]);
    }
    return 0;
}

#ifndef UP_HOST
static const param_desc_t *up_desc(uint32_t e, uint32_t i)
{
    return i >= P_E0 ? &ENGINES[e]->edit[i - P_E0] : &TP[i];
}

static void up_values(const up_rec_t *r, int16_t *v)   /* mapped and clamped for its engine */
{
    int16_t def[P_COUNT];
    uint32_t i;
    for (i = 0; i < P_COUNT; i++)
        def[i] = up_desc(r->engine, i)->def;
    up_params(r, v, def);
    for (i = 0; i < P_COUNT; i++)
        v[i] = (int16_t)clamp(v[i], up_desc(r->engine, i)->min, up_desc(r->engine, i)->max);
}

static void up_boot(void)                      /* persist_boot: the banks from flash */
{
#if FELUCCA_FLASH
    uint32_t b;
    for (b = 0; b < UP_SLOTS / UP_PER_BANK; b++)
        up_bank_check(b, flash_ok ? st_load(OBJ_UPRESET0 + b, &up_bank[b], sizeof up_bank[b]) : -1);
#endif
}

/* record k = *r (0: erase), then the bank to flash: 0 ok, 2 flash error, 3 no flash (kept in RAM) */
static int up_put(uint32_t k, const up_rec_t *r)
{
    up_bank_t *bk = &up_bank[k / UP_PER_BANK];
    bk->magic = UP_BANK_MAGIC;
    bk->rsize = sizeof(up_rec_t);
    bk->nslot = UP_PER_BANK;
    if (r)
        *up_rec(k) = *r;
    else
        memset(up_rec(k), 0, sizeof(up_rec_t));
    if (!r) {
        uint32_t i;
        for (i = 0; i < NTRK; i++)
            if (trk[i].user == k + 1u)
                trk[i].user = 0;
    }
    up_gen++;
#if FELUCCA_FLASH
    if (flash_ok)
        return st_save(OBJ_UPRESET0 + k / UP_PER_BANK, bk, sizeof *bk) ? 2 : 0;
#endif
    return 3;
}

static void up_slot_label(char *b, uint32_t k)  /* "U07" */
{
    b[0] = 'U';
    b[1] = (char)('0' + (k + 1u) / 10u);
    b[2] = (char)('0' + (k + 1u) % 10u);
    b[3] = 0;
}

/* the selected part's sound -> slot k; name 0 or "": engine name + slot number ("ANALOG 07").
 * 1 = the drum track is selected (it has no sound to store) */
static int up_store(uint32_t k, const char *name)
{
    up_rec_t r;
    uint32_t i;
    if (is_drum(TSEL))
        return 1;
    memset(&r, 0, sizeof r);
    r.used = UP_USED;
    r.ver = UP_VER;
    r.engine = TSEL->eng_req;
    if (name && name[0]) {
        for (i = 0; i < 12u && name[i]; i++)
            r.name[i] = name[i];
    } else {
        char b[16], l[4];
        str_cpy(b, ENGINES[TSEL->eng_req]->name, 9);
        up_slot_label(l, k);
        str_cpy(b + str_len(b), " ", 2);
        str_cpy(b + str_len(b), l + 1, 3);
        for (i = 0; i < 12u && b[i]; i++)
            r.name[i] = b[i];
    }
    up_rec_set(&r, TSEL->p);
    up_pat_from(&r, TSEL->step);
    return up_put(k, &r);
}

/* slot k -> the selected part's sound: engine and every parameter except its mix (LEVEL,
 * PAN, MUTE: the TRACKS faders) and its pattern parameters (param_kept). LIVE: the pattern
 * stored in the record is not loaded: changing the sound never changes the sequence.
 * 0 ok, 1 empty (or the drum track is selected) */
static int up_load(uint32_t k)
{
    const up_rec_t *r;
    int16_t v[P_COUNT];
    uint32_t i;
    track_t *t = TSEL;
    if (!up_used(k) || is_drum(t))
        return 1;
    r = up_rec(k);
    up_values(r, v);
    for (i = 0; i < P_COUNT; i++)                       /* (LEN etc. of a kept pattern changed too, and */
        if (param_kept(i))                              /* a preset pattern then counted as edited) */
            v[i] = t->p[i];
    panic_req |= (uint8_t)(1u << song.sel);
    fm1_irq_off();                                      /* the audio ISR must not see half a sound */
    t->eng_req = r->engine;
    for (i = 0; i < P_COUNT; i++)
        t->p[i] = v[i];
    t->preset = 0;
    fm1_irq_on();
    fm6_track_loaded(t);                                /* FM6: a user preset holds the PTCH and the macros */
    t->user = (uint8_t)(k + 1u);
    sync_reload = 1;
    ui.force = 1;
    return 0;
}

static uint32_t up_count(void)                 /* used slots */
{
    uint32_t k, n = 0;
    for (k = 0; k < UP_SLOTS; k++)
        n += (uint32_t)up_used(k);
    return n;
}

static uint32_t up_nth(uint32_t n)             /* slot of the n-th used one (n < up_count()) */
{
    uint32_t k;
    for (k = 0; k < UP_SLOTS; k++)
        if (up_used(k) && !n--)
            return k;
    return 0;
}

static uint32_t up_rank(uint32_t slot)         /* used slots before it */
{
    uint32_t k, n = 0;
    for (k = 0; k < slot && k < UP_SLOTS; k++)
        n += (uint32_t)up_used(k);
    return n;
}

/* SAVE > USER page actions, with the message in the top bar */
static void up_ui(uint32_t op, uint32_t k)     /* 0 load, 1 erase, 2 save */
{
    char l[4];
    int rc;
    up_slot_label(l, k);
    if (op != 1u && is_drum(TSEL)) {
        ui_message("DRUM TRACK: NO SOUND");
        return;
    }
    if (op < 2u && !up_used(k)) {
        ui_message("EMPTY SLOT");
        return;
    }
    if (op == 0u) {
        up_load(k);
        ui_say("LOADED ", l);
        return;
    }
    if (song.playing || transport_req) {               /* a flash erase stops the audio ~50 ms */
        ui_message("STOP BEFORE SAVE");
        return;
    }
    rc = op == 1u ? up_put(k, 0) : up_store(k, 0);
    if (rc == 3)
        ui_message(op == 1u ? "ERASED (RAM)" : "SAVED (RAM)");
    else if (rc)
        ui_message(op == 1u ? "ERASE ERROR" : "SAVE ERROR");
    else
        ui_say(op == 1u ? "ERASED " : "SAVED ", l);
    ui.force = 1;
}
#endif
