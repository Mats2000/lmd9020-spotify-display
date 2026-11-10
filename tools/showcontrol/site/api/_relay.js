// The relay between the phone page and Show Control on the Mac. Redis keeps the Mac's latest
// state (sc:state), its sign-in check (sc:auth: the password's salt and PBKDF2 hash, and the key
// generation), the taps waiting for the Mac (sc:taps) and whether a phone is looking (sc:watch).
import { Redis } from "@upstash/redis";
import crypto from "node:crypto";

const restURL = process.env.KV_REST_API_URL ?? process.env.UPSTASH_REDIS_REST_URL;
const restToken = process.env.KV_REST_API_TOKEN ?? process.env.UPSTASH_REDIS_REST_TOKEN;
export const redis = new Redis({ url: restURL, token: restToken });

// A blocking pop (the client library has none): waits up to `seconds` for something in the list.
export async function blpop(key, seconds) {
    const r = await fetch(restURL, { method: "POST", headers: { Authorization: `Bearer ${restToken}` },
                                     body: JSON.stringify(["BLPOP", key, String(seconds)]) });
    const j = await r.json();
    return j.result ? j.result[1] : null;
}
const secret = process.env.MAC_SECRET ?? "";
const keyKey = crypto.createHash("sha256").update("phone keys:" + secret).digest();

export const same = (a, b) => {
    const x = Buffer.from(String(a)), y = Buffer.from(String(b));
    return x.length === y.length && crypto.timingSafeEqual(x, y);
};
export const isMac = req => secret.length >= 32 && same(req.headers["x-mac-secret"] ?? "", secret);

// A phone's key: the generation it was signed in under, a random part, and a signature.
const sign = s => crypto.createHmac("sha256", keyKey).update(s).digest("hex");
export const newKey = epoch => {
    const s = `${epoch}.${crypto.randomBytes(16).toString("hex")}`;
    return `${s}.${sign(s)}`;
};
export const keyEpoch = key => {
    const [e, r, sig] = String(key ?? "").split(".");
    return e && r && sig && same(sig, sign(`${e}.${r}`)) ? Number(e) : null;
};

export const derive = (password, salt) => crypto.pbkdf2Sync(password, Buffer.from(salt, "base64"), 200000, 32, "sha256");

export const body = req => (typeof req.body === "object" && req.body) || (() => { try { return JSON.parse(req.body); } catch { return {}; } })();

// The state for a phone with a valid key, or null. A phone looking wakes the Mac (every 10 s).
export async function forPhone(req) {
    const [state, auth, watch] = await redis.mget("sc:state", "sc:auth", "sc:watch");
    const epoch = keyEpoch(req.headers["x-show-key"]);
    if (!auth || epoch === null || epoch !== auth.epoch) return null;
    if (!watch) await redis.pipeline().rpush("sc:taps", JSON.stringify({ action: "wake" })).set("sc:watch", 1, { ex: 10 }).exec();
    return state ?? { title: "", artist: "", status: "Waiting for the Mac…", lampsOn: false, fixtures: [], presets: [], scenes: [],
                      styles: [], schemes: [], movements: [], sources: [], directions: [], style: "", scheme: "" };
}
