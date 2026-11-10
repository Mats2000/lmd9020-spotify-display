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
