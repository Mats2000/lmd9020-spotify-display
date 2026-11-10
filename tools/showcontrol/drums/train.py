# Trains the live drum model (../DrumModel.swift) on recordings of real songs.
#
# For each recording: the measurements Drums.swift makes at every step (dumped by the test bench,
# rows of float32 [time, features...]) and an answer key of when each kick, snare and hat started
# (from the drums Demucs separates out, see key.py). A step is a hit's if it's within the first
# 12 ms after it started; steps 12-40 ms after are left out (finding it a little late isn't wrong,
# it's what the threshold and the gap between hits sort out). The thresholds are then chosen on
# the songs held out, with the same peak picking the app does, for the best balance of hits found
# and hits right.
#   python train.py out/DrumModel.swift [steps] rec1.feat rec1.key.json rec2.feat rec2.key.json ...
import json, sys
import numpy as np
import torch

FEATURES = 46
STEPS = int(sys.argv[2]) if sys.argv[2].isdigit() else 3  # steps the model sees: this one and the ones before
HIDDEN = 32
KINDS = ("kick", "snare", "hat")
GAP = (0.07, 0.07, 0.04)


def load(feat, key):
    rows = np.fromfile(feat, dtype=np.float32).reshape(-1, FEATURES + 1)
    t, x = rows[:, 0].astype(np.float64), rows[:, 1:]
    x = np.concatenate([np.roll(x, d, 0) for d in range(STEPS)], 1)  # this step and the ones before
    x[:STEPS] = 0
    k = json.load(open(key))
    y = np.zeros((len(t), 3), np.float32)
    w = np.ones((len(t), 3), np.float32)
    for c, name in enumerate(KINDS):
        on = np.array(k.get(name, []))
        if not len(on):
            continue
        j = np.searchsorted(on, t, side="right") - 1  # the latest hit at or before each step
        since = np.where(j >= 0, t - on[np.clip(j, 0, None)], 1e9)
        y[:, c] = (since >= 0) & (since < 0.012)
        w[(since >= 0.012) & (since < 0.04), c] = 0
    return t, x, y, w, k


def pick(t, p, threshold, gap):
    out, last = [], -1e9
    for i in np.nonzero(p >= threshold)[0]:
        if t[i] - last > gap:
            out.append(t[i]); last = t[i]
    return np.array(out)


def score(found, want):
    # A find counts for a hit it comes 10 ms before to 50 ms after (it's found, not where it starts).
    used, hits, lat = set(), 0, []
    for f in found:
        j = np.searchsorted(want, f - 0.05)
        while j < len(want) and want[j] < f + 0.01:
            if j not in used:
                used.add(j); hits += 1; lat.append(f - want[j]); break
            j += 1
    r, p = hits / max(1, len(want)), hits / max(1, len(found))
    return 2 * r * p / max(1e-9, r + p), r, p, (float(np.median(lat)) if lat else 0)


def main():
    out = sys.argv[1]
    first = 3 if sys.argv[2].isdigit() else 2
    sets = [load(sys.argv[i], sys.argv[i + 1]) for i in range(first, len(sys.argv), 2)]
    # Hold out the last song (or the last third, with only one) to choose thresholds and report.
    if len(sets) > 1:
        train, held = sets[:-1], [sets[-1]]
    else:
        t, x, y, w, k = sets[0]
        cut = int(len(t) * 2 / 3)
        train = [(t[:cut], x[:cut], y[:cut], w[:cut], k)]
        held = [(t[cut:], x[cut:], y[cut:], w[cut:], k)]
    X = np.concatenate([s[1] for s in train]); Y = np.concatenate([s[2] for s in train]); W = np.concatenate([s[3] for s in train])
    mean, std = X.mean(0), X.std(0) + 1e-6
    torch.manual_seed(0)
    net = torch.nn.Sequential(torch.nn.Linear(FEATURES * STEPS, HIDDEN), torch.nn.ReLU(), torch.nn.Linear(HIDDEN, 3))
    xs = torch.tensor((X - mean) / std, dtype=torch.float32)
    ys, ws = torch.tensor(Y), torch.tensor(W)
    pos = ys.mean(0).clamp(min=1e-4)
    weight = torch.where(ys > 0, (1 - pos) / pos * 0.5, torch.ones_like(ys)) * ws  # hits are rare: weigh them up
    opt = torch.optim.Adam(net.parameters(), lr=3e-3, weight_decay=1e-4)
    for epoch in range(60):
        perm = torch.randperm(len(xs))
        for b in range(0, len(xs), 4096):
            i = perm[b:b + 4096]
            loss = (torch.nn.functional.binary_cross_entropy_with_logits(net(xs[i]), ys[i], reduction="none") * weight[i]).mean()
            opt.zero_grad(); loss.backward(); opt.step()

    def predict(x):
        with torch.no_grad():
            return torch.sigmoid(net(torch.tensor((x - mean) / std, dtype=torch.float32))).numpy()

    thresholds, leads = [], []
    for c, name in enumerate(KINDS):
        best = (0, 0.5, 0, 0, 0)
        for th in np.linspace(0.2, 0.98, 40):
            f1s, rs, ps, ls = [], [], [], []
            for t, x, y, w, k in held:
                want = np.array([v for v in k.get(name, []) if t[0] + 0.05 < v < t[-1]])
                found = pick(t, predict(x)[:, c], th, GAP[c])
                f1, r, p, lat = score(found, want)
                f1s.append(f1); rs.append(r); ps.append(p); ls.append(lat)
            if np.mean(f1s) > best[0]:
                best = (np.mean(f1s), th, np.mean(rs), np.mean(ps), np.mean(ls))
        thresholds.append(float(best[1])); leads.append(best[4])
        print(f"{name:6} on held-out: found {100*best[2]:.0f}%, right {100*best[3]:.0f}%, known {1000*best[4]:.0f} ms after it started (threshold {best[1]:.2f})")

    W1, B1 = net[0].weight.detach().numpy(), net[0].bias.detach().numpy()
    W2, B2 = net[2].weight.detach().numpy(), net[2].bias.detach().numpy()
    arr = lambda v: "[" + ", ".join(f"{float(a):.6g}" for a in v) + "]"
    mat = lambda m: "[\n            " + ",\n            ".join(arr(r) for r in m) + "]"
    src = open(out).read()
    head = src[:src.index("    static let current: DrumModel?")]
    body = f"""    static let current: DrumModel? = DrumModel(
        mean: {arr(mean)},
        scale: {arr(1 / std)},
        w1: {mat(W1)},
        b1: {arr(B1)},
        w2: {mat(W2)},
        b2: {arr(B2)},
        thresholds: {arr(thresholds)},
        lead: {float(leads[0]):.4f},
        steps: {STEPS})
}}
"""
    open(out, "w").write(head + body)
    print("wrote", out)


main()
