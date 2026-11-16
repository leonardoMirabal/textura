Settings and scenes now survive a power-off; a power cut during a save keeps the last good copy; the store moved clear of the stock firmware and of Frodhes; smoother effects (no clicks on MASTER, REVERB MOD, REVERB DARK, DRIVE on/off, GRAIN REVERSE); GRAIN DELAY SYNC repeats stay on the beat.

- Settings (last scene, MIDI channels, CLOCK, COLOR, SCALE, EFFECTS ON LOAD) are saved on the FM-1; before, they were lost at every power-off.
- Settings are kept in two copies: a power cut in the middle of a save keeps the older one.
- Scenes and settings live in their own flash place (0xAE000-0xB9FFF), clear of the stock firmware V16 and of Frodhes' store. Scenes saved with an earlier version are not carried over.
- Updates stage in a free area chosen per unit.
- A scene save no longer freezes the screen.
- GRAIN DELAY: with SYNC on, every repeat lands on the beat; GRAINS 1 keeps both read heads when the CPU is tight; no gap when SYNC or GRAIN 0 is switched; no step when REVERSE leaves 0; no dip or swell when GRAINS moves.
- MASTER: the volume ramps while the knob turns (no clicks).
- REVERB: MOD moves without clicks on every type; INFINITE to DARK without a click.
- DRIVE: switching it on no longer bumps the level.
- USB: USB RESCUE works with a busy MIDI input; USB audio keeps every packet while the FM-1 saves; the 48 kHz stream no longer clips on loud passages.
