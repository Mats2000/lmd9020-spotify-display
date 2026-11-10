import { forPhone } from "./_relay.js";

export default async function handler(req, res) {
    const state = await forPhone(req);
    if (!state) return res.status(401).json({ error: "key" });
    res.status(200).json(state);
}
