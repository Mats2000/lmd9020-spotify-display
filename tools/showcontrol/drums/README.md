# The live drum model

Show Control finds kicks (and snares and hats, as detail) with a small model learned from real
songs, not with hand-written rules: `Drums.swift` measures the newest sound every 2.7 ms (energy
in nine bands, how much each rose over 11 and 32 ms, how loud against its own recent peak) and
`DrumModel.swift` (written by `train.py`) turns the last few steps' measurements into the chance
that a hit just started. It's a few thousand multiply-adds a step, with no delay of its own beyond
the step.

The model learns against an answer key made offline, with hindsight, from the song's drums alone:

1. Record what plays: `defaults write com.sentiainstruments.lmd-visualizer recordSeconds -int 600`,
   then restart Show Control. The next 10 minutes of sound it hears go to
   `~/Library/Logs/Show Control/recording-<time>.wav`. Set it back to 0 after.
2. Separate the drums (Demucs, `pip install demucs`):
   `python -m demucs -n htdemucs --two-stems=drums recording.wav`
3. Mark the hits: `python key.py separated/htdemucs/recording/drums.wav recording.key.json view.png 20`
   (check `view.png`: 4 s with the marks over the drums).
4. Dump the app's measurements for each recording with the test bench
   (`swiftc -O -parse-as-library -o bench bench.swift ../Drums.swift ../DrumModel.swift`, then
   `./bench recording.wav hits.json recording.feat`), and train on all of them (the last one is
   held out to choose thresholds and report how well it does), then rebuild the app:
   `python train.py ../DrumModel.swift 3 rec1.feat rec1.key.json rec2.feat rec2.key.json ...`

Needs `librosa`, `torch` and `demucs` in a Python environment of their own.

## Kick or bass note

The model alone takes some bass notes for kicks: at the moment a note starts, a sub-heavy kick and
a bass note look the same in its bands. So each kick it finds is judged:

- **A click** (the top bands, 600 Hz to 8 kHz, rising 9 dB or more within the last few steps): the
  beater. It goes out at once.
- **No click:** it waits 43 ms, watching the low end (25-180 Hz) as a waveform, cycle by cycle. A
  kick falls in pitch fast (about 150 Hz down to 50), so its cycles lengthen: the last against the
  first is 1.4-1.7 on clean kicks. A bass note holds its pitch: 1.0. At 1.25 or more it's a kick,
  else a bass note. Shorter waits don't separate them (at 25 ms clean kicks read 1.1-1.3).
- **Within 0.1 s of the last kick** it's never a new kick: a bass note under it, or the same kick
  found twice.

A bass note goes out as `.bass` and swells instead of flashing; it doesn't step a movement or
count as a beat. The Govee lamp rises over ~0.1 s, then rides the sub as it fades. A Matter bulb in
Pulse or Wash goes part way up its rhythm over 0.2 s, holds 0.2 s and falls over 0.9 s (skipped
while it's still busy with a kick, or its commands queue). The Mac screen brightens all over,
fading with the bass. TILES and the Launchkey take kicks only.

Measured on two recordings (13 minutes): the clean four-to-the-floor stretches keep every kick, a
sustained bass line loses its false kicks, and a busy bass-and-kick section comes out kick, note,
kick, note as the spectrogram shows it. The level-based measures tried first don't work: how far
the low end falls in 43 ms is near 0 for kicks too, and a band ratio is too coarse at 47 Hz a bin.
A loud bass under a kick can hide its pitch fall, which is why a click alone is enough.
