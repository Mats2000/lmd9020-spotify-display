// Show Control's Matter side: joins Matter lights as an extra controller (next to Alexa or Apple
// Home), then drives them for the app.
//   node lights.js pair <11-digit code> [name]   the code from the other app's "share" / "add another"
//   node lights.js list
//   node lights.js serve                         takes {"id","r","g","b","level","fade","kick"} on UDP 127.0.0.1:4005
//                                                ({"save"} before a show, {"restore"} after, {"release"}/{"take"});
//                                                no "id": every light. Prints "Released: <id>" when someone else
//                                                (Alexa) takes a light, and leaves it alone until {"take"}.
//   node lights.js forget <name>
// Matter's storage (the fabric and its keys) lives in matter.js's default place, not in the repo.
import { ServerNode } from "@matter/main";
import { ColorControlClient } from "@matter/main/behaviors/color-control";
import { LevelControlClient } from "@matter/main/behaviors/level-control";
import { OnOffClient } from "@matter/main/behaviors/on-off";
import dgram from "node:dgram";

const controller = await ServerNode.create(ServerNode.RootEndpoint, { id: "show-control" });
const [command, ...args] = process.argv.slice(2);

const lights = () => [...controller.peers].filter(p => p.id !== undefined);

// The first endpoint with a colour or level control: the light itself.
function lightPart(peer) {
    for (const part of peer.parts) {
        if (part.stateOf(ColorControlClient) !== undefined || part.stateOf(LevelControlClient) !== undefined) return part;
    }
    return peer.parts.get(1);
}

function rgbToHueSat(r, g, b) {
    const mx = Math.max(r, g, b), mn = Math.min(r, g, b), c = mx - mn;
    let h = 0;
    if (c > 0) h = mx === r ? ((g - b) / c) % 6 : mx === g ? (b - r) / c + 2 : (r - g) / c + 4;
    h = (h * 60 + 360) % 360;
    return { hue: Math.round((h / 360) * 254), saturation: Math.round((mx ? c / mx : 0) * 254) };
}

switch (command) {
    case "pair": {
        const [pairingCode, name = `light-${Date.now() % 100000}`] = args;
        if (!pairingCode) throw new Error("Usage: node lights.js pair <code> [name]");
        await controller.peers.commission({ id: name, pairingCode: pairingCode.replace(/\D/g, "") });
        console.log(`Paired ${name}`);
        break;
    }
    case "list":
        for (const peer of lights()) console.log(peer.id);
        break;
    case "forget": {
        const peer = controller.peers.get(args[0]);
        if (!peer) throw new Error(`No light called ${args[0]}`);
        await peer.start();
        await peer.decommission();
        console.log(`Forgot ${args[0]}`);
        break;
    }
    case "serve": {
        await controller.start();
        const all = new Map();  // id -> one light's part and show state
        for (const peer of lights()) {
            await peer.start();
            all.set(peer.id, { part: lightPart(peer), busy: false, next: null, saved: null, driving: false, released: false, sent: [] });
            console.log(`Ready: ${peer.id}`);
        }
        const targets = cmd => cmd.id ? [all.get(cmd.id)].filter(Boolean) : [...all.values()];

        const apply = async (light, cmd, isKick = false) => {
            if (!isKick) light.busy = true;
            const part = light.part, fade = Math.round((cmd.fade ?? 0.3) * 10);  // tenths of a second
            try {
                if (cmd.level !== undefined && part.stateOf(LevelControlClient) !== undefined) {
                    const level = Math.max(1, Math.min(254, Math.round(cmd.level * 254)));
                    light.sent.push({ level, at: Date.now() });
                    await part.commandsOf(LevelControlClient).moveToLevelWithOnOff({
                        level, transitionTime: fade, optionsMask: {}, optionsOverride: {},
                    });
                }
                if (cmd.r !== undefined && part.stateOf(ColorControlClient) !== undefined) {
                    const hs = rgbToHueSat(cmd.r, cmd.g, cmd.b);
                    light.sent.push({ hue: hs.hue, at: Date.now() });
                    await part.commandsOf(ColorControlClient).moveToHueAndSaturation({
                        ...hs, transitionTime: fade, optionsMask: {}, optionsOverride: {},
                    });
                }
            } catch (e) {
                console.error(`${e}`);
            }
            if (isKick) return;
            light.busy = false;
            if (light.next && light.driving) {  // only the latest waiting command: late ones are stale
                const n = light.next;
                light.next = null;
                apply(light, n);
            }
        };

        // Each light as it was before the show, put back after it (and when the app quits).
        const save = light => {
            if (light.saved || light.released) return;
            const p = light.part;
            light.saved = {
                on: p.stateOf(OnOffClient)?.onOff, level: p.stateOf(LevelControlClient)?.currentLevel,
                colour: p.stateOf(ColorControlClient) && { ...p.stateOf(ColorControlClient) },
            };
            light.driving = true;
            light.seenOn = false;
            light.seenHue = false;
        };
        const restore = async light => {
            light.driving = false;
            light.next = null;
            const s = light.saved;
            light.saved = null;
            if (!s || light.released) return;
            const part = light.part, opts = { transitionTime: 5, optionsMask: {}, optionsOverride: {} };
            try {
                const c = s.colour, cc = part.commandsOf(ColorControlClient);
                if (c?.colorMode === 2 && c.colorTemperatureMireds) await cc.moveToColorTemperature({ colorTemperatureMireds: c.colorTemperatureMireds, ...opts });
                else if (c?.colorMode === 1 && c.currentX !== undefined) await cc.moveToColor({ colorX: c.currentX, colorY: c.currentY, ...opts });
                else if (c?.currentHue !== undefined) await cc.moveToHueAndSaturation({ hue: c.currentHue, saturation: c.currentSaturation, ...opts });
                if (s.level) await part.commandsOf(LevelControlClient).moveToLevelWithOnOff({ level: s.level, ...opts });
                if (s.on === false) await part.commandsOf(OnOffClient).off();
            } catch (e) {
                console.error(`${e}`);
            }
        };
        const release = (id, light, why) => {
            if (light.released) return;
            light.released = true;
            light.driving = false;
            light.saved = null;  // the light is someone else's now: nothing to put back
            light.next = null;
            console.log(`Released: ${id} ${why}`);
        };

        // Someone else (Alexa, a routine, the switch) changed a light mid-show: let it go until the app
        // takes it back. Off, a white temperature, or a hue the show didn't send.
        setInterval(() => {
            const now = Date.now();
            for (const [id, light] of all) {
                light.sent = light.sent.filter(x => now - x.at < 6000);
                if (!light.driving || light.released) continue;
                const p = light.part, on = p.stateOf(OnOffClient)?.onOff, c = p.stateOf(ColorControlClient);
                if (on === true) light.seenOn = true;
                if (light.seenOn && on === false) { release(id, light, "off"); continue; }
                if (!c) continue;
                if (c.colorMode === 0) light.seenHue = true;
                if (light.seenHue && c.colorMode === 2) { release(id, light, "white"); continue; }
                const hues = light.sent.filter(x => x.hue !== undefined);
                const lastHueAt = Math.max(0, ...hues.map(x => x.at));
                const far = h => Math.min(Math.abs(h - c.currentHue), 254 - Math.abs(h - c.currentHue)) > 25;
                if (light.seenHue && c.colorMode === 0 && hues.length && now - lastHueAt > 1500 && hues.every(x => far(x.hue)))
                    release(id, light, "colour");
            }
        }, 700);

        for (const signal of ["SIGTERM", "SIGINT"]) process.on(signal, async () => {
            await Promise.race([Promise.all([...all.values()].map(restore)), new Promise(r => setTimeout(r, 3000))]);
            process.exit(0);
        });

        const sock = dgram.createSocket("udp4");
        sock.on("message", data => {
            let cmd;
            try { cmd = JSON.parse(data.toString()); } catch { return; }
            for (const light of targets(cmd)) {
                const id = [...all].find(([, l]) => l === light)[0];
                if (cmd.save) save(light);
                else if (cmd.restore) restore(light);
                else if (cmd.release) release(id, light, "app");
                else if (cmd.take) {
                    if (light.released) console.log(`Taken: ${id}`);
                    light.released = false;
                } else if (!light.driving || light.released) continue;  // nothing drives a light outside its show
                else if (cmd.kick) apply(light, cmd, true);  // a kick goes now, alongside whatever is under way
                else if (light.busy) light.next = cmd;
                else apply(light, cmd);
            }
        });
        sock.bind(4005, "127.0.0.1");
        console.log("Listening on 127.0.0.1:4005");
        break;
    }
    default:
        console.log("Commands: pair <code> [name], list, serve, forget <name>");
}
if (command !== "serve") process.exit(0);
