// A tap from the phone, queued for the Mac (which is waiting on the queue, so it hears it at once).
import { redis, forPhone, body } from "./_relay.js";

export default async function handler(req, res) {
    if (req.method !== "POST") return res.status(405).end();
    const state = await forPhone(req);
    if (!state) return res.status(401).json({ error: "key" });
    const tap = body(req);
    if (typeof tap.action !== "string" || JSON.stringify(tap).length > 4000) return res.status(400).end();
    await redis.rpush("sc:taps", JSON.stringify(tap));
    res.status(200).json(state);
}
