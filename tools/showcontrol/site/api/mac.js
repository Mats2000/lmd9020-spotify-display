// Show Control's side: it posts its state (and its sign-in check when that changes) and gets the
// taps waiting, holding on up to `wait` seconds for one when there are none.
import { redis, blpop, isMac, body } from "./_relay.js";

export default async function handler(req, res) {
    if (req.method !== "POST" || !isMac(req)) return res.status(401).end();
    const b = body(req);
    const p = redis.pipeline();
    if (b.state) p.set("sc:state", JSON.stringify(b.state), { ex: 300 });
    if (b.auth?.none) p.del("sc:auth");
    else if (b.auth) p.set("sc:auth", JSON.stringify(b.auth));
    p.lrange("sc:taps", 0, -1);
    p.del("sc:taps");
    const out = await p.exec();
    let taps = out[out.length - 2] ?? [];
    const wait = Math.max(0, Math.min(25, Number(b.wait) || 0));
    if (!taps.length && wait > 0) {
        const got = await blpop("sc:taps", wait);
        if (got) taps = [got];
    }
    res.status(200).json({ taps: taps.map(t => (typeof t === "string" ? JSON.parse(t) : t)) });
}
