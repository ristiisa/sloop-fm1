/* SPDX-License-Identifier: GPL-3.0-only */
/* Parameter locks (seq.c plk_*, ui_layers.c P-LOCK) on the real sequencer, UI and project code:
 *   play     a locked step plays its value for its step (and the TIE after it), then the track's own;
 *            ratchets keep their hits; p[] holds the track's own value outside the render
 *   edits    a knob turned while a lock plays changes the track's own value (kept); a lock changed
 *            while its step plays is heard at once
 *   nothing stuck  STOP, MUTE, a preset, an engine (the lock into its range), a song section (in the
 *            ISR), a project load; a capture in the ISR saves the track's own values
 *   UI       SEQ + HOME, ENV: P-LOCK; a step key held + a knob: a lock (red), + OCT-: cleared; SEQ: the
 *            steps (a mark on locked ones); EDIT SHIFT, LENGTH x2, undo / redo, erase, clear move them;
 *            LOCKS FULL; the drum track: its SLICER
 * Exit status: the number of failed checks. */
/* (the UI on a framebuffer with panel / flash / storage doubles: as tests/ui_pages_test.c) */
#define FELUCCA_ARRANGER 1
#define main hostsim_main
#include "hostsim.c"
#undef main
#include <assert.h>
static uint16_t screen[240*240];
static void lcd_sync(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{ uint32_t i,j; assert(x+w<=240 && y+h<=240); for(j=0;j<h;j++) for(i=0;i<w;i++) screen[(y+j)*240+x+i]=p[j*w+i]; }
#include "../firmware/src/gfx.c"
static void lcd_fill(uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint16_t c)
{ uint32_t i,j; assert(x+w<=240&&y+h<=240); for(j=0;j<h;j++)for(i=0;i<w;i++)screen[(y+j)*240+x+i]=swap16(c); }
static int32_t encs[7];
static uint32_t fm1_ticks(void) { return fm1_ms * 1000u * 24u; }
#define FM1_TICKS_PER_US 24u
static int32_t fm1_enc_take(uint32_t e) { int32_t s = encs[e]; encs[e] = 0; return s; }
static uint8_t fm1_led[16], fm1_led_dim[16], fm1_led_bg[16];
static volatile uint16_t fm1_led_bg_ns;
#define FM1_NCOL 16u
static const int8_t FM1_KEYMAP[5][16];
static void fm1_led_key(uint32_t id, int on) { (void)id; (void)on; }
static uint32_t edges_btn, notes_seen;
static uint32_t fm1_input_edges(int x) { uint32_t e = edges_btn; (void)x; edges_btn = 0; return e; }
static uint32_t fm1_input_note_edges(void) { uint32_t e = fm1_in.notes & ~notes_seen; notes_seen = fm1_in.notes; return e; }
static void fm1_wdt_feed(void) {}
static int32_t fm1_adc_read(int c) { (void)c; return -1; }
static struct { uint32_t magic, stage, page, home, ui_frames; } felucca_dbg;
#define FELUCCA_ICONS 1
#define SCOPE_N 512u
static int16_t scope_buf[SCOPE_N];
static uint32_t scope_w;
#include "../firmware/src/panel.c"
#include "../firmware/src/ui.c"
static uint32_t saves, loads;
static int project_used(uint32_t i) { return i < 2; }
static void project_save(uint32_t i) { (void)i; saves++; ui_message("SAVED"); }
static void project_load(uint32_t i) { (void)i; loads++; }
static void arrangement_save(void) {}
static uint32_t arrangement_ready(void) { return 3; }
static void arrangement_apply(uint32_t s) { (void)s; }
static void song_backup(void) {}
static void song_restore(void) {}
static uint32_t sec_stores, sec_loads;
static void section_store(uint32_t s) { sec_stores++; live_sec = (int8_t)s; }
static void section_load(uint32_t s) { sec_loads++; live_sec = (int8_t)s; }
static int up_used(uint32_t k) { return k < 2; }
static int up_load(uint32_t k) { (void)k; return 0; }
static uint32_t up_count(void) { return 2; }
static uint32_t up_nth(uint32_t n) { return n; }
static uint32_t up_rank(uint32_t s) { return s; }
static void up_name(uint32_t k, char *b) { str_cpy(b, k ? "MY PAD" : "MY LEAD", 13); }
static void up_slot_label(char *b, uint32_t k) { fmt_int(b, (int32_t)k + 1); }
static void up_ui(uint32_t op, uint32_t k) { (void)op; (void)k; }
static void settings_save(void) {}
#include "../firmware/src/ui_song.c"
#include "../firmware/src/ui_studio.c"
#include "../firmware/src/icons.c"
#include "../firmware/src/ui_draw.c"
#include "../firmware/src/ui_layers.c"
#include "../firmware/src/ui_menu.c"
#include "../firmware/src/ui_input.c"
#include "../firmware/src/splash.c"
static const char *outdir;
static void ppm(const char *name) {
    char path[512]; snprintf(path,sizeof path,"%s/%s.ppm",outdir,name);
    FILE *f=fopen(path,"wb"); assert(f); fprintf(f,"P6\n240 240\n255\n");
    for(unsigned i=0;i<240*240;i++) { uint16_t p=swap16(screen[i]); uint8_t rgb[3]={(p>>11)*255/31,((p>>5)&63)*255/63,(p&31)*255/31}; fwrite(rgb,1,3,f); }
    fclose(f);
}
/* one UI frame (~16 ms): the audio between (as the ISR does), then input, LEDs, draw */
static void frame(void)
{
    uint32_t q;
    static int32_t o[CTL * 2];
    for (q = 0; q < 22u; q++) mix_block(o, CTL);
    ui_input(); ui_leds(); ui_draw(); fm1_ms += 16;
}
static void frames(uint32_t n) { while (n--) frame(); }
static uint32_t BT(uint32_t b) { return 1u << panel.btn[b]; }
static void press(uint32_t b) { edges_btn |= BT(b); fm1_in.buttons |= BT(b); frame(); }
static void release(uint32_t b) { fm1_in.buttons &= ~BT(b); frame(); }
static void tap(uint32_t b) { press(b); release(b); }
static void key(uint32_t k) { fm1_in.notes |= 1u << k; frame(); fm1_in.notes &= ~(1u << k); frame(); }
#define PROJ_HOST 1
#include "../firmware/src/project.c"

static int bad;
static void pcheck(int ok, const char *what)
{
    printf("plock: %-72s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

static uint32_t kicks;                           /* drum hits on the kick lane (ratchets) */
static void run_block(void)
{
    int32_t o[CTL * 2];
    uint32_t a = drums.age, k;
    mix_block(o, CTL);
    if (drums.age != a)
        for (k = 0; k < NDRUM; k++)
            kicks += drums.v[k].age > a && drums.v[k].note == LANE_NOTE[0];
}
/* the value of parameter id of t the next block renders with (the locks swapped in as the ISR does) */
static int16_t eff(track_t *t, uint32_t id)
{
    int16_t v;
    plk_block_in();
    v = t->p[id];
    plk_block_out();
    return v;
}
static uint32_t run_to(track_t *t, uint32_t idx)  /* blocks until step idx of t plays */
{
    uint32_t n = 0;
    while ((t->seq_idx != idx || t->seq_abs == SEQ_NONE) && n < 100000u) {
        run_block();
        n++;
    }
    return n < 100000u;
}

static void seq_part(void)
{
    static const uint8_t N[8] = {60, 62, 0, 64, 65, 0, 67, 69};
    static project_t q, q2;
    track_t *t = &trk[0];
    uint32_t i, ok, okb, blocks;
    int16_t v;
    transport_req = 0;
    song.playing = 0;
    host_tracks_init();
    host_preset(t, 0, 0);
    t->p[P_SLEN] = 8;
    t->p[P_ED_FLT] = 0;
    t->p[P_DEC] = 70;
    for (i = 0; i < 8u; i++)
        put_step(t, i, N[i] ? 1u : 0u, &N[i], i == 2u ? ST_TIE : N[i] ? ST_NOTE : ST_REST, 0);
    t->step[4].rat = 2;                          /* x3 */
    pcheck(plk_set(t, 1, P_ED_FLT, 50) && plk_set(t, 4, P_ED_FLT, -30) && plk_set(t, 4, P_DEC, 5) &&
           plk_count(t, 4) == 2u, "locks set (FLT on 1, FLT and DEC on 4)");
    pcheck(!plk_lockable(t, P_SLEN) && !plk_lockable(t, P_AMODE) && !plk_lockable(t, P_LEVEL) && !plk_lockable(t, P_MUTE) &&
           plk_lockable(t, P_E3) && plk_lockable(t, P_REV) && !plk_lockable(TDRUM, P_ATK) && plk_lockable(TDRUM, P_SLCR),
           "lockable: the sound, not the pattern / arp / level / mute; drums: the SLICER");
    dstep_set(&TDRUM->dstep[0], 0, LV_NORM, 2);  /* a kick x3 on the drums' step 1, its SLICER depth locked */
    TDRUM->p[P_SLEN] = 8;
    plk_set(TDRUM, 0, P_SLDEPTH, 10);

    /* two loops: the locked value during its step (and the TIE), the track's own between, p[] its own */
    transport_req = 1;
    ok = okb = 1;
    kicks = 0;
    for (blocks = 0; blocks < 2u * 8u * 172u; blocks++) {   /* (a step: 172.3 blocks at 120 BPM) */
        uint32_t s;
        int16_t e, want;
        run_block();
        s = t->seq_idx;
        e = eff(t, P_ED_FLT);
        want = s == 1u || s == 2u ? 50 : s == 4u ? -30 : 0;
        ok &= e == want && eff(t, P_DEC) == (s == 4u ? 5 : 70);
        okb &= t->p[P_ED_FLT] == 0 && t->p[P_DEC] == 70 && TDRUM->p[P_SLDEPTH] == TP[P_SLDEPTH].def &&
               eff(TDRUM, P_SLDEPTH) == (TDRUM->seq_idx == 0u ? 10 : TP[P_SLDEPTH].def);
    }
    pcheck(ok, "playing: the lock during its step and the TIE after it, the track's own between");
    pcheck(okb, "playing: p[] holds the track's own values outside the render (drums: SLICER)");
    pcheck(kicks == 6u, "the drums' ratchet x3 on a locked step: 3 hits a loop");

    /* a knob turned while the lock plays: the track's own value, kept */
    run_to(t, 1);
    t->p[P_ED_FLT] = -20;                         /* (the UI, between two blocks) */
    run_block();
    ok = eff(t, P_ED_FLT) == 50 && t->p[P_ED_FLT] == -20;
    plk_set(t, 1, P_ED_FLT, 60);                  /* the lock turned while its step plays: heard at once */
    run_block();
    ok &= eff(t, P_ED_FLT) == 60;
    run_to(t, 3);
    pcheck(ok && eff(t, P_ED_FLT) == -20 && t->p[P_ED_FLT] == -20, "a knob turned during a lock: kept, heard after it");
    t->p[P_ED_FLT] = 0;

    /* STOP during a lock */
    run_to(t, 4);
    transport_req = 2;
    run_block();
    pcheck(!song.playing && !t->lk_n && !t->lk_on && eff(t, P_ED_FLT) == 0 && eff(t, P_DEC) == 70,
           "STOP during a lock: the track's own values");

    /* MUTE: the steps run on, nothing stays */
    transport_req = 1;
    run_to(t, 1);
    t->p[P_MUTE] = 1;
    run_block();
    run_to(t, 3);
    pcheck(eff(t, P_ED_FLT) == 0 && t->p[P_ED_FLT] == 0, "MUTE during a lock: the track's own value after its step");
    t->p[P_MUTE] = 0;

    /* a preset during a lock: its values are the track's own, the lock plays on */
    run_to(t, 1);
    host_preset_req(t, 0, 3);
    v = t->p[P_ED_FLT];
    run_block();
    ok = eff(t, P_ED_FLT) == 60 && t->p[P_ED_FLT] == v;   /* (the lock turned to 60 above) */
    run_to(t, 3);
    pcheck(ok && eff(t, P_ED_FLT) == v && t->p[P_DEC] == ENGINES[0]->presets[3].env[1],
           "a preset during a lock: the preset's values after it, none lost");

    /* an engine switch: a lock of EDIT into the new engine's range */
    {
        uint32_t a, b = 0, k = 0, found = 0;
        for (a = 0; a < NENGINES && !found; a++)
            for (b = 0; b < NENGINES && !found; b++)
                for (k = 0; k < 8u && !found; k++)
                    found = ENGINES[a]->edit[k].max > ENGINES[b]->edit[k].max;
        a--, b--, k--;
        host_preset(t, a, 0);
        plk_set(t, 1, P_E0 + k, ENGINES[a]->edit[k].max);
        run_to(t, 3);
        host_preset_req(t, b, 0);                 /* (the engine the UI asks for: the ISR fades, then switches) */
        run_to(t, 1);
        run_block();
        v = eff(t, P_E0 + k);
        pcheck(found && v <= ENGINES[b]->edit[k].max && v >= ENGINES[b]->edit[k].min && t->p[P_E0 + k] == ENGINES[b]->presets[0].e[k],
               "an engine switch: an EDIT lock plays inside the new engine's range");
        host_preset(t, 0, 0);
        t->p[P_ED_FLT] = 0;
    }

    /* a capture in the ISR (song mode PLAY): the track's own values */
    run_to(t, 1);
    plk_block_in();
    proj_capture(&q);
    plk_block_out();
    pcheck(q.t[0].p[P_ED_FLT] == 0 && plk_find(0 << 6 | 1, P_ED_FLT) >= 0 && !memcmp(q.lk, plk, sizeof q.lk),
           "a capture while a lock plays (ISR): the track's own value, the locks");

    /* a song section (applied in the ISR, its locks swapped in): the section's values, its locks */
    q2 = q;
    q2.t[0].p[P_ED_FLT] = -40;
    memset(q2.lk, 0, sizeof q2.lk);
    q2.lk[3] = (plk_t){0 << 6 | 6, P_ED_FLT + 1, 33};
    run_to(t, 1);
    plk_block_in();
    proj_apply(&q2, 0);
    seq_reset_tracks(0);
    plk_block_out();
    ok = t->p[P_ED_FLT] == -40 && !t->lk_n && eff(t, P_ED_FLT) == -40;
    run_to(t, 6);
    ok &= eff(t, P_ED_FLT) == 33;
    run_to(t, 7);
    pcheck(ok && eff(t, P_ED_FLT) == -40, "a song section during a lock: its values, its locks, nothing left");

    /* a project load (the UI, IRQ off) during a lock */
    run_to(t, 6);
    proj_apply(&q, 1);
    transport_req = 2;
    run_block();
    pcheck(t->p[P_ED_FLT] == 0 && eff(t, P_ED_FLT) == 0 && plk_find(0 << 6 | 1, P_ED_FLT) >= 0 &&
           plk_find(0 << 6 | 6, P_ED_FLT) < 0, "a project load during a lock: its values and locks");

    /* no locks: the track plays its own values on every step */
    plk_clear_track(t);
    transport_req = 1;
    ok = 1;
    for (blocks = 0; blocks < 8u * 175u; blocks++) {
        run_block();
        ok &= eff(t, P_ED_FLT) == 0 && !t->lk_n;
    }
    transport_req = 2;
    run_block();
    pcheck(ok, "no locks: the track's own values on every step");
}

static int msg_is(const char *s) { return ui.msg_t && !memcmp(ui.msg, s, str_len(s)); }

static void ui_part(void)
{
    track_t *t = &trk[0];
    int16_t v = 0, atk;
    uint32_t i, k3 = key_of_white(2), len;
    host_tracks_init();
    for (i = 0; i < NPART; i++) { set_engine_of(&trk[i], TRK_DEF[i][0]); apply_preset_to(&trk[i], TRK_DEF[i][1]); trk[i].engine = trk[i].eng_req; }
    TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;
    song.sel = 0;
    go_home(); frames(2);
    press(B_SEQ); frames(3); tap(B_HOME); release(B_SEQ); frames(3);
    pcheck(ly_lock == LY_STEP && ui.layer == LY_STEP && !plk_view(), "SEQ + HOME: the steps, locked open");
    tap(B_ENV); frames(2);
    pcheck(ly_lock == LY_STEP && plk_view() && cur_fam() == FAM_ENV, "locked SEQ, ENV: its page, the keys still steps (P-LOCK)");
    ui.force = 1; frame(); ppm("plock-env");
    atk = t->p[P_ATK];
    fm1_in.notes |= 1u << k3; frame();           /* step 3 held: set */
    encs[panel.enc[EN_K1]] = 5; frame();
    pcheck(step_on(&t->step[2]) && plk_get(t, 2, P_ATK, &v) && v == atk + 5 && t->p[P_ATK] == atk,
           "a step key held + KNOB 1: ATK locked on it, the track's own ATK kept");
    frames(42); ui.force = 1; frame(); ppm("plock-held");   /* (the message gone: LOCK 3 in the header) */
    pcheck(ui.col[0][str_len(ui.col[0]) - 3] == 'E', "the locked value drawn red");
    fm1_in.notes &= ~(1u << k3); frames(2);
    pcheck(step_on(&t->step[2]) && plk_count(t, 2) == 1u, "let go: the step and its lock stay");
    encs[panel.enc[EN_K2]] = 3; frames(2);
    pcheck(t->p[P_DEC] != ENGINES[t->eng_req]->presets[t->preset].env[1] && plk_count(t, 2) == 1u,
           "no step held, KNOB 2: the track's own DEC (no lock)");
    fm1_in.notes |= 1u << k3; frame();
    press(B_OCTDN); release(B_OCTDN);
    pcheck(plk_count(t, 2) == 0u && step_on(&t->step[2]) && msg_is("LOCKS CLEARED"), "a step key held + OCT-: its locks go, the step stays");
    encs[panel.enc[EN_K4]] = -4; frame();
    fm1_in.notes &= ~(1u << k3); frames(2);
    pcheck(plk_get(t, 2, P_REL, &v) && v == t->p[P_REL] - 4, "KNOB 4: REL locked");
    tap(B_SEQ); frames(2);
    pcheck(ly_lock == LY_STEP && !plk_view() && ui.layer == LY_STEP, "SEQ tapped: the steps again, still locked");
    ui.force = 1; frame(); ppm("plock-steps");
    pcheck(plk_marks(t, 0) == 1u << 2, "the steps with locks are marked");
    tap(B_FX); frames(2);
    pcheck(plk_view() && cur_fam() == FAM_FX, "locked SEQ, FX tapped: the FX page (P-LOCK)");
    tap(B_HOME); frames(2);
    pcheck(ly_lock == LY_PLAY && !ui.plk, "HOME: let go");

    /* EDIT: SHIFT, LENGTH x2, undo / redo: the locks follow their steps */
    len = trk_len(t);
    press(B_EDIT); encs[panel.enc[EN_K1]] = 1; frame(); release(B_EDIT);
    pcheck(plk_count(t, 2) == 0u && plk_get(t, 3, P_REL, &v) && step_on(&t->step[3]), "EDIT SHIFT: the lock one step later, with its step");
    press(B_EDIT); encs[panel.enc[EN_K2]] = 1; frame(); release(B_EDIT);
    pcheck(trk_len(t) == 2u * len && plk_count(t, 3) == 1u && plk_count(t, 3 + len) == 1u, "EDIT LENGTH x2: the lock copied with the pattern");
    press(B_EDIT); tap(B_OCTDN); release(B_EDIT);
    pcheck(trk_len(t) == len && plk_count(t, 3 + len) == 0u && plk_count(t, 3) == 1u, "undo: the copy gone");
    press(B_EDIT); tap(B_OCTUP); release(B_EDIT);
    pcheck(trk_len(t) == 2u * len && plk_count(t, 3 + len) == 1u, "redo: back");
    press(B_EDIT); encs[panel.enc[EN_K2]] = -1; frame(); release(B_EDIT);

    /* erase the step's note: an empty step, no locks */
    t->p[P_TRANS] = 0;                           /* (808 BOOM: -24, the note out of the keys) */
    press(B_EDIT);
    for (i = 0; i < 27u && kb_map(t, i) != t->step[3].note[0]; i++)
        ;
    fm1_in.notes |= 1u << i; frame(); fm1_in.notes &= ~(1u << i); frame();
    release(B_EDIT);
    pcheck(!step_on(&t->step[3]) && plk_count(t, 3) == 0u, "EDIT + its note (stopped): the step empty, its lock gone");

    /* LOCKS FULL: the pool, a step */
    plk_clear_track(t);
    for (i = 0; i < PLK_MAX; i++)
        plk_set(&trk[1], i / PLK_STEP, P_ATK + (i % PLK_STEP < 7u ? i % PLK_STEP : 8u), 1);
    pcheck(plk_count(&trk[1], 0) == PLK_STEP && !plk_set(&trk[1], 0, P_GLIDE, 3) && !plk_set(&trk[1], 9, P_GLIDE, 3),
           "the pool: PLK_STEP a step, PLK_MAX in all");
    press(B_SEQ); frames(3); tap(B_HOME); release(B_SEQ); frames(3);
    tap(B_ENV); frames(2);
    fm1_in.notes |= 1u << k3; frame();
    encs[panel.enc[EN_K1]] = 2; frame();
    pcheck(plk_count(t, 2) == 0u && msg_is("LOCKS FULL"), "pool full: LOCKS FULL, nothing written");
    fm1_in.notes &= ~(1u << k3); frames(2);
    tap(B_HOME); frames(2);
    plk_clear_track(&trk[1]);

    /* clear the track (REC held): its locks go */
    plk_set(t, 5, P_REV, 90);
    track_defaults_steps(t);
    pcheck(plk_count(t, 5) == 0u, "the track cleared: its locks go");

    /* the drum track: its SLICER */
    song.sel = TRK_DRUM; go_home(); frames(2);
    press(B_SEQ); frames(3); tap(B_HOME); release(B_SEQ); frames(3);
    tap(B_FX); frames(2); tap(B_FX); frames(2);
    pcheck(plk_view() && cur_page()->graph == GR_SLCR, "the drum track, P-LOCK: FX twice, the SLICER");
    fm1_in.notes |= 1u << k3; frame();
    encs[panel.enc[EN_K4]] = -6; frame();
    ui.force = 1; frame(); ppm("plock-drums");
    fm1_in.notes &= ~(1u << k3); frames(2);
    pcheck(plk_get(TDRUM, 2, P_SLDEPTH, &v) && v == TDRUM->p[P_SLDEPTH] - 6 && dstep_mask(&TDRUM->dstep[2]),
           "a drum step held + KNOB 4: the SLICER depth locked");
    tap(B_FX); frames(2); tap(B_FX); frames(2);
    fm1_in.notes |= 1u << k3; frame();
    encs[panel.enc[EN_K2]] = 3; frame();
    pcheck(msg_is("NO LOCK HERE") && plk_count(TDRUM, 2) == 1u, "the drum track, a global page: NO LOCK HERE");
    fm1_in.notes &= ~(1u << k3); frames(2);
    tap(B_HOME); frames(2);
    song.sel = 0;
}

int main(int argc, char **argv)
{
    outdir = argc > 1 ? argv[1] : "build/host";
    panel = PANEL_DEFAULT;
    layers_init();
    settings.palette = 4;
    palette_set(4);
    seq_part();
    ui_part();
    printf("%s\n", bad ? "PARAMETER LOCK TEST FAILED" : "parameter lock test passed");
    return bad;
}
