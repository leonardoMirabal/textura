// SPDX-License-Identifier: GPL-3.0-only
// Textura player: the engines (WebAssembly, tx_web.c) played from the computer keys, the mouse or a MIDI keyboard,
// four layers at once (stage S3: each its own engine, parameters, level and pan; keys shared or in zones; the CHAMBER
// after the mix), with CPU, level and particle meters and a 10 s stereo recorder. The audio-callback shape (ScriptProcessor, a linear
// resampler when the browser will not open 44.1 kHz) follows Frodhes' web/app.js.
"use strict";

const ENG_RATE = 44100, BLOCK = 256, BLOCK_US = 5805, LAYER = 0.18, GATE = 0.75;   // one layer, all four
const KEY_LO = 53, KEY_HI = 79;                      // the FM-1's keys F3 .. G5
// the computer keys, as Ableton's: the middle letter row plays the white keys, the row above the black ones
const KEYMAP = { KeyA: 0, KeyW: 1, KeyS: 2, KeyE: 3, KeyD: 4, KeyF: 5, KeyT: 6, KeyG: 7, KeyY: 8, KeyH: 9, KeyU: 10, KeyJ: 11,
                 KeyK: 12, KeyO: 13, KeyL: 14, KeyP: 15, Semicolon: 16, Quote: 17 };
const KEYCAP = { KeyA: "A", KeyW: "W", KeyS: "S", KeyE: "E", KeyD: "D", KeyF: "F", KeyT: "T", KeyG: "G", KeyY: "Y", KeyH: "H",
                 KeyU: "U", KeyJ: "J", KeyK: "K", KeyO: "O", KeyL: "L", KeyP: "P", Semicolon: ";", Quote: "'" };
const VELS = [20, 40, 64, 80, 100, 115, 127];          // C / V step through these
const NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];

let M = null, ctx = null, node = null, calib = null, fm1PerUs = 0, engIdx = 0, base = 60;   // A = C4
// the layers: engine index (-1 off), parameter values, pan, key zone; ed = the edited one
const layers = [0, 1, 2, 3].map(k => ({ eng: -1, vals: {}, pan: 64, lo: KEY_LO, hi: KEY_HI, time: 0, pk: -120 }));
let ed = 0, keyMode = "shared";
const down = new Map();                              // note -> { count of sources holding it, layers it went to }
const pressed = new Map();                           // computer key code -> note
// CPU: browsers coarsen performance.now() (100 us unless the page is cross-origin isolated: audition/serve.py), so
// the meter averages every callback of the last 2 s; the max of single callbacks is shown only when isolated
// The FM-1 estimate times one block of the reference scene (txbench's, in calib.json) next to the engine in every
// callback: engine / reference at the same moment x the reference's native time x the calibration factor.
const cpu = { log: [], ref: [], hold: 0, holdT: 0, fine: !!window.crossOriginIsolated };
const lvl = { hold: 0, holdT: 0, gr: 0, grT: 0 };
let rec = null, lastTotal = 0, lastT = 0, evRate = 0;

function $(id) { return document.getElementById(id); }
function status(t) { $("status").textContent = t; }
window.txTest = { ready: false, errors: [] };
window.addEventListener("error", e => { window.txTest.errors.push(String(e.message)); status("Error: " + e.message); });
window.addEventListener("unhandledrejection", e => { window.txTest.errors.push(String(e.reason)); });

var Module = window.Module = {
  onRuntimeInitialized() { M = Module; init(); },
  print: t => console.log(t), printErr: t => console.warn(t),
};
const S = p => M.UTF8ToString(p);

// ---- start-up: engines, calibration, keyboard, MIDI
async function init() {
  const sel = $("engine");                            // the engine stepper: a slider over the engines
  sel.max = M._txw_count() - 1;
  sel.oninput = () => { if (+sel.value !== layers[ed].eng) selectEngine(+sel.value); };
  $("eng-prev").onclick = () => stepEngine(-1);
  $("eng-next").onclick = () => stepEngine(1);
  try { const s = +localStorage.getItem("textura.engine"); if (s >= 0 && s < M._txw_count()) engIdx = s; } catch (e) { }
  let saved = null;
  try { saved = JSON.parse(localStorage.getItem("textura.layers") || "null"); } catch (e) { }
  buildLayers();
  if (saved && saved.layers) {
    keyMode = saved.mode === "zones" ? "zones" : "shared";
    saved.layers.forEach((l, k) => { if (k < 4) Object.assign(layers[k], { pan: l.pan ?? 64, lo: l.lo ?? KEY_LO, hi: l.hi ?? KEY_HI }); });
    for (let k = 3; k >= 1; k--) {
      const e = saved.layers[k] ? saved.layers[k].eng : -1;
      if (e >= 0 && e < M._txw_count()) { editLayer(k); selectEngine(e); }
    }
  }
  $("keymode").value = keyMode;
  $("keymode").onchange = () => { allOff(); keyMode = $("keymode").value; saveLayers(); };
  for (let k = 0; k < 4; k++) M._txw_pan(k, layers[k].pan);
  editLayer(0);
  selectEngine(engIdx);
  buildChamber();
  buildKeyboard();
  octave(0);
  $("velv").textContent = $("vel").value;
  await loadCalib();
  midiInit();
  window.txTest.ready = true;
  window.txTest.sha = i => S(M._txw_max_sha(i));
  window.txTest.render = n => { M._txw_render(n); return M._txw_peak_take(); };
  window.txTest.layers = layers;
  window.txTest.editLayer = editLayer;
  window.txTest.selectEngine = selectEngine;
  window.txTest.layerOff = layerOff;
  window.txTest.stepEngine = stepEngine;
  window.txTest.held = () => [...down.keys()];
  window.txTest.renderCopy = n => { const p = M._txw_render(n) >> 2; return Array.from(M.HEAPF32.subarray(p, p + 2 * n * BLOCK)); };
  window.txTest.setMode = m => { $("keymode").value = m; $("keymode").onchange(); };
  window.txTest.route = n => [0, 1, 2, 3].filter(k => layers[k].eng >= 0 && (keyMode === "shared" || (n >= layers[k].lo && n <= layers[k].hi)));
  window.txTest.zone = (k, lo, hi) => { layers[k].lo = lo; layers[k].hi = hi; showLayers(); };
  window.txTest.chSha = i => S(M._txw_ch_max_sha(i));
  status("Ready. Press Start sound, then play.");
  requestAnimationFrame(meters);
}

async function loadCalib() {
  try {
    const r = await fetch("calib.json", { cache: "no-store" });
    if (!r.ok) throw new Error(r.status);
    calib = await r.json();
  } catch (e) { calib = null; }
  if (!calib || !calib.factor) {
    $("cpu-note").textContent = "No calibration (calib.json from make tx-bench): browser time only.";
    return;
  }
  // the reference render (engine 0's worst scene) in this browser vs natively: browser -> native -> FM-1
  let best = 1e9;
  for (let k = 0; k < 3; k++) {
    const t0 = performance.now(), n = M._txw_ref(), dt = performance.now() - t0;
    if (n) best = Math.min(best, dt * 1000 / n);
  }
  const toNative = calib.ref_native_run_us / best;
  fm1PerUs = toNative * calib.factor;
  window.txTest.calib = { wasmRefUs: best, toNative, fm1PerUs };
  $("cpu-note").textContent =
    `FM-1 estimate: each audio callback also renders one block of a reference scene (${calib.ref_engine} ${calib.ref_scene}, ` +
    `${calib.ref_native_run_us.toFixed(2)} µs/block natively; ${best.toFixed(2)} here at full speed), so the engine is ` +
    `compared with it at the same clock (a computer slows down between callbacks), then scaled by Frodhes' calibration: ` +
    `3620 µs measured on the FM-1 against ${calib.frodhes_host_worst_us.toFixed(1)} µs natively (×${calib.factor.toFixed(1)}). ` +
    `CPU here: all layers together, averaged over 2 s, split between the layers by their own render times (each row); txbench gives ` +
    `the per-block worst. The red mark is the 75 % gate for four layers; a layer over 18 % is flagged in its row.` +
    (window.crossOriginIsolated ? "" : " Timer: coarse (serve with audition/serve.py for 5 µs and a max).");
}

// ---- the engine and its sliders
// the edited layer's engine (fresh, at its defaults). Instant and click-free: the keys this layer holds are released
// on the old engine, which fades out over 23 ms (tx_web.c) while the new one starts, and pressed again on the new one
function selectEngine(i) {
  const k = ed, held = [...down.entries()].filter(([, h]) => h.to.includes(k));
  if (ed === 0) { engIdx = i; try { localStorage.setItem("textura.engine", String(i)); } catch (e) { } }
  M._txw_slot(k);
  for (const [n] of held) M._txw_key_slot(k, n, 0, 0);
  const seed = new Uint32Array(1);                       // a fresh seed each time: no two sessions alike (the checks
  crypto.getRandomValues(seed);                         // use fixed seeds of their own)
  M._txw_select(i, seed[0] || 1);
  layers[k].eng = i; layers[k].vals = {};
  for (const [n, h] of held) M._txw_key_slot(k, n, h.vel, 1);
  buildSliders();
  showLayers();
  showEngine();
  saveLayers();
}
function stepEngine(d) {                             // previous / next engine on the edited layer, wrapping
  const n = M._txw_count(), cur = layers[ed].eng < 0 ? (d > 0 ? -1 : 0) : layers[ed].eng;
  selectEngine(((cur + d) % n + n) % n);
}
function showEngine() {                              // the stepper and the big name for the edited layer
  const i = layers[ed].eng;
  $("eng-layer").textContent = `(layer ${ed + 1})`;
  if (i < 0) { $("eng-num").textContent = "–"; $("eng-name").textContent = "off"; $("eng-title").textContent = "this layer is off: pick an engine"; return; }
  $("engine").value = i;
  $("eng-num").textContent = i + 1;
  $("eng-name").textContent = S(M._txw_name(i));
  $("eng-title").textContent = `${S(M._txw_title(i))} · ${i + 1} of ${M._txw_count()}`;
}
function layerOff(k) {
  for (const [n, h] of down.entries()) if (h.to.includes(k)) { M._txw_key_slot(k, n, 0, 0); h.to = h.to.filter(x => x !== k); }
  M._txw_slot(k); M._txw_select(-1, 0);
  M._txw_slot(ed);
  layers[k].eng = -1; layers[k].vals = {};
  if (k === ed) { buildSliders(); showEngine(); }
  showLayers(); saveLayers();
}
function editLayer(k) {
  ed = k;
  M._txw_slot(k);
  buildSliders();
  showLayers();
  showEngine();
}
function buildSliders() {                            // the edited layer's sliders, at the values it has now
  const box = $("sliders"), L = layers[ed], i = L.eng;
  box.textContent = "";
  if (i < 0) { $("params-title").textContent = `Parameters: layer ${ed + 1} is off (pick an engine above)`; return; }
  $("params-title").textContent = `Parameters: layer ${ed + 1}, ${S(M._txw_name(i))} (state ${M._txw_state_bytes(i)} B)`;
  const v = (id, d) => (id in L.vals ? L.vals[id] : d);
  const set = (id, f) => x => { L.vals[id] = x; f(x); };
  for (let p = 0; p < M._txw_nparams(); p++)
    slider(box, S(M._txw_pname(p)), S(M._txw_phelp(p)), 0, M._txw_pmax(p), v(p, M._txw_pdef(p)), set(p, x => M._txw_param(p, x)), false, p);
  slider(box, "PARTICLES", "shared: the hero-pool limit (density against CPU)", 1, M._txw_particles_max(),
         v(-1, M._txw_particles_def()), set(-1, x => M._txw_param(-1, x)), true, -1);
  slider(box, "LEVEL", "shared: this layer's level, 100 = unity", 0, 127, v(-2, 100), set(-2, x => M._txw_param(-2, x)), true, -2);
}

// ---- the four layer rows: name, engine, pan, key zone, CPU share and level
function noteName(n) { return `${NAMES[n % 12]}${(n / 12 | 0) - 1}`; }
function buildLayers() {
  const box = $("layers");
  for (let k = 0; k < 4; k++) {
    const d = document.createElement("div");
    d.className = "layer"; d.dataset.k = k;
    d.innerHTML = `<span class="nm">Layer ${k + 1}</span><span class="eng"></span>` +
      `<label class="small">pan <input type="range" min="0" max="127" class="pan"></label>` +
      `<span class="small">zone <select class="lo"></select> – <select class="hi"></select></span>` +
      `<span class="st"></span>`;
    const lo = d.querySelector(".lo"), hi = d.querySelector(".hi");
    for (let n = KEY_LO; n <= KEY_HI; n++) { lo.add(new Option(noteName(n), n)); hi.add(new Option(noteName(n), n)); }
    d.onclick = e => { if (e.target.tagName !== "INPUT" && e.target.tagName !== "SELECT" && e.target.tagName !== "BUTTON") editLayer(k); };
    const pan = d.querySelector(".pan");
    pan.oninput = () => { layers[k].pan = +pan.value; M._txw_pan(k, layers[k].pan); saveLayers(); };
    lo.onchange = () => { allOff(); layers[k].lo = +lo.value; saveLayers(); };
    hi.onchange = () => { allOff(); layers[k].hi = +hi.value; saveLayers(); };
    const off = document.createElement("button");
    off.textContent = "off"; off.className = "small"; off.style.padding = "1px 6px"; off.style.marginLeft = "6px";
    off.onclick = () => layerOff(k);
    d.querySelector(".nm").appendChild(off);
    box.appendChild(d);
  }
}
function showLayers() {
  document.querySelectorAll("#layers .layer").forEach(d => {
    const k = +d.dataset.k, L = layers[k];
    d.classList.toggle("ed", k === ed);
    d.querySelector(".eng").textContent = L.eng >= 0 ? `${S(M._txw_name(L.eng))}: ${S(M._txw_title(L.eng))}` : "off";
    d.querySelector(".pan").value = L.pan;
    d.querySelector(".lo").value = L.lo; d.querySelector(".hi").value = L.hi;
  });
}
function saveLayers() {
  try { localStorage.setItem("textura.layers", JSON.stringify({ mode: keyMode, layers: layers.map(l => ({ eng: l.eng, pan: l.pan, lo: l.lo, hi: l.hi })) })); } catch (e) { }
}

// ---- the CHAMBER after the engine: its own sliders and an on / off switch (remembered in this browser)
function buildChamber() {
  const box = $("ch-sliders"), on = $("ch-on");
  for (let p = 0; p < M._txw_ch_n(); p++) {
    slider(box, S(M._txw_ch_pname(p)), S(M._txw_ch_phelp(p)), 0, M._txw_ch_pmax(p), M._txw_ch_pdef(p),
           v => M._txw_ch_param(p, v), false, "ch" + p);
    M._txw_ch_param(p, M._txw_ch_pdef(p));
  }
  try { on.checked = localStorage.getItem("textura.chamber") === "1"; } catch (e) { }
  M._txw_ch_on(on.checked ? 1 : 0);
  on.onchange = () => { M._txw_ch_on(on.checked ? 1 : 0); try { localStorage.setItem("textura.chamber", on.checked ? "1" : "0"); } catch (e) { } };
}

function slider(box, name, help, lo, hi, def, set, shared, id) {
  const d = document.createElement("div");
  d.className = "sl" + (shared ? " shared" : "");
  d.innerHTML = `<label><span></span><span></span></label><input type="range"><small></small>`;
  const [l, v] = d.querySelectorAll("label span"), inp = d.querySelector("input");
  l.textContent = name; d.querySelector("small").textContent = help;
  inp.min = lo; inp.max = hi; inp.value = def; v.textContent = def;
  inp.dataset.id = id;
  inp.oninput = () => { v.textContent = inp.value; set(+inp.value); };
  box.appendChild(d);
}

function setSlider(id, val) {                         // from MIDI CC (the edited layer)
  const inp = document.querySelector(`#sliders input[data-id="${id}"]`);
  if (!inp) return;
  inp.value = Math.max(+inp.min, Math.min(+inp.max, val));
  inp.oninput();
}

// ---- notes (several sources may hold the same note)
function noteOn(n, vel) {                            // to every layer on (shared) or whose zone holds the note
  if (!M || n < 0 || n > 127) return;
  const h = down.get(n) || { c: 0, to: [], vel: 100 };
  h.vel = Math.max(1, Math.min(127, vel));
  if (!h.c) h.to = [0, 1, 2, 3].filter(k => layers[k].eng >= 0 && (keyMode === "shared" || (n >= layers[k].lo && n <= layers[k].hi)));
  h.c++;
  down.set(n, h);
  for (const k of h.to) M._txw_key_slot(k, n, Math.max(1, Math.min(127, vel)), 1);   // a re-press: another burst
  lightKey(n, true);
}
function noteOff(n) {
  const h = down.get(n);
  if (!h) return;
  if (h.c > 1) { h.c--; return; }
  down.delete(n);
  for (const k of h.to) M._txw_key_slot(k, n, 0, 0);
  lightKey(n, false);
}
function allOff() { for (const n of [...down.keys()]) { down.get(n).c = 1; noteOff(n); } pressed.clear(); }
function vel() { return +$("vel").value; }

function codeNote(code) { return code in KEYMAP ? base + KEYMAP[code] : -1; }
function octave(d) {
  base = Math.max(12, Math.min(96, base + 12 * d));
  $("oct").textContent = `A = ${NAMES[base % 12]}${base / 12 - 1}`;
  keyLabels();
}
function velStep(d) {                                 // C / V: the next lower / higher of the velocity steps
  const v = vel(), up = VELS.find(x => x > v), dn = [...VELS].reverse().find(x => x < v);
  const nv = d > 0 ? (up ?? 127) : (dn ?? 1);
  $("vel").value = nv; $("vel").oninput();
}
// at document level, in the capture phase: a focused slider, select or button never swallows a playing key
document.addEventListener("keydown", e => {
  if (e.metaKey || e.ctrlKey || e.altKey) return;
  const c = e.code;
  if (c === "KeyZ" || c === "KeyX") { e.preventDefault(); if (!e.repeat) octave(c === "KeyZ" ? -1 : 1); return; }
  if (c === "KeyC" || c === "KeyV") { e.preventDefault(); if (!e.repeat) velStep(c === "KeyC" ? -1 : 1); return; }
  if (c === "BracketLeft" || c === "BracketRight" || c === "Minus" || c === "Equal" ||
      ((c === "ArrowLeft" || c === "ArrowRight") && e.target.tagName !== "INPUT" && e.target.tagName !== "SELECT")) {
    e.preventDefault();
    if (!e.repeat || c.startsWith("Arrow")) stepEngine(c === "BracketLeft" || c === "Minus" || c === "ArrowLeft" ? -1 : 1);
    return;
  }
  const n = codeNote(c);
  if (n < 0) return;
  e.preventDefault();                                 // also stops a focused select's type-ahead
  if (e.repeat || pressed.has(c)) return;             // held: no retrigger
  if (!ctx) startAudio();
  pressed.set(c, n);
  noteOn(n, vel());
}, true);
document.addEventListener("keyup", e => {
  const n = pressed.get(e.code);
  if (n === undefined) return;
  pressed.delete(e.code);
  noteOff(n);
}, true);
window.addEventListener("blur", () => { for (const [, n] of pressed) noteOff(n); pressed.clear(); });
// after a click on a slider or a select, give the keys back to the page (the arrows then step engines too)
document.addEventListener("change", e => { if (e.target.tagName === "SELECT" || e.target.type === "range") e.target.blur(); }, true);
document.addEventListener("pointerup", e => { if (e.target.type === "range") setTimeout(() => e.target.blur(), 0); }, true);
$("vel").oninput = () => { $("velv").textContent = $("vel").value; };
$("octdn").onclick = () => octave(-1);
$("octup").onclick = () => octave(1);

// ---- the on-screen keys: F3 .. G5 as on the FM-1
function buildKeyboard() {
  const kb = $("kb"), whites = [];
  for (let n = KEY_LO; n <= KEY_HI; n++) if (![1, 3, 6, 8, 10].includes(n % 12)) whites.push(n);
  const w = 100 / whites.length;
  for (let n = KEY_LO; n <= KEY_HI; n++) {
    const black = [1, 3, 6, 8, 10].includes(n % 12), k = document.createElement("div");
    k.className = "key " + (black ? "b" : "w");
    k.dataset.n = n;
    if (black) { const left = whites.indexOf(n - 1); k.style.left = `calc(${(left + 1) * w}% - ${w * 0.3}%)`; k.style.width = `${w * 0.6}%`; }
    else {
      k.style.left = `${whites.indexOf(n) * w}%`; k.style.width = `${w}%`;
      if (n % 12 === 0) k.innerHTML = `<span>C${n / 12 - 1}</span>`;
    }
    k.insertAdjacentHTML("beforeend", `<em class="lt"></em>`);
    let on = false;
    const press = e => { e.preventDefault(); if (!ctx) startAudio(); if (!on) { on = true; noteOn(n, vel()); } };
    const release = () => { if (on) { on = false; noteOff(n); } };
    k.addEventListener("pointerdown", press);
    k.addEventListener("pointerenter", e => { if (e.buttons & 1) press(e); });
    k.addEventListener("pointerup", release);
    k.addEventListener("pointerleave", release);
    kb.appendChild(k);
  }
}
function keyLabels() {                               // the computer letter of each on-screen key, for this octave
  const at = {};
  for (const c in KEYMAP) at[base + KEYMAP[c]] = KEYCAP[c];
  document.querySelectorAll("#kb .key").forEach(k => { k.querySelector(".lt").textContent = at[+k.dataset.n] || ""; });
}
function lightKey(n, on) { const k = document.querySelector(`#kb .key[data-n="${n}"]`); if (k) k.classList.toggle("on", on); }

// ---- Web MIDI: notes with velocity, CC 20-27 -> the first eight parameters, CC 7 -> level
function midiInit() {
  if (!navigator.requestMIDIAccess) { $("midi").textContent = "(no Web MIDI in this browser)"; return; }
  navigator.requestMIDIAccess().then(acc => {
    const hook = () => {
      const names = [];
      for (const inp of acc.inputs.values()) { inp.onmidimessage = midiMsg; names.push(inp.name); }
      $("midi").textContent = names.length ? `(MIDI: ${names.join(", ")})` : "(MIDI: no input connected)";
    };
    hook();
    acc.onstatechange = hook;
  }, () => { $("midi").textContent = "(MIDI not allowed)"; });
}
function midiMsg(ev) {
  const [st, d1, d2] = ev.data, t = st & 0xF0;
  if (t === 0x90 && d2 > 0) { if (!ctx) startAudio(); noteOn(d1, d2); }
  else if (t === 0x80 || (t === 0x90 && d2 === 0)) noteOff(d1);
  else if (t === 0xB0) {
    if (d1 >= 20 && d1 < 28 && d1 - 20 < M._txw_nparams()) setSlider(d1 - 20, d2);
    else if (d1 === 7) setSlider(-2, d2);
    else if (d1 === 120 || d1 === 123) allOff();
  }
}

// ---- sound: 256-frame blocks at 44.1 kHz, as the device's audio interrupt
function render(blocks) {                             // -> Float32Array (a view: copy before the next call)
  const t0 = performance.now(), p = M._txw_render(blocks) >> 2, t1 = performance.now(), us = (t1 - t0) * 1000 / blocks;
  cpu.log.push([t1, (t1 - t0) * 1000, blocks]);
  while (cpu.log.length && cpu.log[0][0] < t1 - 2000) cpu.log.shift();
  if (calib) {                                        // the reference block, at the same clock
    M._txw_ref_step();
    const t2 = performance.now();
    cpu.ref.push([t2, (t2 - t1) * 1000]);
    while (cpu.ref.length && cpu.ref[0][0] < t2 - 8000) cpu.ref.shift();
  }
  if (us > cpu.hold || t1 - cpu.holdT > 2000) { cpu.hold = us; cpu.holdT = t1; }
  const out = M.HEAPF32.subarray(p, p + 2 * blocks * BLOCK);   // stereo, interleaved
  if (rec) recordPush(out);
  return out;
}
let fifo = new Float32Array(32768), fifoN = 0, frac = 0;       // interleaved frames
function startAudio() {
  if (ctx) { if (ctx.state !== "running") ctx.resume(); return; }
  const AC = window.AudioContext || window.webkitAudioContext;
  try { ctx = new AC({ sampleRate: ENG_RATE, latencyHint: "interactive" }); } catch (e) { ctx = new AC(); }
  const ratio = ENG_RATE / ctx.sampleRate;
  node = ctx.createScriptProcessor(1024, 0, 2);
  node.onaudioprocess = ev => {
    const L = ev.outputBuffer.getChannelData(0), R = ev.outputBuffer.getChannelData(1), n = L.length;
    if (!M) { L.fill(0); R.fill(0); return; }
    if (ratio === 1) {
      const f = render(n / BLOCK);
      for (let i = 0; i < n; i++) { L[i] = f[2 * i]; R[i] = f[2 * i + 1]; }
      return;
    }
    const need = Math.ceil(n * ratio + frac) + 2;                  // frames
    while (fifoN < need) { fifo.set(render(4), 2 * fifoN); fifoN += 4 * BLOCK; }
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
  ctx.resume().then(() => {
    status(`Sound on (${ctx.sampleRate} Hz${ratio !== 1 ? ", resampled from 44.1 kHz" : ""}).`);
    $("start").textContent = "Sound on"; $("start").disabled = true; $("rec").disabled = false;
  }, e => status("Sound could not start: " + e));
}
$("start").onclick = startAudio;

// ---- record 10 s of the output (44.1 kHz, 24-bit stereo WAV)
$("rec").onclick = () => {
  if (rec) return;
  rec = { buf: new Float32Array(ENG_RATE * 10 * 2), n: 0 };
  $("rec").classList.add("on"); $("rec").textContent = "Recording…";
};
function recordPush(f) {
  const m = Math.min(f.length, rec.buf.length - rec.n);
  rec.buf.set(f.subarray(0, m), rec.n);
  rec.n += m;
  if (rec.n >= rec.buf.length) { const r = rec; rec = null; saveWav(r.buf); $("rec").classList.remove("on"); $("rec").textContent = "Record 10 s"; }
}
function saveWav(x) {
  const n = x.length, b = new ArrayBuffer(44 + n * 3), v = new DataView(b);
  const w = (o, s) => { for (let i = 0; i < s.length; i++) v.setUint8(o + i, s.charCodeAt(i)); };
  w(0, "RIFF"); v.setUint32(4, 36 + n * 3, true); w(8, "WAVEfmt "); v.setUint32(16, 16, true);
  v.setUint16(20, 1, true); v.setUint16(22, 2, true); v.setUint32(24, ENG_RATE, true); v.setUint32(28, ENG_RATE * 6, true);
  v.setUint16(32, 6, true); v.setUint16(34, 24, true); w(36, "data"); v.setUint32(40, n * 3, true);
  for (let i = 0, o = 44; i < n; i++, o += 3) {
    const s = Math.max(-8388608, Math.min(8388607, Math.round(x[i] * 8388608)));
    v.setUint8(o, s & 255); v.setUint8(o + 1, (s >> 8) & 255); v.setUint8(o + 2, (s >> 16) & 255);
  }
  const a = document.createElement("a"), stamp = new Date().toISOString().replace(/[:.]/g, "-").slice(0, 19);
  a.href = URL.createObjectURL(new Blob([b], { type: "audio/wav" }));
  const names = layers.filter(l => l.eng >= 0).map(l => S(M._txw_name(l.eng))).join("+") || "silence";
  a.download = `textura-${names}-${stamp}.wav`;
  document.body.appendChild(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  status("Saved 10 s (stereo) as " + a.download);
}

// ---- meters, ~10 times a second
let lastMeter = 0;
function meters(t) {
  requestAnimationFrame(meters);
  if (t - lastMeter < 100) return;
  lastMeter = t;
  let su = 0, sb = 0;
  for (const [, u, b] of cpu.log) { su += u; sb += b; }
  const host = sb ? su / sb : 0;
  $("cpu-host").textContent = `${host.toFixed(2)} µs/block` + (cpu.fine ? ` (max ${cpu.hold.toFixed(1)})` : " (2 s average)");
  $("cpu-host-bar").style.width = `${Math.min(100, host / 50 * 100)}%`;
  let refUs = 0;
  for (const [, u] of cpu.ref) refUs += u;
  refUs = cpu.ref.length > 50 ? refUs / cpu.ref.length : 0;
  const per = refUs ? calib.ref_native_run_us * calib.factor / refUs : fm1PerUs;   // FM-1 us per browser us, now
  if (per) {
    const est = host * per, mx = cpu.hold * per, pct = 100 * est / BLOCK_US;
    $("cpu-fm1").textContent = `${est.toFixed(0)} µs, ${pct.toFixed(1)} %` + (cpu.fine ? ` (max ${(100 * mx / BLOCK_US).toFixed(1)} %)` : "");
    $("cpu-fm1-bar").style.width = `${Math.min(100, pct)}%`;
    $("cpu-fm1-box").classList.toggle("hot", est > GATE * BLOCK_US);
  } else $("cpu-fm1").textContent = "no calibration";
  {                                                   // each layer's share of the time, and its level
    const ts = [0, 1, 2, 3].map(k => M._txw_slot_time(k)), tot = ts.reduce((a, b) => a + b, 0);
    document.querySelectorAll("#layers .layer").forEach(d => {
      const k = +d.dataset.k, L = layers[k], pk = M._txw_slot_peak(k) / 10;
      if (pk > L.pk || t - (L.pkT || 0) > 1500) { L.pk = pk; L.pkT = t; }
      if (L.eng < 0) { d.querySelector(".st").textContent = ""; return; }
      const share = tot ? ts[k] / tot : 0, est = per ? host * per * share : 0;
      L.time = est;
      d.querySelector(".st").textContent = (per ? `${(100 * est / BLOCK_US).toFixed(1)} %` + (est > LAYER * BLOCK_US ? " (over 18 %)" : "") : "") +
        `, ${L.pk > -120 ? L.pk.toFixed(0) : "-inf"} dBFS`;
    });
  }
  window.txTest.live = { host, refUs, per };
  const pk = M._txw_peak_take(), db = pk ? 20 * Math.log10(pk / 8388608) : -120;
  if (db > lvl.hold || t - lvl.holdT > 1500) { lvl.hold = db; lvl.holdT = t; }
  const gr = M._txw_lim_take();                        // the output limiter (after the mix and the CHAMBER)
  if (gr > lvl.gr || t - lvl.grT > 1500) { lvl.gr = gr; lvl.grT = t; }
  $("lvl").textContent = `${db > -120 ? db.toFixed(1) : "-inf"} dBFS (hold ${lvl.hold > -120 ? lvl.hold.toFixed(1) : "-inf"})` +
    (lvl.gr ? `, limiter -${(lvl.gr / 10).toFixed(1)} dB` : "");
  $("lvl-bar").style.width = `${Math.max(0, Math.min(100, (db + 60) / 60 * 100))}%`;
  $("lvl-box").classList.toggle("hot", lvl.hold >= 0);
  const st = new Int32Array(M.HEAP32.buffer, M._txw_stats(), 8);
  if (lastT) evRate = 0.7 * evRate + 0.3 * ((st[5] - lastTotal) * 1000 / Math.max(1, t - lastT));
  lastTotal = st[5]; lastT = t;
  $("parts").textContent = `${st[0]} of ${st[1]}, ${evRate.toFixed(0)} events/s, ${st[4]} stolen${st[6] ? ", quiet" : ""}`;
  $("parts-bar").style.width = `${st[1] ? 100 * st[0] / st[1] : 0}%`;
}
