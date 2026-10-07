# SLOOP arpeggiator: ideas for a future version

Scope: things not already in the firmware (UP, DOWN, UP-DOWN x2, RANDOM, AS-PLAYED, CONVERGE, DIVERGE, THUMB, PINKY, WALK, SHUFFLE, OCT interleave, accent patterns, euclid gate, per-note ratchet, CHORD, rate/oct/gate/swing/prob/hold/order).

Conventions
- Example input is always held notes `C E G B` (C4 E4 G4 B4). `C+` = C one octave up, `C++` = two octaves up, `.` = rest, `-` = tie.
- Complexity for a C arp with <=16 held notes, a tempo grid and one sounding note:
  - S = a few lines, at most one small state variable, no new UI beyond a param
  - M = new per-arp state or a ROM table, one or two params, sub-step timing maybe
  - L = new data model (stored patterns/editor), polyphony, or timing outside the grid
- "Seen in" is marked `(v)` when I read it in a manual/doc during this research, `(m)` when it is from memory and not verified, `(s)` when only a search snippet confirmed it. Several hardware manuals (Digitone, Syntakt, Wavestate, Minilogue, OP-1/OP-Z, Moog, JD-Xi/JP-8000, Reface, Circuit, KeyStep Pro, Rene) could not be fetched, so entries for them are `(m)` or `(s)`.
- Within each group, ranked by musical payoff per effort (best first).

## Top 10 picks (if only a few get built)

1. RHYM - rhythm preset masks (S/M)
2. LANE - independent-length gate/velocity/octave lanes, gives polyrhythm for free (M)
3. ROT - pattern offset / rotate (S)
4. SYNC - retrigger on note / on bar (S)
5. DEJA - "deja vu" repeatable randomness (M)
6. TIE - ties/legato steps (S)
7. TRNS / DEG - transpose per cycle, chromatic and in-scale (S / M)
8. ONCE - random order locked until notes change (S)
9. STRUM - ratchet-as-run through held notes (M)
10. HUM - humanize (S)

## 1. Note order

| Name | What it does (held C E G B) | Seen in | Cx |
|---|---|---|---|
| ROT | Rotate the played pattern by N steps; offset 1 gives `E G B C`, offset 2 gives `G B C E`. Cheap to make live-tweakable and combines with every other order. | Ableton Pattern Offset (v), Reason Shift Step -16..+16 (v), Digitone II "shift" arp left/right (v, forum), Minimal Audio Step Offset (v) | S |
| ONCE | One random permutation is picked and then repeated until the held-note set changes: `G C B E G C B E ...`. Differs from SHUFFLE, which re-rolls every cycle. Store the permutation, rebuild only on note add/remove. | Ableton "Random Once" (v) | S |
| INV | Inversion walk: each cycle rotates the chord and lifts the wrapped note an octave: `C E G B`, `E G B C+`, `G B C+ E+`, `B C+ E+ G+`, then reset at oct range. Sounds like a rising chord voicing rather than a scale run. | DeepMind 12 Up Inv / Down Inv / Up&Down Inv (v) | S |
| REP2 | Each note repeated N times (x2, x4): `C C E E G G B B`. Good for 1/32 machine-gun feel without ratchet params. | Omnisphere Repeat X2/X4 (v), Reason Dual Arpeggio "Repeat" (v) | S |
| WIND | Sliding window of 3, step back after each: `C E G`, `E G B`, `G B C+`, `B C+ E+`. A "triad walk"; less obvious than UP, great on 4+ notes. | expressible in QMidiArp pattern text (v), Arpache User Pattern (v); no named hardware preset known | S |
| OCTM | Octave-behaviour variants: THIN flattens all octaves into one sorted line (UP-DOWN at 2 oct walks `C E G B C+ E+ G+ B+ G+ ...` as one shape); 1BY1 plays the whole up-down in oct 1, then the whole up-down in oct 2. Complements the existing interleave. | Bitwig Arpeggiator Octave Behavior Broad/Thin/1 by 1 (v) | S |
| DBL | "Double up / double down" modes (semantics not verified; probably each note plus its octave). Worth a look only if cheap. | Arturia KeyStep (s) | S |

Excluded: DeepMind "Up Alt / Down Alt" (outer/inner ping-pong) - the doc description is too vague and it overlaps CONVERGE/DIVERGE.

## 2. Step-sequenced and preset patterns (Korg/Roland/Yamaha style)

| Name | What it does | Seen in | Cx |
|---|---|---|---|
| IDX | Pattern = 16 steps of note-index into the sorted held list (1..N), plus rest and tie; indices wrap (mod N) when fewer notes are held. Pattern `1 3 2 4 . 3 - 1` on C E G B plays `C G E B . G - C`. One engine subsumes every order above and unlocks ROM preset lists (32-64 "shapes"). ROM presets first (M), user editor later (L). | QMidiArp pattern text `0..9`, `p`, `(012)` (v), Cubase Arpache 5 "User Pattern" 12 slots (v), Arpache SX Sequence mode (v), Yamaha Montage/Motif arp types (v), Yarns 22 preset patterns (v) | M (ROM) / L (editor) |
| PATV | Velocity + gate + tie preset table, independent of note order. DeepMind has 32 factory + 32 user patterns of 1-32 steps; loop length is independent of note count so patterns phase against the cycle. Pair with LANE. | DeepMind 12 patterns 1-64 (v), Logic "Pattern" Live/Grid (v), Omnisphere 32 steps with per-step length/velocity/tie (v) | M |
| PHRS | Chord-aware phrase: a recorded phrase stored as scale-degree/chord-tone relative to the chord root, so playing Cmaj7 or Dm7 re-maps the same melody. Needs chord detection. Yamaha caps phrases at 16 unique notes (matches our 16-note buffer). | Yamaha Montage "Original Notes"/chord-intelligent arps (v), Arpache SX Sort Normal/First/Any (v) | L |
| FIXD | "Fixed note" mode for drum tracks: held notes only trigger the pattern, pitch is ignored (pads fire the same lane). | Yamaha Montage fixed-note arps (v) | S |

## 3. Rhythm and gate

| Name | What it does | Seen in | Cx |
|---|---|---|---|
| RHYM | Library of 16-step rhythm masks, rest-aware. Two pointer policies: the note pointer advances on rests (`C . G B . C+ . E+`) or holds on rests (`C . E G . B . C+`). Bank of ~32 grows from simple (all quarters) to syncopated. Cheap way to get a lot of variety without a pattern editor. | Novation Peak 33 arp rhythms (s), Electribe 2 50 gate arp types (s), Yarns 22 patterns (v), Minimal Audio Rhythmic Mode + Rhythm Start (v) | S/M |
| TIE | Tied/legato step: previous note keeps sounding, no retrigger. In a mono voice a tie into a different pitch can glide. Real legato phrases, not just gate > 100%. | Ableton Gate >100% legato (v), Logic ties (v), Omnisphere ties (v), Reason pattern ties / gate 25-400% (v) | S |
| HUM | Humanize: random +/- timing (a few ticks), velocity, gate per step; amount knob. | Bitwig randomize vel/timing/duration (v), QMidiArp random timing/velocity/length (v) | S |
| GLAN | Per-step gate lane (4-16 values, 10-100%): `25 90 50 90` gives staccato-legato phrasing. Pairs well with LANE. | Logic Grid pattern (v), Reason gate (v), Bitwig per-step duration (v), Wavestate gate lane (s) | M |
| FADE | Velocity ramp over the cycle or N steps (fade in/out, target velocity + time), reset on retrigger. `127 -> 40` over 2 bars. Ableton's Vel Decay/Target; QMidiArp attack/release ramps. | Ableton (v), QMidiArp (v) | S |
| GRV | Groove templates beyond swing: 16-step timing/velocity offset tables (MPC-style shaped swing, "late 3rd step", etc.), amount knob. | Ableton Groove Pool (v), Omnisphere Groove Lock (v), Reason shuffle variants (v) | M |
| TUP | Extra rate grid: dotted, quintuplet (1/20), septuplet; great for 5:4 against drums. | Digitone II quintuplets (v, forum), Reason shuffle rates (v) | S |
| SKIP | Per-step random skip (separate from the arp's global probability, e.g. never skip step 1). | Minimal Audio Skip (v) | S |
| FIXV | Velocity source: REAL (from key), FIXED, ACCENT-only. | Roland SH-4D velocity REAL/1-127 (v), Arpache fixed vs input (v) | S |

## 4. Ratchet variants (existing per-note ratchet)

| Name | What it does | Seen in | Cx |
|---|---|---|---|
| STRUM | Ratchet-as-run: the step plays N fast notes walking through the held notes instead of repeating one: at 1/4 rate each step is `C E G B` as 32nds. Single voice, so it is a fast run, not a chord. Ableton notes that a low Repeats value emulates strumming. | Ableton Repeats (v), generic strum on most DAWs (m) | M |
| RAMP | Velocity curve across ratchet hits: decay `100 80 60 40` (echo/flam) or crescendo `40 60 80 100` (build-up). | Elektron retrig with velocity curve (m), OP-Z retrig step component (s) | S |
| FILL | Ratchet/roll only on the last step of the cycle or last cycle in 4 (turnaround fill): `C E G B` then next bar ends `... B B B B`. | Elektron FILL trig condition (m), Marbles random ratcheting (s) | S |
| ACCL | Accelerating / decelerating ratchet (bouncing-ball): hit spacing 8,6,5,4,3 ticks, tick-quantized so it stays on the clock. | Minimal Audio exponential rhythm curve (v) | M |
| RPIT | Ratchet with pitch: x3 ratchet on `C` plays `C E G` or `C G C+` (root, 5th, octave) so a ratchet turns into a mini-arp. | OP-Z multi-program steps (s) | M |
| ROLL | Momentary performance button: while held, the arp rate doubles (or forces x2/x4 ratchet); release returns. | MPC-style Note Repeat (m), Bitwig Note Repeat (m) | S |

## 5. Pitch transforms

| Name | What it does | Seen in | Cx |
|---|---|---|---|
| TRNS | Per-cycle transpose: add interval X each cycle for K steps, then reset. `+12`, 2 steps: `C E G B`, `C+ E+ G+ B+`, `C++ E++ G++ B++`. | Ableton Distance/Steps (v), Arpache SX Repeats and Pitch Shift (v), Minimal Audio Step Shift/Alternate Steps (v) | S |
| DEG | Same, but in scale degrees (stay in key): +1 degree per cycle in C major gives `C E G B`, `D F A C+`, `E G B D+`. Needs the song/track scale table. | Ableton Distance in scale degrees + "Use Current Scale" (v) | M |
| OCTP | Per-step octave offset pattern (or random jump probability): offsets `0 0 +1 0` give `C E G+ B`; random 25%: `C E+ G B ...`. | Bitwig per-step pitch (v), Wavestate lanes (s) | S |
| XPOS | Per-step semitone/interval lane (e.g. `0 0 +7 +12`) applied after note choice, quantized to scale optionally. Generalizes OCTP. | Bitwig per-step pitch offset (v), Wavestate (s) | M |
| ROOT | Root accent / pedal: every Nth step plays the lowest note an octave down: `C E G C- B ...`. Generalizes THUMB with controllable period. | THUMB/PINKY neighbours; generic (m) | S |

## 6. Cycle-level variation and randomness

| Name | What it does | Seen in | Cx |
|---|---|---|---|
| DEJA | "Deja vu": random choices (order/octave/probability) come from a ring buffer of N decisions; one knob: 0 = fresh random, 100 = the same N-step loop forever, between = occasional mutation. Turns every random feature into a "random but repeating phrase" tool. | Mutable Marbles deja vu + loop length (s) | M |
| SPIC | Spice/Dice: one knob blends the straight gate pattern with a random mask; Dice re-rolls the mask. `x x x x` + 40% spice becomes `x . x x`. | Arturia MicroFreak Spice and Dice (s) | M |
| COND | Cycle conditions per step (1:2, 3:4, NOT-FIRST, FILL): a step plays only on given cycles of the note loop. Gives evolving, long-form patterns from a 4-note cycle. | Elektron trig conditions (m), OP-Z "variation per cycle" step components (s) | M |
| EVOL | Each cycle swap one random adjacent pair of the order: `C E G B`, `C G E B`, `C G B E`... drifts slowly; reset on note change. | generic generative practice (m) | S |

## 7. Sync, held-note handling and performance

| Name | What it does | Seen in | Cx |
|---|---|---|---|
| SYNC | Restart modes: free-run, restart on new note, restart on bar (or every N beats). Keeps a 3- or 5-step cycle phase-locked to the song. | Ableton Retrigger Off/Note/Beat (v), DeepMind Key Sync (v), Logic Trigger (v), QMidiArp restart/trigger (v) | S |
| ONE | One-shot / N repeats then stop (or rest): play the cycle once per key press, or `Repeats = 2`. With SYNC gives a played fill. | Ableton Repeats (v), Arpache One Shot (v), Minimal Audio One-Shot (v) | S |
| LEN | Fixed cycle length (1-16 steps) regardless of held notes: 4 notes over 5 steps plays `C E G B C`, `E G B C E`, ... shifts phase each cycle. Cheap polymeter. | Reason Steps 1-16 (v), Logic cycle length (v) | S |
| HOLD+ | Latch variants: additive (new key adds), toggle (replaying a latched note removes it), chord-replace on new chord after release. | Ableton Hold add/remove (v) | S |
| SPLT | Key range: arp only responds below/above a split; other notes pass through to the voice. Only fits if the voice can play the pass-through note. | Reason Input Range (v), Arpache key range (m) | M |

## 8. Polyrhythm

| Name | What it does | Seen in | Cx |
|---|---|---|---|
| LANE | Independent lanes (accent, gate, octave, ratchet, rest), each with its own length (1-16) and optionally start point, looping against the note cycle. 4 notes against a 3-step gate lane = 12-step phrase; 5-step accent lane gives a 20-step phrase. Highest payoff in this list for polyrhythm. | Wavestate Wave Sequencing 2.0 lanes with own steps/loop points (v, snippet), DeepMind pattern loop independent of note cycle (v), Logic Pattern (v), OP-Z step components (s) | M |
| LEN | See section 7 (note cycle shorter/longer than the step count). | Reason Steps (v) | S |
| TUP | See section 3 (quintuplet/septuplet grids). | Digitone II (v) | S |
| EPTR | Euclid variants: with pointer-advance on hits only (rests hold the note), versus advance on every grid tick; two euclid masks (gate + ratchet) with different pulses/length. | Yarns Euclidean + Grids family (v/s) | S |

## Notes for the C implementation

- Rest semantics matter for every rhythm feature: define once whether a rest advances the note pointer (grid-locked, like a hardware step sequencer) or holds it (rest-skips). Offer it as a parameter for RHYM/EPTR/SKIP.
- LANE, IDX and PATV share one data shape (array of up to 16 values, length, phase). Build that once and the rest are presets of it.
- ONCE, DEJA and EVOL all need a deterministic seed per held-note set; store a small xorshift state and a 16-slot ring buffer.
- Anything inside a step (STRUM, RAMP, ACCL, HUM timing) should be scheduled in ticks of the existing clock (e.g. 24 or 96 PPQN) so it stays tempo-locked and cancellable on note-off.
- Keep rate/octave/gate/swing as the global params; put new features behind one "shape" selector plus 2 macro knobs (AMT, LEN) so the small OLED stays usable.

## Sources actually consulted

Fetched and read:
- https://www.ableton.com/en/live-manual/12/live-midi-effect-reference/ (Arpeggiator: styles, Offset, Distance/Steps, Repeats, Retrigger, Hold, Vel)
- https://support.spectrasonics.net/manual/Omnisphere/arpeggiator/all.htm
- https://docs.propellerheads.se/reason9/Players.12.4.html (Dual Arpeggio)
- https://static.roland.com/manuals/sh-4d/eng/50790926.html
- https://help.apple.com/logicpro/mac/10.4.5/en.lproj/lgce1465941c.html and https://help.apple.com/logicpro-instruments/mac/10.2.3/en.lproj/lgsidb3bab5a.html
- https://cdn.jsdelivr.net/npm/patchwork-deepmind@0.3.0/skills/deepmind-parameter-guide/sections/arp-sequencer.md (Behringer DeepMind 12)
- https://pichenettes.github.io/mutable-instruments-documentation/modules/yarns/
- https://archive.steinberg.help/cubase_plugin_reference/v9/en/_shared/topics/plug_ref/arpache_sx_r.html
- https://qmidiarp.sourceforge.net/qmidiarp_doc_en_arp.html
- https://yamahasynth.com/learn/montage-series-synthesizers/mastering-montage-arpeggio-making-101-part-i/
- https://manual.minimal.audio/current-manual/keyboard-and-midi-effects/arpeggiator
- https://www.bitwig.com/userguide/latest/note_fx
- https://www.elektronauts.com/t/digitone-2-arpeggiator/252804

Search snippets only (page not read in full):
- https://steinberg.help/cubase_pro_plugin_reference/v13/en/_shared/topics/plug_ref/arpache_5/arpache_5_overview_r.html
- https://www.soundonsound.com/node/4920255?page=2 and https://markmoshermusic.com/2022/01/19/arturia-microfreak-synth-tip-converting-an-arp-to-a-sequence-enabling-modulation-track-automation/ (MicroFreak)
- https://ask.audio/articles/get-great-results-quickly-from-novation-peaks-arpeggiator- (Peak rhythms)
- https://www.korg.com/us/products/synthesizers/wavestate/ (Wavestate lanes)
- https://cdm.link/arturia-keystep-is-a-sequencer-arpeggiator-controller-keyboard/ (KeyStep modes)
- https://www.synthtopia.com/?p=95034 (Marbles)
- Electribe 2 and OP-Z product pages surfaced by search (retailer pages, no direct URL cited)

Not verified (manuals unreachable or content absent): Elektron Digitone/Syntakt mode lists and trig conditions, Korg Minilogue/Electribe arp tables, Roland JD-Xi/JP-8000/Jupiter-X, Moog Subsequent, Yamaha Reface, OP-1/OP-Z arp internals, Novation Circuit, Kontakt arp scripts, Make Noise Rene. Entries tagged `(m)` rely on general knowledge.
