The desktop plugin gets a roomier window, its sources moved by LFOs, envelopes and followers straight in the window, and the M/S section.

- Effect drawers: click an effect's header or ALL > and a drawer rises from the strip with a large live picture (compressor meters and curve, drive curve, filter response in Hz, delay taps, reverb tail) and every parameter in titled groups. Esc or CLOSE closes it. MOVEMENT opens the same drawer with the LFO 1-8, ENV and FOLLOW, and MATRIX tabs.
- ENGINE and SOURCE: the window says ENGINE for what a lane plays and SOURCE for the physics model it runs on. Host parameters L1 .. L16 SOUND are now named ENGINE; their ids and ranges are unchanged, so saved projects and automation still load.
- Each lane row has one small swap icon after its name to change the engine. A click anywhere else on the row selects the lane.
- The engine browser plays the engine under the cursor on the lane, a stopped lane too. CLOSE puts everything back as it was; KEEP keeps the engine.
- STOP / PLAY on every row, and STOP LANE / PLAY LANE in the lane panel. A lane stopped on its own comes back with its own HOW MANY and HOW ACTIVE, and PLAY ALL keeps it stopped.
- LFO, ENV and FOLLOW source mode: click a source and a red bar opens its mode. Drag any control to set how far the source moves it, on the control's own fader; double-click removes the link; CLEAR removes the source's links; DONE or Esc leaves. Outside the mode a moved control shows its reach and a live tick. Dragging a source's grip onto a control, or a right-click, are shortcuts. The MATRIX tab stays.
- UNDO and CLEAR ALL MOVEMENT.
- M/S: a seventh section last in the effects strip, after the reverb, as on the FM-1. L R M S meters, a stereo scope and correlation, MID and SIDE (-INF to +6 dB), WIDTH (0 to 200 %) and MONO. At 0 dB, 100 % and MONO off it leaves the sound untouched. Host parameters were added at the end; existing ids are unchanged.
- Fixes: a state saved during a preview keeps the lane as it was; a preview's HOW MANY and HOW ACTIVE no longer show as host automation; the reverb graph is dim when TYPE is OFF; compressor ATTACK and RELEASE read in ms; hints no longer touch their fader.
- The manual (Textura-Plugin-Manual.pdf, 30 pages) and its pictures are redone for all of this, and every zip carries it.
- Builds: macOS (universal: Apple silicon and Intel; CLAP, VST3, Audio Unit), Windows x64 and Linux (x86_64, aarch64): CLAP and VST3.
