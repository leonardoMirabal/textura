// SPDX-License-Identifier: GPL-3.0-only
// The panel in a web page, around the Textura firmware compiled to WebAssembly (web_hal.c). From Frodhes' web/app.js.
"use strict";

const BTN = { "OCT−": 0, "OCT+": 1, FX: 2, SEL: 3, ENV: 4, LFO: 5, EDIT: 6, GLO: 7, HOME: 8, SAVE: 9, ARP: 10, SEQ: 11, PLAY: 12, REC: 13 };
const BTN_ORDER = ["HOME", "EDIT", "ENV", "LFO", "SEQ", "SEL", "ARP", "GLO", "FX", "SAVE", "PLAY", "REC", "OCT−", "OCT+"];
const ENCS = [["SELECT", 0], ["ALGORITHM", 1], ["PRESETS", 6], ["K1", 2], ["K2", 3], ["K3", 4], ["K4", 5]];
const NKEYS = 27;                                   // F3 .. G5
const KEYMAP = "awsedrfgyhujkolp;".split("");       // F3, F#3, G3 ... chromatic
const STORE_KEY = "textura.settings";
const SCENES_KEY = "textura.scenes";                 // the scene banks (web_hal.c fl_window_*)
const b64 = b => { let s = ""; for (let i = 0; i < b.length; i += 8192) s += String.fromCharCode(...b.subarray(i, i + 8192)); return btoa(s); };

let M = null, ctx = null, node = null, keysDown = 0, keysHeld = new Set();

function $(id) { return document.getElementById(id); }
function status(t) { $("status").textContent = t; }

// ---- the version (web/build.sh writes version.js: the release's X.Y.Z, or "dev" for a local build)
const VERSION = (typeof window.TEXTURA_VERSION === "string" && window.TEXTURA_VERSION) || "dev";
$("ver").textContent = VERSION;
$("ver-foot").textContent = VERSION;
document.title = `Textura ${VERSION}`;

// ---- the module (Emscripten, MODULARIZE off: a global Module)
var Module = window.Module = {
  onRuntimeInitialized() {
    M = Module;
    try {                                            // the settings record, before main() reads it
      const s = localStorage.getItem(STORE_KEY);
      if (s) {
        const b = Uint8Array.from(atob(s), c => c.charCodeAt(0));
        M.HEAPU8.set(b.subarray(0, 260), M._web_store());
      }
      const w = localStorage.getItem(SCENES_KEY);
      if (w) {
        const b = Uint8Array.from(atob(w), c => c.charCodeAt(0));
        if (b.length === M._web_flw_len()) M.HEAPU8.set(b, M._web_flw_buf());
      }
    } catch (e) { /* private window: no storage */ }
    M._web_master(master);
    requestAnimationFrame(frame);
  },
  print: t => console.log(t), printErr: t => console.warn(t),
};

// ---- the screen and the LEDs, every animation frame
// RGB565 -> the framebuffer's own colours (the default since 2026-10-18: the camera-calibrated panel model's yellow is
// not what the eye sees); ?look shows them through that "device look" of tools/manual_shots.py --device-look (the
// dark tones lifted, the hue towards yellow; tools/display_cal.py)
const LOOK = { g: [1.0907, 0.8288, 0.9032], m: [[2.4821, 0.5194, -0.2992], [0.1402, 3.4473, 0.2092], [0.0378, 0.0352, 3.4810]],
               f: [0.0356, 0.0340, 0.0583] };
const RGB = new Uint32Array(65536), RAW = !/[?&]look\b/.test(location.search);   // 565 -> RGBA bytes (little-endian)
for (let v = 0; v < 65536; v++) {
  const x = [((v >> 11) & 31) / 31, ((v >> 5) & 63) / 63, (v & 31) / 31], o = [0, 0, 0];
  for (let c = 0; c < 3; c++) {
    if (RAW) { o[c] = Math.round(x[c] * 255); continue; }
    let l = LOOK.f[c];
    for (let k = 0; k < 3; k++) l += LOOK.m[c][k] * Math.pow(x[k], LOOK.g[k]);
    o[c] = Math.round(255 * Math.pow(Math.min(1, Math.max(0, l)), 1 / 2.2));
  }
  RGB[v] = o[0] | o[1] << 8 | o[2] << 16 | 0xFF000000;
}
let img = null;
function frame() {
  const c = $("screen"), g = c.getContext("2d");
  if (!img) img = g.createImageData(240, 240);
  const fb = new Uint16Array(M.HEAPU8.buffer, M._web_fb(), 240 * 240), d = new Uint32Array(img.data.buffer);
  for (let i = 0; i < 240 * 240; i++) d[i] = RGB[fb[i]];
  g.putImageData(img, 0, 0);
  const led = new Uint8Array(M.HEAPU8.buffer, M._web_leds(), 64);
  for (const b of document.querySelectorAll("button.b")) b.classList.toggle("on", led[+b.dataset.id] === 1);
  for (const k of document.querySelectorAll("#kb > .key")) {
    const v = led[14 + (+k.dataset.k)];
    k.classList.toggle("lit", v === 1);
    k.classList.toggle("glow", v === 2);
  }
  captions();
  if (M._web_store_take_dirty()) {
    try {
      const b = new Uint8Array(M.HEAPU8.buffer, M._web_store(), 260);
      localStorage.setItem(STORE_KEY, btoa(String.fromCharCode(...b)));
    } catch (e) { }
  }
  if (M._web_flw_take_dirty()) {
    try { localStorage.setItem(SCENES_KEY, b64(new Uint8Array(M.HEAPU8.buffer, M._web_flw_buf(), M._web_flw_len()))); } catch (e) { }
  }
  requestAnimationFrame(frame);
}

// ---- sound: the audio callback is the FM-1's audio interrupt (256-frame blocks at 44.1 kHz)
// the engine runs at 44.1 kHz; a phone that will not open a 44.1 kHz context gets the engine resampled (linear)
const ENG = 44100;
let fifo = new Float32Array(2 * 8192), fifoN = 0, frac = 0;
function pull(frames) {                              // engine frames into the fifo (L R interleaved)
  const blocks = Math.ceil(frames / 256), p = M._web_audio(blocks) >> 2, f = M.HEAPF32;
  fifo.set(f.subarray(p, p + 2 * 256 * blocks), 2 * fifoN);
  fifoN += 256 * blocks;
}
function startAudio() {                              // synchronous up to resume(): iOS wants it inside the tap
  try {
    if (navigator.audioSession) navigator.audioSession.type = "playback";   // iPhone: play with the silent switch on
  } catch (e) { }
  if (ctx) { if (ctx.state !== "running") ctx.resume(); return; }
  const AC = window.AudioContext || window.webkitAudioContext;
  try { ctx = new AC({ sampleRate: ENG, latencyHint: "interactive" }); } catch (e) { ctx = new AC(); }
  const ratio = ENG / ctx.sampleRate;
  node = ctx.createScriptProcessor(2048, 0, 2);
  node.onaudioprocess = ev => {
    const L = ev.outputBuffer.getChannelData(0), R = ev.outputBuffer.getChannelData(1), n = L.length;
    if (!M) { L.fill(0); R.fill(0); return; }
    if (ratio === 1) {
      const p = M._web_audio(n / 256) >> 2, f = M.HEAPF32;
      for (let i = 0; i < n; i++) { L[i] = f[p + 2 * i]; R[i] = f[p + 2 * i + 1]; }
      return;
    }
    const need = Math.ceil(n * ratio + frac) + 2;
    if (fifoN < need) pull(need - fifoN);
    let pos = frac;
    for (let i = 0; i < n; i++, pos += ratio) {
      const k = pos | 0, t = pos - k;
      L[i] = fifo[2 * k] + (fifo[2 * k + 2] - fifo[2 * k]) * t;
      R[i] = fifo[2 * k + 1] + (fifo[2 * k + 3] - fifo[2 * k + 1]) * t;
    }
    const used = pos | 0;
    frac = pos - used;
    fifo.copyWithin(0, 2 * used, 2 * fifoN);
    fifoN -= used;
  };
  node.connect(ctx.destination);
  const unlock = new Audio("data:audio/wav;base64,UklGRiQAAABXQVZFZm10IBAAAAABAAEARKwAAIhYAQACABAAZGF0YQAAAAA=");
  unlock.play().catch(() => { });                   // older iPhones: an <audio> play in the tap opens the audio session
  ctx.resume().then(() => {
    status(`Sound on (${ctx.sampleRate} Hz${ratio !== 1 ? ", resampled" : ""}). Play the keys, the computer keyboard or a MIDI keyboard.`);
    $("start").textContent = "Sound on";
  }, e => status("Sound could not start: " + e));
}
window.addEventListener("error", e => status("Error: " + e.message));
window.addEventListener("unhandledrejection", e => status("Error: " + e.reason));
$("start").onclick = startAudio;

// ---- the panel, laid out as the FM-1 (device coordinates, 1200 x 700; fit() scales it to the window)
const KNOBS = [["MASTER", -1, 50, 22, "VOLUME"], ["SELECT", 0, 200, 22, "PAGES"], ["PRESETS", 6, 50, 152, "SCENES"],
               ["ALGORITHM", 1, 200, 152, "LANDSCAPE·TYPE"], ["KNOB 1", 2, 685, 22, "CARD 1"], ["KNOB 2", 3, 800, 22, "CARD 2"],
               ["KNOB 3", 4, 915, 22, "CARD 3"], ["KNOB 4", 5, 1030, 22, "CARD 4"]];
// the buttons' captions (the ones still to come are blank: envelopes and scenes)
const HINT = { FX: "EFFECTS", SEL: "KEY MODE", ENV: "LEARN · AGAIN: ENV", LFO: "LEARN · AGAIN: LFO", EDIT: "SOUND", GLO: "TEMPO", HOME: "LANDSCAPE", SAVE: "SCENE · HOLD: UNDO", ARP: "MATRIX",
               SEQ: "", PLAY: "TAP · HOLD: RUN", REC: "ALL ON·OFF", "OCT−": "LANE", "OCT+": "LANE" };
const HINT_ON = {                                    // what a button does on a given page (else HINT)
  LANDSCAPE: { HOME: "BACK" }, SOUND: { EDIT: "DRIVE" }, DRIVE: { EDIT: "FILTER" }, COMPRESSOR: { FX: "GRAIN" }, GRAIN: { FX: "REVERB" }, TEMPO: { GLO: "MIDI" }, MIDI: { GLO: "DISPLAY" },
  LFO: { LFO: "NEXT" }, ENV: { ENV: "NEXT" },
};
let shownPage = "";
function captions() {                                // the page on the screen: the buttons' captions
  const p = M.HEAPU8, a = M._web_page();
  let page = ""; for (let i = a; p[i] && i < a + 16; i++) page += String.fromCharCode(p[i]);
  if (page === shownPage) return;
  shownPage = page;
  for (const b of document.querySelectorAll("button.b")) {
    const n = b.dataset.name, sm = b.querySelector("small");
    if (sm) sm.textContent = (HINT_ON[page] && HINT_ON[page][n]) || HINT[n];
  }
}
const ROWS = [["FX", "SEL", "ENV", "LFO", "EDIT", "GLO"], ["HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC"]];
let master = 3500;
function knob(name, id, x, y, sub) {
  const k = document.createElement("div");
  k.className = "knob" + (id >= 0 ? " endless" : "");
  k.style.left = x + "px"; k.style.top = y + "px";
  k.innerHTML = `<div class="lab">${name}</div><div class="dial"></div><div class="sub">${sub}</div>`;
  const dial = k.children[1];
  let angle = 0, lastY = null, acc = 0;
  const show = () => dial.style.setProperty("--a", (id < 0 ? -135 + 270 * master / 4096 : angle) + "deg");
  const turn = d => {
    if (!d) return;
    if (id < 0) { master = Math.max(0, Math.min(4096, master + d * 128)); if (M) M._web_master(master); }
    else { if (!M) return; M._web_enc(id, d); angle += d * 15; }
    show();
  };
  // the wheel / trackpad: steps from the distance scrolled (a trackpad sends many small events per swipe), and the
  // steps go out at most a few per frame so the firmware's encoder acceleration only answers a real fast turn
  let wacc = 0, pend = 0, raf = 0;
  const flush = () => { raf = 0; const n = Math.max(-2, Math.min(2, pend)); pend -= n; for (let i = 0; i < Math.abs(n); i++) turn(Math.sign(n)); if (pend) raf = requestAnimationFrame(flush); };
  const queue = n => { pend = Math.max(-12, Math.min(12, pend + n)); if (!raf) raf = requestAnimationFrame(flush); };
  k.addEventListener("wheel", e => {
    e.preventDefault();
    const px = e.deltaMode === 1 ? e.deltaY * 50 : e.deltaMode === 2 ? e.deltaY * 400 : e.deltaY;
    wacc -= px;
    const STEP = 50;
    if (Math.abs(wacc) >= STEP) { const n = Math.trunc(wacc / STEP); wacc -= n * STEP; queue(n); }
  }, { passive: false });
  k.addEventListener("pointerdown", e => { e.preventDefault(); lastY = e.clientY; acc = 0; k.setPointerCapture(e.pointerId); });
  k.addEventListener("pointermove", e => {
    if (lastY === null) return;
    acc += lastY - e.clientY; lastY = e.clientY;
    const n = Math.trunc(acc / 10); if (n) { acc -= n * 10; queue(n); }
  });
  k.addEventListener("pointerup", () => { lastY = null; wacc = 0; });
  k.addEventListener("pointercancel", () => { lastY = null; });
  show();
  return k;
}
function button(name, id, x, y, cls) {
  const b = document.createElement("button");
  b.className = "b" + (cls ? " " + cls : ""); b.dataset.id = id; b.dataset.name = name;
  b.style.left = x + "px"; b.style.top = y + "px";
  b.innerHTML = (name === "PLAY" ? "PLAY/STOP" : name) + `<small>${HINT[name] || ""}</small>`;
  if (name === "PLAY") b.classList.add("small");
  const down = e => { e.preventDefault(); if (!M) return; startAudio(); M._web_btn(id, 1); b.classList.add("down"); try { b.setPointerCapture(e.pointerId); } catch (x) { } };
  const up = () => { if (!M || !b.classList.contains("down")) return; M._web_btn(id, 0); b.classList.remove("down"); };
  b.addEventListener("pointerdown", down); b.addEventListener("pointerup", up); b.addEventListener("pointercancel", up);
  return b;
}
function buildPanel() {
  const c = $("ctrls");
  for (const [n, id, x, y, sub] of KNOBS) c.appendChild(knob(n, id, x, y, sub));
  c.appendChild(button("OCT−", BTN["OCT−"], 68, 299, "oct"));
  c.appendChild(button("OCT+", BTN["OCT+"], 216, 299, "oct"));
  ROWS.forEach((row, r) => row.forEach((n, i) => c.appendChild(button(n, BTN[n], 712 + i * 76, 198 + r * 76, n === "REC" ? "rec" : ""))));
}
function fit() {                                     // as big as the window allows: its width (beside the guide) and height
  const st = $("stage"), d = $("device"), g = document.querySelector("aside"), narrow = innerWidth <= 760;
  const top = st.getBoundingClientRect().top + scrollY, room = innerHeight - top - 70;
  const zg = narrow ? 1 : Math.max(1, Math.min(1.5, innerHeight / 760, innerWidth / 1200));   // the guide's text grows with the window
  if (g) g.style.setProperty("--z", zg);
  const w = narrow ? innerWidth - 32 : innerWidth - 32 - 22 - 250 * zg - 32;
  const s = Math.max(0.2, Math.min(w / 1200, narrow ? 9 : room / 700, 1.6));
  d.style.transform = `scale(${s})`;
  d.classList.toggle("shrunk", s < 0.999);
  st.style.width = 1200 * s + "px";
  st.style.height = 700 * s + "px";
  d.style.marginLeft = "0px";
}
addEventListener("resize", fit);

function buildKeys() {                               // 16 white keys below, the 11 black ones above, as on the FM-1
  const kb = $("kb"), whites = [];
  const black = s => [1, 3, 6, 8, 10].includes((5 + s) % 12);   // F3 = key 0 = pitch class 5
  for (let k = 0; k < NKEYS; k++) if (!black(k)) whites.push(k);
  const step = (1140 - 40) / whites.length;
  for (let k = 0; k < NKEYS; k++) {
    const d = document.createElement("div"); d.dataset.k = k; d.className = "key";
    if (black(k)) { const w = whites.indexOf(k - 1); d.style.left = `${20 + (w + 1) * step - 27}px`; d.style.top = "26px"; }
    else { d.style.left = `${20 + whites.indexOf(k) * step + (step - 54) / 2}px`; d.style.top = "160px"; }
    d.addEventListener("pointerdown", e => { e.preventDefault(); d.setPointerCapture(e.pointerId); press(k, true); });
    d.addEventListener("pointerup", () => press(k, false));
    d.addEventListener("pointercancel", () => press(k, false));
    kb.appendChild(d);
  }
}
function press(k, down) {
  if (!M) return;
  startAudio();
  keysDown = down ? keysDown | (1 << k) : keysDown & ~(1 << k);
  M._web_keys(keysDown >>> 0);
  const el = document.querySelector(`#kb > .key[data-k="${k}"]`); if (el) el.classList.toggle("down", down);
}

// ---- the computer keyboard
const SPECIAL = { z: ["btn", BTN["OCT−"]], x: ["btn", BTN["OCT+"]], " ": ["btn", BTN.REC] };
addEventListener("keydown", e => {
  if (e.repeat || e.metaKey || e.ctrlKey || !M) return;
  const key = e.key.toLowerCase(), i = KEYMAP.indexOf(key);
  if (i >= 0) { press(i, true); e.preventDefault(); return; }
  if (SPECIAL[key]) { startAudio(); M._web_btn(SPECIAL[key][1], 1); e.preventDefault(); return; }
  const turn = { arrowright: [0, 1], arrowleft: [0, -1], arrowup: [1, 1], arrowdown: [1, -1] }[key];   // pages, landscapes
  if (turn) { M._web_enc(turn[0], turn[1]); e.preventDefault(); }
});
addEventListener("keyup", e => {
  if (!M) return;
  const key = e.key.toLowerCase(), i = KEYMAP.indexOf(key);
  if (i >= 0) press(i, false);
  if (SPECIAL[key]) M._web_btn(SPECIAL[key][1], 0);
});

// ---- Web MIDI in
async function setupMidi() {
  if (!navigator.requestMIDIAccess) { $("midi").disabled = true; return; }
  try {
    const acc = await navigator.requestMIDIAccess();
    const sel = $("midi"), fill = () => {
      const cur = sel.value; sel.length = 1;
      // (the list is the user's pick, never automatic: a "Textura" port first, and no "Frodhes" one beside it, as
      // tools/tx_ports.py: a Frodhes FM-1 may be on the same computer)
      const ins = [...acc.inputs.values()], tex = n => /textura/i.test(n || "");
      const has = ins.some(i => tex(i.name));
      ins.sort((a, b) => tex(b.name) - tex(a.name));
      for (const inp of ins) {
        if (has && /frodhes/i.test(inp.name || "")) continue;
        const o = document.createElement("option"); o.value = inp.id; o.textContent = inp.name; sel.appendChild(o);
      }
      sel.value = cur;
    };
    fill(); acc.onstatechange = fill;
    sel.onchange = () => {
      for (const inp of acc.inputs.values()) inp.onmidimessage = null;
      const inp = acc.inputs.get(sel.value);
      if (inp) inp.onmidimessage = m => {         // notes and CCs; the clock and its transport (F8 FA FB FC) too
        const [st, d1 = 0, d2 = 0] = m.data;
        if (M && st < 0xF8) { startAudio(); M._web_midi(st, d1, d2); } else if (M && st <= 0xFC && st !== 0xF9) M._web_midi(st, d1, d2);
      };
    };
  } catch (e) { $("midi").disabled = true; }
}

buildPanel();
buildKeys();
fit();
if (innerWidth <= 760) $("guide").open = false;            // phones: the guide folded, one tap opens it
setupMidi();

// ---- ?selftest: the landscape started (REC), its level and the screen's state in the title (for headless checks)
if (location.search.includes("selftest")) {
  const t0 = performance.now();
  const wait = () => {
    if (!M || performance.now() - t0 < 1500) return setTimeout(wait, 100);
    M._web_btn(BTN.REC, 1); M._web_btn(BTN.REC, 0);    // all lanes start: the landscape's first sounds, low
    setTimeout(() => {
      const p = M._web_audio(16) >> 2, f = M.HEAPF32;
      let s = 0; for (let i = 0; i < 2 * 256 * 16; i++) s += f[p + i] * f[p + i];
      const rms = 10 * Math.log10(s / (2 * 256 * 16) + 1e-20);
      const fb = new Uint16Array(M.HEAPU8.buffer, M._web_fb(), 240 * 240);
      let lit = 0; for (const v of fb) if (v) lit++;
      document.title = `SELFTEST rms=${rms.toFixed(1)}dB screen=${lit}px`;
    }, 2500);
  };
  wait();
}
