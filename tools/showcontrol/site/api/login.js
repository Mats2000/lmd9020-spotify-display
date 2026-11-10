// Sign in with the password set on the Mac. Five wrong tries lock it for 30 s, doubling each time.
import { redis, derive, newKey, same, body } from "./_relay.js";

export default async function handler(req, res) {
    if (req.method !== "POST") return res.status(405).end();
    const [auth, locked] = await redis.mget("sc:auth", "sc:lock");
    if (!auth) return res.status(403).json({ error: "nopass" });
    if (locked) return res.status(429).json({ error: "wait" });
    const password = String(body(req).password ?? "");
    const got = derive(password, auth.salt).toString("base64");
    if (!same(got, auth.hash)) {
        const fails = await redis.incr("sc:fails");
        await redis.expire("sc:fails", 3600);
        if (fails >= 5) await redis.set("sc:lock", 1, { ex: Math.min(3600, 30 * 2 ** (fails - 5)) });
        return res.status(401).json({ error: "wrong" });
    }
    await redis.del("sc:fails");
    res.status(200).json({ key: newKey(auth.epoch) });
}
