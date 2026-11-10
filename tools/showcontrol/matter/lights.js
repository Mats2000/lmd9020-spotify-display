// Show Control's Matter side: joins Matter lights as an extra controller (next to Alexa or Apple
// Home), then drives them for the app.
//   node lights.js pair <11-digit code> [name]   the code from the other app's "share" / "add another"
//   node lights.js list
//   node lights.js serve                         takes {"r","g","b","level","fade","kick"} on UDP 127.0.0.1:4005,
//                                                {"save":true} before a show and {"restore":true} after
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
        const parts = new Map();
        for (const peer of lights()) {
            await peer.start();
            parts.set(peer.id, lightPart(peer));
            console.log(`Ready: ${peer.id}`);
        }
        let busy = false, next = null;
        const apply = async (cmd, isKick = false) => {
            if (!isKick) busy = true;
            const fade = Math.round((cmd.fade ?? 0.3) * 10);  // tenths of a second
            await Promise.all([...parts.values()].map(async part => {
                try {
                    if (cmd.on === false) return void (await part.commandsOf(OnOffClient).off());
                    if (cmd.level !== undefined && part.stateOf(LevelControlClient) !== undefined)
                        await part.commandsOf(LevelControlClient).moveToLevelWithOnOff({
                            level: Math.max(1, Math.min(254, Math.round(cmd.level * 254))), transitionTime: fade,
                            optionsMask: {}, optionsOverride: {},
                        });
                    if (cmd.r !== undefined && part.stateOf(ColorControlClient) !== undefined)
                        await part.commandsOf(ColorControlClient).moveToHueAndSaturation({
                            ...rgbToHueSat(cmd.r, cmd.g, cmd.b), transitionTime: fade, optionsMask: {}, optionsOverride: {},
                        });
                } catch (e) {
                    console.error(`${e}`);
                }
            }));
            if (isKick) return;
            busy = false;
            if (next) {  // only the latest waiting command: lights are slow, and late commands are stale
                const n = next;
                next = null;
                apply(n);
            }
        };
        // Each light as it was before the show, put back after it (and when the app quits).
        let saved = null;
        const save = () => {
            if (saved) return;
            saved = new Map([...parts].map(([id, part]) => [id, {
                on: part.stateOf(OnOffClient)?.onOff, level: part.stateOf(LevelControlClient)?.currentLevel,
                colour: part.stateOf(ColorControlClient) && { ...part.stateOf(ColorControlClient) },
            }]));
        };
        const restore = async () => {
            if (!saved) return;
            const was = saved;
            saved = null;
            busy = true;
            next = null;
            await Promise.all([...parts].map(async ([id, part]) => {
                const s = was.get(id);
                if (!s) return;
                const opts = { transitionTime: 5, optionsMask: {}, optionsOverride: {} };
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
            }));
            busy = false;
        };
        for (const signal of ["SIGTERM", "SIGINT"]) process.on(signal, async () => {
            await Promise.race([restore(), new Promise(r => setTimeout(r, 3000))]);
            process.exit(0);
        });

        const sock = dgram.createSocket("udp4");
        sock.on("message", data => {
            let cmd;
            try { cmd = JSON.parse(data.toString()); } catch { return; }
            if (cmd.save) save();
            else if (cmd.restore) restore();
            else if (!saved) return;  // nothing drives the lights outside a show
            else if (cmd.kick) apply(cmd, true);  // a kick goes now, alongside whatever is under way
            else if (busy) next = cmd;
            else apply(cmd);
        });
        sock.bind(4005, "127.0.0.1");
        console.log("Listening on 127.0.0.1:4005");
        break;
    }
    default:
        console.log("Commands: pair <code> [name], list, serve, forget <name>");
}
if (command !== "serve") process.exit(0);
