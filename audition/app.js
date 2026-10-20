// SPDX-License-Identifier: GPL-3.0-only
// Textura player: the engines (WebAssembly, tx_web.c) played from the computer keys, the mouse or a MIDI keyboard,
// with CPU, level and particle meters and a 10 s recorder. The audio-callback shape (ScriptProcessor, a linear
// resampler when the browser will not open 44.1 kHz) follows Frodhes' web/app.js.
"use strict";

const ENG_RATE = 44100, BLOCK = 256, BLOCK_US = 5805, LAYER = 0.18;
const KEY_LO = 53, KEY_HI = 79;                      // the FM-1's keys F3 .. G5
const LOWER = ["KeyZ", "KeyS", "KeyX", "KeyD", "KeyC", "KeyV", "KeyG", "KeyB", "KeyH", "KeyN", "KeyJ", "KeyM",
               "Comma", "KeyL", "Period", "Semicolon", "Slash"];
const UPPER = ["KeyQ", "Digit2", "KeyW", "Digit3", "KeyE", "KeyR", "Digit5", "KeyT", "Digit6", "KeyY", "Digit7",
               "KeyU", "KeyI", "Digit9", "KeyO", "Digit0", "KeyP"];
const NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];

let M = null, ctx = null, node = null, calib = null, fm1PerUs = 0, engIdx = 0, base = 48;
const down = new Map();                              // note -> count of sources holding it
const pressed = new Map();                           // computer key code -> note
// CPU: browsers coarsen performance.now() (100 us unless the page is cross-origin isolated: audition/serve.py), so
// the meter averages every callback of the last 2 s; the max of single callbacks is shown only when isolated
// The FM-1 estimate times one block of the reference scene (txbench's, in calib.json) next to the engine in every
// callback: engine / reference at the same moment x the reference's native time x the calibration factor.
const cpu = { log: [], ref: [], hold: 0, holdT: 0, fine: !!window.crossOriginIsolated };
const lvl = { hold: 0, holdT: 0 };
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
  const sel = $("engine");
  for (let i = 0; i < M._txw_count(); i++) {
    const o = document.createElement("option");
    o.value = i; o.textContent = `${S(M._txw_name(i))}: ${S(M._txw_title(i))}`;
    sel.appendChild(o);
  }
  try { const s = +localStorage.getItem("textura.engine"); if (s >= 0 && s < M._txw_count()) engIdx = s; } catch (e) { }
  sel.value = engIdx;
  sel.onchange = () => selectEngine(+sel.value);
  selectEngine(engIdx);
  buildChamber();
  buildKeyboard();
  octave(0);
  await loadCalib();
  midiInit();
  window.txTest.ready = true;
  window.txTest.sha = i => S(M._txw_max_sha(i));
  window.txTest.render = n => { M._txw_render(n); return M._txw_peak_take(); };
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
    `CPU here: the engine alone, averaged over 2 s; txbench gives the per-block worst. The red mark is 18 % (one of four layers).` +
    (window.crossOriginIsolated ? "" : " Timer: coarse (serve with audition/serve.py for 5 µs and a max).");
}

// ---- the engine and its sliders
function selectEngine(i) {
  allOff();
  engIdx = i;
  try { localStorage.setItem("textura.engine", String(i)); } catch (e) { }
  M._txw_select(i, 1);
  const box = $("sliders");
  box.textContent = "";
  $("params-title").textContent = `Parameters: ${S(M._txw_name(i))} (state ${M._txw_state_bytes(i)} B)`;
  for (let p = 0; p < M._txw_nparams(); p++)
    slider(box, S(M._txw_pname(p)), S(M._txw_phelp(p)), 0, M._txw_pmax(p), M._txw_pdef(p), v => M._txw_param(p, v), false, p);
  slider(box, "PARTICLES", "shared: the hero-pool limit (density against CPU)", 1, M._txw_particles_max(),
         M._txw_particles_def(), v => M._txw_param(-1, v), true, -1);
  slider(box, "LEVEL", "shared: output level, 100 = unity", 0, 127, 100, v => M._txw_param(-2, v), true, -2);
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

function setSlider(id, val) {                         // from MIDI CC
  const inp = document.querySelector(`#sliders input[data-id="${id}"]`);
  if (!inp) return;
  inp.value = Math.max(+inp.min, Math.min(+inp.max, val));
  inp.oninput();
}

// ---- notes (several sources may hold the same note)
function noteOn(n, vel) {
  if (!M || n < 0 || n > 127) return;
  const c = down.get(n) || 0;
  down.set(n, c + 1);
  M._txw_key(n, Math.max(1, Math.min(127, vel)), 1);  // a re-press while held fires another burst
  lightKey(n, true);
}
function noteOff(n) {
  const c = down.get(n) || 0;
  if (!c) return;
  if (c > 1) { down.set(n, c - 1); return; }
  down.delete(n);
  M._txw_key(n, 0, 0);
  lightKey(n, false);
}
function allOff() { for (const n of [...down.keys()]) { down.set(n, 1); noteOff(n); } pressed.clear(); }
function vel() { return +$("vel").value; }

function codeNote(code) {
  let i = LOWER.indexOf(code);
  if (i >= 0) return base + i;
  i = UPPER.indexOf(code);
  return i >= 0 ? base + 12 + i : -1;
}
function octave(d) {
  base = Math.max(12, Math.min(96, base + 12 * d));
  $("oct").textContent = `Z = ${NAMES[base % 12]}${base / 12 - 1}, Q = ${NAMES[base % 12]}${base / 12}`;
}
window.addEventListener("keydown", e => {
  if (e.metaKey || e.ctrlKey || e.altKey || e.target.tagName === "SELECT") return;
  if (e.code === "BracketLeft") { octave(-1); return; }
  if (e.code === "BracketRight") { octave(1); return; }
  const n = codeNote(e.code);
  if (n < 0 || e.repeat || pressed.has(e.code)) return;
  e.preventDefault();
  if (!ctx) startAudio();
  pressed.set(e.code, n);
  noteOn(n, vel());
});
window.addEventListener("keyup", e => {
  const n = pressed.get(e.code);
  if (n === undefined) return;
  pressed.delete(e.code);
  noteOff(n);
});
window.addEventListener("blur", () => { for (const [c, n] of pressed) noteOff(n); pressed.clear(); });
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
  const out = M.HEAPF32.subarray(p, p + blocks * BLOCK);
  if (rec) recordPush(out);
  return out;
}
let fifo = new Float32Array(16384), fifoN = 0, frac = 0;
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
      L.set(f); R.set(f);
      return;
    }
    const need = Math.ceil(n * ratio + frac) + 2;
    while (fifoN < need) { fifo.set(render(4), fifoN); fifoN += 4 * BLOCK; }
    let pos = frac;
    for (let i = 0; i < n; i++, pos += ratio) {
      const k = pos | 0, t = pos - k;
      L[i] = R[i] = fifo[k] + (fifo[k + 1] - fifo[k]) * t;
    }
    const used = pos | 0;
    frac = pos - used;
    fifo.copyWithin(0, used, fifoN);
    fifoN -= used;
  };
  node.connect(ctx.destination);
  ctx.resume().then(() => {
    status(`Sound on (${ctx.sampleRate} Hz${ratio !== 1 ? ", resampled from 44.1 kHz" : ""}).`);
    $("start").textContent = "Sound on"; $("start").disabled = true; $("rec").disabled = false;
  }, e => status("Sound could not start: " + e));
}
$("start").onclick = startAudio;

// ---- record 10 s of the engine's output (44.1 kHz, 24-bit mono WAV)
$("rec").onclick = () => {
  if (rec) return;
  rec = { buf: new Float32Array(ENG_RATE * 10), n: 0 };
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
  v.setUint16(20, 1, true); v.setUint16(22, 1, true); v.setUint32(24, ENG_RATE, true); v.setUint32(28, ENG_RATE * 3, true);
  v.setUint16(32, 3, true); v.setUint16(34, 24, true); w(36, "data"); v.setUint32(40, n * 3, true);
  for (let i = 0, o = 44; i < n; i++, o += 3) {
    const s = Math.max(-8388608, Math.min(8388607, Math.round(x[i] * 8388608)));
    v.setUint8(o, s & 255); v.setUint8(o + 1, (s >> 8) & 255); v.setUint8(o + 2, (s >> 16) & 255);
  }
  const a = document.createElement("a"), stamp = new Date().toISOString().replace(/[:.]/g, "-").slice(0, 19);
  a.href = URL.createObjectURL(new Blob([b], { type: "audio/wav" }));
  a.download = `textura-${S(M._txw_name(engIdx))}-${stamp}.wav`;
  document.body.appendChild(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  status("Saved 10 s as " + a.download);
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
    $("cpu-fm1-box").classList.toggle("hot", est > LAYER * BLOCK_US);
  } else $("cpu-fm1").textContent = "no calibration";
  window.txTest.live = { host, refUs, per };
  const pk = M._txw_peak_take(), db = pk ? 20 * Math.log10(pk / 8388608) : -120;
  if (db > lvl.hold || t - lvl.holdT > 1500) { lvl.hold = db; lvl.holdT = t; }
  $("lvl").textContent = `${db > -120 ? db.toFixed(1) : "-inf"} dBFS (hold ${lvl.hold > -120 ? lvl.hold.toFixed(1) : "-inf"})`;
  $("lvl-bar").style.width = `${Math.max(0, Math.min(100, (db + 60) / 60 * 100))}%`;
  $("lvl-box").classList.toggle("hot", lvl.hold >= 0);
  const st = new Int32Array(M.HEAP32.buffer, M._txw_stats(), 8);
  if (lastT) evRate = 0.7 * evRate + 0.3 * ((st[5] - lastTotal) * 1000 / Math.max(1, t - lastT));
  lastTotal = st[5]; lastT = t;
  $("parts").textContent = `${st[0]} of ${st[1]}, ${evRate.toFixed(0)} events/s, ${st[4]} stolen${st[6] ? ", quiet" : ""}`;
  $("parts-bar").style.width = `${st[1] ? 100 * st[0] / st[1] : 0}%`;
}
