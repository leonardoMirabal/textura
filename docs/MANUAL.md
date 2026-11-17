<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Textura manual

Every control of Textura on the FM-1, in full. The emulator's QUICK GUIDE has one line for each; this is the long
version. The screen shows one page at a time, one card per knob (K1 .. K4), and each page's footer says what
ALGORITHM and the buttons do there. Press a button again on its page for its second job.

## The keys

**Black keys**: by their groups (3, 2, 3, 2): lanes 1–4, shown on LANDSCAPE (F#5: nothing; a lane's sound: SEQ, the sound browser); held 0.6 s: that lane muted, or unmuted (it plays on, so it comes back at once; MUTED on its card; a scene keeps it).

**F3 .. E4 (the 7 left white keys)**: how many: off, 1, 2, 4, 10, 19, 32.

**F4 .. G5 (the 9 right white keys)**: how active: rare .. busy (each key sounds the same more).

Four lanes play at once; the keys' lights blink with each lane's activity. **PLAY mode** (SEL) turns the keys into a pitched keyboard for the selected lane; OCT− and OCT+ then shift the octave.

## Knobs

**KNOB 1–4**: home: the volumes of lanes 1–4; the lane pages: lanes 1–4; other pages: the cards.

**SELECT**: pages, first to last (no wrap), in the sound's order: after home the selected lane's SOUND (the source), then the lane pages (the mixer), each the four lanes as rows: HOW MANY, HOW ACTIVE, PAN, ON / MUTE.

**ALGORITHM**: named on each page's footer with its value: home: picks a landscape (EDIT loads it); SOUND: opens the sound browser; PERFORM: the reverb's type; COMPRESSOR: on / off; DRIVE: its type; FILTER: the slope; DELAY: freeze; REVERB: its type; LFO: which LFO; MATRIX: which slot; GROOVE: the groove's type.

**PRESETS**: the presets list: the factory presets (F001–F100) under their families, then USER (your U01–U16), then LANDSCAPES (each landscape's own start, L01–L10: a starting point to shape and save as your own), opened on the one loaded; PRESETS, ALGORITHM or K1 move the cursor (nothing plays until you choose), SELECT jumps a family, EDIT loads it (the list stays open), HOME closes it. FX there: LOAD OPTIONS, PRESETS CHANGE TEMPO (YES: a preset brings its tempo) and EFFECTS ON LOAD (SMART, the default: a preset brings its drive, filter, compressor, delay and reverb unless you turned one since, then yours stay, "FX: YOURS"; ALWAYS; NEVER; they glide, the tails ring on).

**OCT− OCT+ (buttons beside the knobs)**: previous / next lane: SOUND stays on that page, other pages go to LANDSCAPE (PLAY keys: octave).

**MASTER**: the output volume.

## Buttons

**HOME**: home (TEXTURA); again: back to the last page left (the footer names it); tapped twice quickly (within 0.3 s): PERFORM; held (0.6 s): the setup, MIDI CLOCK (GLO on: MIDI IN, DISPLAY).

**SEQ**: SOUNDS: the sound browser for the selected lane, one list as the presets': each family's header, then its sounds, opened on the lane's sound. ALGORITHM moves (past a family's last sound, the next family's first; a playing lane plays the sound after 0.4 s, HEARING, the kept one marked), SELECT jumps family, EDIT keeps the sound (the list stays), HOME or any other button closes with the kept sound back; SAVE held undoes the last sound kept. Any sound on any lane, the same one on two lanes too.

**EDIT**: the selected lane's sound (again: its next page).

**FX**: held (0.6 s), from any page: PERFORM at once (HOME, HOME again: back to it). Tapped: PERFORM, the front page to play the effects live: HIGH PASS, LOW PASS, DELAY MIX, REVERB MIX; again: every page of the compressor, the drive, the filter, the grain delay, the reverb, then round: the mix's chain, left to right in the order the sound goes through it, as Frodhes (one of each on all the lanes together; the reverb takes what the grain delay gives). FILTER: HIGH PASS (K1) into LOW PASS (K2): each a real filter over its whole knob, 20 Hz .. 20 kHz, shown in Hz where it is -3 dB, OFF at its open end (HIGH PASS 0, LOW PASS 127); bring them together for a band (a band-pass around them, RESONANCE makes it a peaky bell), cross them and it closes; RESONANCE peaks both edges; SLOPE 12 / 18 / 24 dB (also ALGORITHM, in that order). GRAIN 2/4: MOD takes the delay from plain (0) to an evolving cloud (the time drifts in a slow wow, each grain its own size, start, direction and place, more grains leave unison for INTERVALS: octaves, 5th + octave, 4th + 5th, major, minor, all, or up, which climbs on the repeats); MOD RATE how fast it moves, always at random; GRAIN the echo .. the grains. MIX (on every effect's card a bar, a line at 64): DRY (0, the sound alone); up to 64 the effect comes in, the sound whole (the bar in the colour); 64 both whole ("50/50"); above it the sound fades out (the bar on in the colour's complement); WET (127, the effect alone).

**SEL**: keys: landscape / play (in play every key lit); held, the other mode while held: in play, SEL held + a black key picks that lane (OCT: the next / previous one), the keys play it once SEL is let go.

**LFO**: the 8 LFOs' pages; held while a card turns: that LFO onto the card (the turn its depth, back to 0 removes it; in the browser: a click latches learn for 3 s, turn a knob; a second click: the LFO pages).

**ENV**: the ENV / FOLLOW pages (2 envelopes, 2 envelope followers); held while a card turns: the selected ENV or FOLLOW onto the card, as LFO (in the browser: a click latches learn for 3 s, turn a knob; a second click: the ENV pages).

**ARP**: the mod matrix: 16 slots, source > target, depth, spread (a modulated card shows ~ and its value now).

**GLO**: GROOVE, every groove control together on two pages: 1/2 TYPE, AMOUNT, SHIFT (ALGORITHM the TYPE too). The groove: the landscape's small events lean toward one shared, slowly drifting pulse, felt rather than heard, never a beat: AMOUNT (35 by default, light; 0: off), TYPE (polyrhythms first: 3:4, 3:4:5 ..., then swings, claves and other rhythms, then even spreads E(k,n)), SHIFT. GLO again: GROOVE 2/2, each lane's role on its knob (FREE; AUTO, by its sound: "AUTO: PULSE"; PULSE, OFFBEAT, FILL; or its own 2–9 pulses over the span, "5 PER 4 BEATS", a polyrhythm), with its pulse drawn; a sound the groove leaves alone says "FLOWS FREE". Then TEMPO (BPM, FINE, TRANSPORT, STATUS), SCALE (as Frodhes': MODE OFF, SNAP (a key out of the scale plays its nearest note) or DEGREE (the white keys walk the scale, the black ones silent), SCALE, ROOT, TRANSP; the keys only, MIDI notes play as sent; in PLAY only the scale's keys light), MIDI (clock in: AUTO follows a clock while one comes), MIDI IN (each lane's channel, notes and CCs, lane n on channel n; ALGORITHM: SELECTED CH, the channel that plays the selected lane, 1; OFF: channel 1 is lane 1's), DISPLAY. Water, wind and the other flows stay free; insects, birds, crackles and knocks lean the most. A scene keeps it.

**SAVE**: save the scene: PRESETS or K1 picks U01–U16, SAVE again saves, HOME cancels; held: undo the last load or the last sound kept (again: redo); the emulator keeps the scenes in the browser. MIDI program change picks one too (0–9, 16–31).

**PLAY**: tap tempo; held: start / stop.

**REC**: all lanes and the transport stop / start (bar 1 there: the groove starts in phase); pressed again within 1.5 s: taps, the tempo from the third (a series never leaves things stopped; no taps while a clock leads).

## In the browser

The emulator is the same firmware, compiled to WebAssembly. Computer keys: A .. ; play the keys F3 .. A4, Z and X are
OCT− and OCT+, Space is REC, the arrows turn SELECT (left / right) and ALGORITHM (up / down). Knobs: drag or use the
wheel. Web MIDI in: channel 1 plays the selected lane, channels 1 .. 4 lanes 1 .. 4 (GLO: MIDI IN); CC and NRPN reach
lanes 1 .. 4 on their channels, the effects on channel 5. Scenes and settings are kept in the browser.
