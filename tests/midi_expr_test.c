/* SPDX-License-Identifier: GPL-3.0-only */
/* MIDI expression in (seq.c midi_ctl, voice.c mx_tick) through the real MIDI queue, sequencer and mix:
 *   bend     +-2 semitones by default, the pitch measured from the output; centre exact; glides in
 *   RPN 0    a channel's range (semitones, cents, capped at 24), other channels kept, NRPN / null ignored
 *   wheel    CC1 vibrato (~+-50 cents at 127), off again: the plain pitch
 *   sustain  holds MIDI note-offs until the pedal is up (POLY, MONO, ARP, a note played again), per channel
 *   panic    nothing left sounding after CC123 / CC120 / CC121, STOP, a sound or engine change, MUTE
 *   drums    the drum channel: no bend / vibrato / sustain, CC120 / CC123 cut its voices
 *   select   a channel that plays the selected track: the parts it reaches, the part it left goes back
 *   rec      a note recorded with the pedal ends at the pedal's release
 * Exit status: the number of failed checks. */
#define main hostsim_main
#include "hostsim.c"
#undef main

static int bad;
static void ck(int ok, const char *what)
{
    printf("midi expr: %-92s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

static int32_t out[2 * CTL];
static void blocks(uint32_t n)
{
    while (n--)
        mix_block(out, CTL);
}
static void pkt(uint32_t st, uint32_t d1, uint32_t d2)
{
    midi_in_q[mi_w++ % MQ] = (st >> 4) | st << 8 | d1 << 16 | d2 << 24;
}
static void cc(uint32_t ch, uint32_t c, uint32_t v) { pkt(0xB0u | ch, c, v); }
static void bend(uint32_t ch, uint32_t v) { pkt(0xE0u | ch, v & 127u, v >> 7); }
static void on(uint32_t ch, uint32_t n) { pkt(0x90u | ch, n, 100); }
static void off(uint32_t ch, uint32_t n) { pkt(0x80u | ch, n, 0); }

static uint32_t gated(const track_t *t)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += t->v[i].active && t->v[i].gate;
    return n;
}
static uint32_t active(const track_t *t)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += t->v[i].active;
    return n;
}
static uint32_t drums_active(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < NDRUM; i++)
        n += drums.v[i].active;
    return n;
}

static void reset(void)
{
    uint32_t i;
    host_tracks_init();
    for (i = 0; i < NTRK; i++)
        trk_panic(&trk[i]);
    for (i = 0; i < NPART; i++)
        xfade_sine(&trk[i]);
    memset(&mxi, 0, sizeof mxi);
    memset(mx, 0, sizeof mx);
    memset(midi_sel_on, 0, sizeof midi_sel_on);
    song.sel = 0;
    song.rec = 0;
    song.solo = 0;
    mi_r = mi_w;
    blocks(2u * FS / CTL);                         /* the release tails ring out */
}

/* the periods of the left output over nb blocks (upward zero crossings, interpolated): shortest, longest, mean */
static double per_lo, per_hi;
static double periods(uint32_t nb)
{
    double last = -1, sum = 0;
    uint32_t i, pos = 0, cnt = 0;
    int32_t prev = out[2u * (CTL - 1u)];
    per_lo = 1e9;
    per_hi = 0;
    while (nb--) {
        mix_block(out, CTL);
        for (i = 0; i < CTL; i++, pos++) {
            int32_t x = out[2u * i];
            if (prev < 0 && x >= 0) {
                double at = (double)pos - 1.0 + (double)-prev / (double)(x - prev);
                if (last >= 0) {
                    double p = at - last;
                    per_lo = p < per_lo ? p : per_lo;
                    per_hi = p > per_hi ? p : per_hi;
                    sum += p;
                    cnt++;
                }
                last = at;
            }
            prev = x;
        }
    }
    return cnt ? sum / cnt : 0;
}
static int near(double a, double b, double tol) { return fabs(a / b - 1.0) < tol; }

/* the steps of track 0 from its note at step s: 1 + the TIE steps after it */
static uint32_t note_len(uint32_t s)
{
    uint32_t n = 1;
    while (s + n < NSTEP && trk[0].step[s + n].time == ST_TIE)
        n++;
    return n;
}
static uint32_t first_note(void)
{
    uint32_t i;
    for (i = 0; i < NSTEP; i++)
        if (trk[0].step[i].time == ST_NOTE && trk[0].step[i].n)
            return i;
    return NSTEP;
}
/* record one note on track 0 from MIDI ch 1: the key up after 1 step, the pedal (if any) up after 4 */
static uint32_t rec_take(int pedal)
{
    uint32_t stp = BEAT_U / 4u / 120u / CTL + 1u, k;   /* blocks a 1/16 step at 120 BPM */
    reset();
    trk[0].p[P_SLEN] = 16;
    trk[0].p[P_SDIV] = 2;
    steps_clear(&trk[0]);
    transport_req = 1;
    blocks(1);
    song.rec = 1;
    blocks(2);
    if (pedal)
        cc(0, 64, 127);
    on(0, 60);
    blocks(stp);
    off(0, 60);
    blocks(3u * stp);
    cc(0, 64, 0);
    for (k = 0; k < 3u * stp; k++)
        blocks(1);
    transport_req = 2;
    blocks(1);
    song.rec = 0;
    k = first_note();
    return k < NSTEP && trk[0].step[k].note[0] == 60u ? note_len(k) : 0u;
}

int main(void)
{
    double p0, p;
    uint32_t i, ok;
    track_t *t = &trk[0];

    memset(trk, 0, sizeof trk);
    memset(&song, 0, sizeof song);
    reset();

    /* ---- pitch bend */
    on(0, 69);
    blocks(300);
    p0 = periods(600);
    ck(near(p0, FS / 440.0, 0.002), "A4 on channel 1: 440 Hz with no bend");
    bend(0, 16383);
    blocks(1);
    ok = mx[0].bcur > 0 && mx[0].bcur < 8191;
    blocks(40);
    ck(ok && mx[0].bcur == 8191 && !mx[1].bcur && !mx[2].bcur, "bend up: it glides there (not a jump), part 1 only");
    p = periods(600);
    ck(near(p0 / p, pow(2.0, 8191.0 / 4096.0 / 12.0), 0.002), "bend 16383: +2 semitones (the pitch measured)");
    bend(0, 0);
    blocks(60);
    p = periods(600);
    ck(mx[0].bcur == -8192 && near(p0 / p, pow(2.0, -2.0 / 12.0), 0.002), "bend 0: -2 semitones exactly");
    bend(0, 8192);
    blocks(60);
    p = periods(600);
    ck(mx[0].bcur == 0 && near(p, p0, 0.0005), "bend 8192: the centre, exactly the note");
    trk_note_on(t, 76, 100);                       /* (a second voice: bent the same) */
    bend(0, 16383);
    blocks(60);
    ck(gated(t) == 2u && mx[0].bcur == 8191, "every sounding voice of the part bends (one offset a block)");
    trk_note_off(t, 76);

    /* ---- RPN 0: the bend range */
    cc(0, 101, 0);
    cc(0, 100, 0);
    cc(0, 6, 12);
    cc(0, 38, 0);
    bend(0, 16383);
    blocks(80);
    p = periods(600);
    ck(mx[0].bend == 8191 * 1200 / 200 && near(p0 / p, 2.0, 0.003), "RPN 0 = 12 semitones: bend 16383 an octave up");
    bend(1, 16383);
    blocks(2);
    ck(mx[1].bend == 8191, "channel 2 keeps +-2 semitones (the range is per channel)");
    cc(0, 6, 30);
    bend(0, 0);
    blocks(2);
    ck(mx[0].bend == -8192 * 2400 / 200, "RPN 0 = 30: capped at 24 semitones");
    cc(0, 6, 0);
    cc(0, 38, 50);
    bend(0, 16383);
    blocks(2);
    ck(mx[0].bend == 8191 * 50 / 200, "RPN 0 = 0 semitones 50 cents (CC38)");
    cc(0, 99, 1);
    cc(0, 98, 2);
    cc(0, 6, 5);
    cc(0, 101, 127);
    cc(0, 100, 127);
    cc(0, 6, 7);
    bend(0, 16383);
    blocks(2);
    ck(mx[0].bend == 8191 * 50 / 200, "data entry after an NRPN or the null RPN: the range unchanged");
    cc(2, 6, 12);
    bend(2, 16383);
    blocks(2);
    ck(mx[2].bend == 8191, "data entry with no RPN selected (power on): nothing");
    bend(0, 8192);
    bend(1, 8192);
    bend(2, 8192);
    blocks(60);
    off(0, 69);

    /* ---- the mod wheel */
    reset();
    on(0, 81);
    blocks(300);
    p0 = periods(1400);
    ck(per_hi / per_lo < 1.004, "A5, no wheel: a steady pitch");
    cc(0, 1, 127);
    blocks(1);
    ok = mx[0].dcur > 0 && mx[0].dcur < 127 << 8;
    blocks(40);
    ck(ok && mx[0].dcur == 127 << 8 && !mx[1].dcur, "CC1 127: the depth glides to the wheel, part 1 only");
    p = periods(1400);
    ck(per_hi / per_lo > 1.045 && per_hi / per_lo < 1.075 && near(p, p0, 0.003),
       "CC1 127: vibrato of about +-50 cents around the note");
    ck(fabs((double)VIB_INC * FS / CTL / 4294967296.0 - 5.5) < 0.01, "the vibrato: 5.5 Hz");
    cc(0, 1, 0);
    blocks(40);
    p = periods(1400);
    ck(mx[0].dcur == 0 && mx[0].vph == 0 && per_hi / per_lo < 1.004 && near(p, p0, 0.0005), "CC1 0: off, the plain note");
    off(0, 81);

    /* ---- sustain */
    reset();
    cc(0, 64, 127);
    on(0, 60);
    on(0, 64);
    on(0, 67);
    blocks(20);
    off(0, 60);
    off(0, 64);
    off(0, 67);
    blocks(20);
    ck(gated(t) == 3u, "pedal down: three notes keep sounding after their note-offs");
    cc(0, 64, 0);
    blocks(2);
    ck(gated(t) == 0u && !mxi.sch[0], "pedal up: released");
    cc(0, 64, 100);
    on(0, 60);
    off(0, 60);
    on(0, 60);
    blocks(4);
    ck(gated(t) == 1u && !(mxi.sus[0][1] & 1u << 28), "a held note played again: one voice, its key down (not held)");
    off(0, 60);
    blocks(4);
    ck(gated(t) == 1u, ".. its key up: held again");
    cc(0, 64, 0);
    blocks(2);
    ck(gated(t) == 0u, ".. pedal up: released");
    cc(0, 64, 127);
    on(1, 62);
    off(1, 62);
    blocks(4);
    ck(gated(&trk[1]) == 0u, "the pedal of channel 1 does not hold channel 2");
    cc(0, 64, 0);
    t->p[P_VOICE] = V_MONO;
    cc(0, 64, 127);
    on(0, 60);
    on(0, 62);
    off(0, 62);
    off(0, 60);
    blocks(10);
    ck(gated(t) == 1u && t->mono_note == 62u, "MONO, pedal down: the last note held");
    cc(0, 64, 0);
    blocks(2);
    ck(gated(t) == 0u && !t->nmono && !t->mono_note, "MONO, pedal up: released, the mono stack empty");
    t->p[P_VOICE] = V_POLY;
    t->p[P_AMODE] = 1;
    cc(0, 64, 127);
    on(0, 60);
    on(0, 64);
    blocks(4);
    off(0, 60);
    off(0, 64);
    blocks(300);
    ck(t->nheld == 2u, "ARP, pedal down: the arp keeps both notes after their keys");
    cc(0, 64, 0);
    blocks(300);
    ck(t->nheld == 0u && gated(t) == 0u, "ARP, pedal up: the arp lets them go");
    t->p[P_AMODE] = 0;
    {   /* the FM-1's keys ignore the pedal */
        cc(0, 64, 127);
        input_on(t, 60, 100);
        blocks(4);
        input_off(t, 60);
        blocks(4);
        ck(gated(t) == 0u, "the FM-1's keys ignore the pedal");
        cc(0, 64, 0);
    }

    /* ---- no hanging note */
    reset();
    cc(0, 64, 127);
    on(0, 60);
    on(0, 64);
    off(0, 64);
    blocks(10);
    cc(0, 123, 0);
    blocks(2);
    ck(gated(t) == 0u && !mxi.sch[0], "CC123: every note off, the pedal's and a key still down");
    off(0, 60);
    cc(0, 64, 0);
    blocks(2);
    ck(gated(t) == 0u, ".. the late note-off and pedal up: nothing comes back");
    on(0, 60);
    on(0, 67);
    blocks(10);
    cc(0, 120, 0);
    blocks(3);
    ck(active(t) == 0u, "CC120: the voices gone within 3 blocks (a fade, no release)");
    off(0, 60);
    off(0, 67);
    cc(0, 64, 127);
    on(0, 60);
    off(0, 60);
    bend(0, 12000);
    cc(0, 1, 90);
    blocks(10);
    cc(0, 121, 0);
    blocks(40);
    ck(gated(t) == 0u && !mxi.ped && !mx[0].bcur && !mx[0].dcur, "CC121: no bend, no vibrato, pedal up and its notes released");
    reset();
    transport_req = 1;
    blocks(1);
    cc(0, 64, 127);
    on(0, 60);
    off(0, 60);
    blocks(10);
    transport_req = 2;
    blocks(1);
    ck(gated(t) == 0u && !mxi.sch[0], "STOP: the pedal's notes released");
    cc(0, 64, 0);
    cc(1, 64, 127);
    on(1, 60);
    off(1, 60);
    blocks(10);
    host_preset_req(&trk[1], 1, 0);
    panic_req |= 2u;
    blocks(40);
    ck(gated(&trk[1]) == 0u && trk[1].engine == 1u && !mxi.sch[1], "a sound / engine change (panic): released");
    on(1, 62);
    off(1, 62);
    blocks(10);
    host_preset_req(&trk[1], 2, 0);                /* (an engine switch on its own) */
    blocks(40);
    ck(gated(&trk[1]) == 0u && trk[1].engine == 2u && !mxi.sch[1], "an engine switch: released");
    cc(1, 64, 0);
    cc(2, 64, 127);
    on(2, 62);
    off(2, 62);
    blocks(10);
    trk[2].p[P_MUTE] = 1;
    blocks(1);
    ck(gated(&trk[2]) == 0u && !mxi.sch[2], "MUTE: released");
    trk[2].p[P_MUTE] = 0;
    blocks(10);
    ck(gated(&trk[2]) == 0u, ".. unmuted: nothing comes back");
    on(2, 64);
    off(2, 64);
    blocks(10);
    song.solo = 1u;
    blocks(1);
    ck(gated(&trk[2]) == 0u && !mxi.sch[2], "another track soloed: released");
    song.solo = 0;
    cc(2, 64, 0);

    /* ---- the drum channel (10) */
    reset();
    on(0, 60);
    cc(9, 64, 127);
    bend(9, 16383);
    cc(9, 1, 127);
    on(9, 36);
    blocks(4);
    ok = drums_active() > 0u;
    off(9, 36);
    blocks(40);
    ck(ok && !mx[0].bend && !mx[0].dep && !mx[1].bend && !mx[2].bend && !mxi.sch[0] && !mxi.sch[1] && !mxi.sch[2],
       "drum channel: bend, wheel and pedal reach no part; its hits play");
    on(9, 38);
    on(9, 42);
    blocks(2);
    ok = drums_active() >= 2u;
    cc(9, 120, 0);
    blocks(1);
    ck(ok && drums_active() == 0u && gated(t) == 1u, "drum channel CC120: its voices cut, part 1 sounds on");
    on(9, 49);
    blocks(2);
    ok = drums_active() > 0u;
    cc(9, 123, 0);
    blocks(1);
    ck(ok && drums_active() == 0u && gated(t) == 1u, "drum channel CC123: its voices cut");
    cc(9, 64, 0);
    off(0, 60);

    /* ---- a channel that plays the selected track (5) */
    reset();
    on(4, 60);
    bend(4, 16383);
    blocks(2);
    ck(mx[0].bend == 8191 && !mx[1].bend, "channel 5, track 1 selected: bends track 1");
    song.sel = 1;
    on(4, 64);
    bend(4, 16000);
    blocks(2);
    ck(mx[0].bend == mx[1].bend && mx[1].bend == (16000 - 8192), "track 2 selected: both (track 1 still holds its note)");
    off(4, 60);
    bend(4, 8192);
    bend(4, 16383);
    blocks(60);
    ck(!mx[0].bend && !mx[0].bcur && mx[1].bend == 8191, "its note let go: track 1 back to no bend, track 2 bent");
    song.sel = 3;
    bend(4, 16383);
    blocks(2);
    ck(!mx[0].bend && mx[1].bend == 8191 && gated(&trk[1]) == 1u, "drums selected: the part holding its note only");
    off(4, 64);
    bend(4, 8192);
    blocks(60);
    ck(!mx[1].bcur && gated(&trk[1]) == 0u, ".. released, centred");

    /* ---- recording */
    {
        uint32_t a = rec_take(0), b = rec_take(1);
        char what[128];
        snprintf(what, sizeof what, "recording: a note held 1 step is %u step(s); with the pedal up 3 steps later, %u", a, b);
        ck(a == 1u && b == 4u, what);
    }

    printf(bad ? "midi expr: %d FAILED\n" : "midi expr: all ok\n", bad);
    return bad != 0;
}
