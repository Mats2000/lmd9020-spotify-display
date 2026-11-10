# The answer key, from a drums-only track: in each band, a hit is a sharp jump in energy (dB)
# over the quietest point of the 30 ms before, loud for that track. With bass and notes gone, a
# jump under 150 Hz is a kick; across 250 Hz-4 kHz with no kick under it a snare or clap; only
# above 7 kHz a hat. The drums come from Demucs:
#   python -m demucs -n htdemucs --two-stems=drums rec.wav
#   python key.py separated/htdemucs/rec/drums.wav rec.key.json [view.png start]
# The picture shows 4 s from `start` with the marks, to check the key by eye.
import json, sys
import numpy as np
import librosa

y, sr = librosa.load(sys.argv[1], sr=None, mono=True)
hop = 128
S = np.abs(librosa.stft(y, n_fft=1024, hop_length=hop)) ** 2
freqs = librosa.fft_frequencies(sr=sr, n_fft=1024)
times = librosa.frames_to_time(np.arange(S.shape[1]), sr=sr, hop_length=hop) + 1024 / sr / 2 - 0.004
fps = sr / hop

def energy(lo, hi):
    return 10 * np.log10(S[(freqs >= lo) & (freqs < hi)].sum(axis=0) + 1e-10)

def hits(E, jump, floor_db, gap):
    back = int(0.03 * fps)
    loud = np.percentile(E, 99) - floor_db
    rise = np.array([E[i] - E[max(0, i - back):i].min() if i else 0 for i in range(len(E))])
    out, last = [], -1e9
    i = 0
    while i < len(E):
        if rise[i] >= jump and E[i] >= loud and i - last > gap * fps:
            j = i + int(np.argmax(rise[i:i + int(0.02 * fps)]))   # the jump's steepest point
            start = i  # where it first crossed
            out.append(start)
            last = start
            i = j + int(0.03 * fps)
            continue
        i += 1
    return np.array(out, dtype=int), rise

low, body, mid, top = energy(30, 150), energy(200, 2000), energy(250, 4000), energy(7000, 16000)
# A kick reaches near the track's usual kick loudness: a tick lifting a silent low end doesn't count.
k, krise = hits(low, 9, 12, 0.07)
s, srise = hits(mid, 9, 20, 0.07)
h, hrise = hits(top, 8, 22, 0.035)
# A snare or clap has a body (200 Hz-2 kHz) about as loud as its top and isn't a kick's own click;
# a tick or a hat reaching down into the mids is mostly top.
s = np.array([f for f in s if body[f:f + 4].max() >= top[f:f + 4].max() - 3 and body[f] >= np.percentile(body, 99) - 15
              and (krise[max(0, f - 3):f + 6].max() < srise[max(0, f - 3):f + 6].max() + 3 or mid[f] > low[f] + 6)], dtype=int)
h = np.array([f for f in h if not len(s) or np.min(np.abs(s - f)) > 4], dtype=int)
key = {"kick": times[k].tolist(), "snare": times[s].tolist(), "hat": times[h].tolist()}
tempo, beats = librosa.beat.beat_track(y=y, sr=sr, hop_length=hop, units="time")
key["beats"] = list(map(float, beats))
json.dump(key, open(sys.argv[2], "w"))
print(f"{sys.argv[1]}: {len(k)} kicks, {len(s)} snares/claps, {len(h)} hats over {len(y)/sr:.0f} s")

if len(sys.argv) > 4:
    import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt; import librosa.display
    t0 = float(sys.argv[4]); a, b = int(t0 * sr), int((t0 + 4) * sr)
    D = librosa.amplitude_to_db(np.abs(librosa.stft(y[a:b], n_fft=2048, hop_length=128)), ref=np.max)
    fig, ax = plt.subplots(figsize=(16, 5))
    librosa.display.specshow(D, sr=sr, hop_length=128, x_axis="time", y_axis="log", ax=ax, cmap="magma")
    for name, c, yy in (("kick", "cyan", 40), ("snare", "lime", 600), ("hat", "yellow", 9000)):
        for x in key[name]:
            if t0 <= x < t0 + 4: ax.plot([x - t0] * 2, [30, 16000], color=c, lw=1.2); ax.text(x - t0, yy, name[0], color=c)
    ax.set_ylim(30, 16000); ax.set_title(f"drums {t0}-{t0+4} s: cyan kick, green snare, yellow hat")
    plt.tight_layout(); plt.savefig(sys.argv[3], dpi=70)
