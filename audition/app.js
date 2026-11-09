// SPDX-License-Identifier: GPL-3.0-only
// Textura player: the engines (WebAssembly, tx_web.c) played from the computer keys, the mouse or a MIDI keyboard,
// four layers at once (stage S3: each its own engine, parameters, level and pan; keys shared or in zones; the EFFECTS:
// each layer's filter; the drive, compressor, grain delay and reverb on the mix), with CPU, level and particle meters and a 10 s stereo recorder. The audio-callback shape (ScriptProcessor, a linear
// resampler when the browser will not open 44.1 kHz) follows Frodhes' web/app.js.
"use strict";

const ENG_RATE = 44100, BLOCK = 256, BLOCK_US = 5805, LAYER = 0.18, GATE = 0.75;   // one layer, all four
const KEY_LO = 53, KEY_HI = 79;                      // the FM-1's keys F3 .. G5
const SMK_LO = 48, SMK_HI = 84;                      // the SMK-37's keybed C3 .. C6
let kbLo = KEY_LO, kbHi = KEY_HI;                    // the on-screen keyboard outside lane mode (Keyboard: FM-1 / SMK-37)
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
  srcInit();
  if (srcEng >= 0) {                                  // the page opens on the landscape: the source on layer 1, lane mode on
    editLayer(0); selectEngine(srcEng);
    srcFindIds(0);
    setLaneMode(true);
  }
  $("velv").textContent = $("vel").value;
  await loadCalib();
  await loadAB();
  midiInit();
  showMidiMap();
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
  window.txTest.fx = (g, k, p, v) => { const id = `fx${g}-${p}`, box = g === 4 ? "fx-cp" : g === 3 ? "fx-gd" : g === 2 ? "fx-rv" : g ? "fx-fl" : "fx-dr";
    if (g === 1 && k !== ed) editLayer(k); const inp = document.querySelector(`#${box} input[data-id="${id}"]`);
    if (inp) { inp.value = v; inp.oninput(); } return { running: M._txw_fx_running(), frozen: M._txw_fx_frozen(),
    grains: M._txw_fx_grains(), cap: M._txw_fx_cap() }; };
  window.txTest.ratings = () => JSON.parse(JSON.stringify(ratings));
  window.txTest.ab = () => ({ loaded: !!ab, buttons: document.querySelectorAll("#ab-buttons button").length,
                              playing: abNow ? abNow.label : null, ours: $("ab-ours").currentSrc, ref: $("ab-ref").currentSrc });
  window.txTest.src = () => ({ eng: srcEng, panel: !$("src-panel").hidden, laneMode, lane, run: [...laneRun], step: [0, 1, 2, 3].map(laneStepOf), cnt: [0, 1, 2, 3].map(laneCntOf),
                               held: [...srcHeld], intens: [0, 1, 2, 3].map(k => layers[k].eng === srcEng && srcIds ? srcGet(k, "ACTIVITY") : -1),
                               lit: [...document.querySelectorAll("#kb .key.lane-bar")].length,
                               quick: quick.map(q => ({ step: q.step, cur: { ...q.cur } })), eng: [0, 1, 2, 3].map(engName), snd: [...laneSnd],
                               sel: [...document.querySelectorAll("#kb .key.lane-sel")].map(k => +k.dataset.n),
                               glow: [...document.querySelectorAll("#kb .key.lane-glow")].map(k => +k.dataset.n),
                               blink: [...document.querySelectorAll("#kb .key.lane-blink")].map(k => [+k.dataset.n, k.style.getPropertyValue("--blink")]),
                               mute: [...laneMute] });
  window.txTest.setLaneMode = setLaneMode;
  window.txTest.landscape = startLandscape;
  window.txTest.sound = laneSound;
  window.txTest.vol = (k, v) => laneVol(k, v);
  window.txTest.family = f => { setFamily(f); return { fam, snd: [...laneSnd], names: FAMILIES[fam][1].map((_, i) => soundName(i)) }; };
  window.txTest.gov = (budget) => { if (budget) M._txw_gov_budget(budget); return [0, 1, 2, 3].map(k => [M._txw_gov_cur(k), M._txw_gov_user(k)]); };
  window.txTest.gen = k => ({ on: gen[k].on, state: genState(k), step: gen[k].step });
  window.txTest.midi = (st, d1, d2) => midiMsg({ data: [st, d1, d2] });
  window.txTest.midiMap = () => ({ map: { ...midiMap }, learn, sustain, sustained: [...sustained] });
  window.txTest.keys = () => [...document.querySelectorAll("#kb .key")].map(k => +k.dataset.n);
  window.txTest.abPlay = k => { const b = document.querySelectorAll("#ab-buttons button")[k]; if (b) b.click(); };
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
  M._txw_gov_factor(Math.max(1, Math.round(fm1PerUs * 1000)));   // the governor reads the layers' cost as FM-1 us
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
  srcHeld[k] = false; quick[k].cur = {};               // a lane: its new engine starts as the old one stops (no held key)
  if (laneMode && laneRun[k]) setTimeout(() => laneStart(k, true), 0);
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
  showAB();
}

// ---- A/B: the edited layer's engine against its reference recordings (make tx-ab: build/audition/ab/, local only;
// the recordings are never committed and never played by an engine, only played back here for listening). Each
// reference is played at the gain that matches ours' loudness (BS.1770, in manifest.json); the spectrograms (10 s,
// 20 Hz-20 kHz log, one dB scale, the reference after its gain) sit side by side.
let ab = null, abSel = 0, abNow = null;
const abBufs = new Map();
async function loadAB() {
  try { const r = await fetch("ab/manifest.json", { cache: "no-store" }); if (!r.ok) throw new Error(r.status); ab = await r.json(); }
  catch (e) { ab = null; }
  showAB();
}
function abStop() {
  if (abNow) { try { abNow.src.stop(); } catch (e) { } abNow = null; }
  document.querySelectorAll("#ab-buttons button.on").forEach(b => b.classList.remove("on"));
}
async function abPlay(url, gainDb, btn, label) {
  if (!ctx) startAudio();
  abStop();
  btn.classList.add("on");
  try {
    let buf = abBufs.get(url);
    if (!buf) {
      const r = await fetch(url, { cache: "no-store" });
      buf = await ctx.decodeAudioData(await r.arrayBuffer());
      abBufs.set(url, buf);
    }
    const src = ctx.createBufferSource(), g = ctx.createGain();
    src.buffer = buf; g.gain.value = Math.pow(10, gainDb / 20);
    src.connect(g).connect(ctx.destination);
    src.onended = () => { if (abNow && abNow.src === src) abStop(); };
    abStop(); btn.classList.add("on");
    abNow = { src, label };
    src.start();
  } catch (e) { btn.classList.remove("on"); status("A/B: could not play " + url + " (" + e + ")"); }
}
// ---- ratings (step 5): a 1-5 rating, a note and the kept reference candidates per engine, kept in this browser and
// sent to the local server (audition/serve.py writes .local/ratings.json); "export" downloads the same JSON
const RKEY = "textura.ratings";
let ratings = {};
try { ratings = JSON.parse(localStorage.getItem(RKEY) || "{}") || {}; } catch (e) { ratings = {}; }
function ratingsJson() {
  const entries = Object.keys(ratings).sort().map(n => Object.assign({ engine: n }, ratings[n]));
  return { exported: new Date().toISOString(), app: "textura-player", entries };
}
let postTimer = 0;
function saveRatings() {
  try { localStorage.setItem(RKEY, JSON.stringify(ratings)); } catch (e) { }
  clearTimeout(postTimer);
  postTimer = setTimeout(() => {                         // (debounced: typing a note does not post every key)
    fetch("ratings", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(ratingsJson()) })
      .then(r => { $("rate-status").textContent = r.ok ? "saved (.local/ratings.json)" : "saved in this browser"; })
      .catch(() => { $("rate-status").textContent = "saved in this browser (export to share)"; });
  }, 400);
  $("rate-status").textContent = "saved in this browser";
}
function rateEntry(name, num) {
  if (!ratings[name]) ratings[name] = { num, rating: 0, note: "", kept: [], updated: "" };
  return ratings[name];
}
function showRating() {
  const i = layers[ed].eng, box = $("rate-stars");
  box.textContent = "";
  if (i < 0) { $("rate-what").textContent = "Rate (this layer is off)"; $("rate-note").value = ""; $("rate-note").disabled = true; return; }
  const name = S(M._txw_name(i)), cur = ratings[name] || { rating: 0, note: "" };
  $("rate-what").textContent = `Rate #${i + 1} ${name}`;
  $("rate-note").disabled = false;
  $("rate-note").value = cur.note || "";
  for (let v = 1; v <= 5; v++) {
    const b = document.createElement("button");
    b.textContent = v; b.title = ["", "1: bad", "2: poor", "3: fair", "4: good", "5: real"][v];
    b.classList.toggle("on", cur.rating === v);
    b.onclick = () => {
      const r = rateEntry(name, i + 1);
      r.rating = r.rating === v ? 0 : v; r.updated = new Date().toISOString();
      saveRatings(); showRating();
    };
    box.appendChild(b);
  }
}
$("rate-note").oninput = () => {
  const i = layers[ed].eng;
  if (i < 0) return;
  const r = rateEntry(S(M._txw_name(i)), i + 1);
  r.note = $("rate-note").value; r.updated = new Date().toISOString();
  saveRatings();
};
$("rate-export").onclick = () => {
  const a = document.createElement("a"), stamp = new Date().toISOString().replace(/[:.]/g, "-").slice(0, 19);
  a.href = URL.createObjectURL(new Blob([JSON.stringify(ratingsJson(), null, 1)], { type: "application/json" }));
  a.download = `textura-ratings-${stamp}.json`;
  document.body.appendChild(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  $("rate-status").textContent = "exported " + a.download;
};
function showKeep(name, num, e) {                     // the reference candidates kept for this engine (1-2)
  const box = $("ab-keep");
  box.textContent = "";
  if (!e || !e.refs.length) return;
  const lab = document.createElement("span"); lab.className = "small"; lab.textContent = "keep:"; box.appendChild(lab);
  const r = ratings[name], kept = r ? r.kept : [];
  e.refs.forEach((ref, k) => {
    const b = document.createElement("button"), on = kept.some(x => x.name === ref.name);
    b.textContent = (on ? "★ " : "☆ ") + "ref " + (k + 1); b.className = "small" + (on ? " on" : "");
    b.title = (on ? "kept: " : "keep this reference: ") + ref.name;
    b.onclick = () => {
      const en = rateEntry(name, num), at = en.kept.findIndex(x => x.name === ref.name);
      if (at >= 0) en.kept.splice(at, 1); else en.kept.push({ name: ref.name, url: ref.url || "", licence: ref.licence || "", wav: ref.wav });
      en.updated = new Date().toISOString();
      saveRatings(); showKeep(name, num, e);
    };
    box.appendChild(b);
  });
}

function showAB() {
  const box = $("ab-buttons"), i = layers[ed].eng;
  box.textContent = ""; $("ab-info").textContent = ""; $("ab-keep").textContent = "";
  showRating();
  const imgs = [$("ab-ours"), $("ab-ref")];
  const hide = () => { imgs.forEach(im => { im.removeAttribute("src"); im.style.display = "none"; }); $("ab-ours-cap").textContent = $("ab-ref-cap").textContent = ""; };
  if (!ab) { hide(); $("ab-note").textContent = "(no A/B data: run make tx-ab; the references stay in .local, local only)"; return; }
  if (i < 0) { hide(); $("ab-note").textContent = "(this layer is off)"; return; }
  const name = S(M._txw_name(i));
  let e = ab.engines[name];
  if (!e) { hide(); $("ab-note").textContent = "(not in the A/B data: run make tx-ab again)"; return; }
  if (name === "source") {                            // the source's page holds the bird candidates: shown for the birds only
    const pre = srcIds ? srcGet(i === layers[ed].eng ? ed : ed, "PRESET") : 0, pn = SRC_PRESETS[pre] || "";
    const bird = BIRDS.includes(pn);
    if (!bird) e = { ...e, refs: [] };
  }
  if (abSel >= e.refs.length) abSel = 0;
  $("ab-note").textContent = e.refs.length ? `#${e.num} ${name}: ${e.refs.length} reference${e.refs.length > 1 ? "s" : ""}, loudness-matched to ours`
                                           : `#${e.num} ${name}: no reference recordings yet (step 4 brings candidates)`;
  const add = (text, title, fn) => { const b = document.createElement("button"); b.textContent = text; b.title = title; b.onclick = () => fn(b); box.appendChild(b); return b; };
  add("▶ ours", `ours at the defaults: ${e.ours.kind} (${e.ours.lufs} LUFS)`, b => abPlay(e.ours.wav, 0, b, "ours"));
  e.refs.forEach((r, k) => {
    const b = add(`▶ ref ${k + 1}: ${r.name.length > 28 ? r.name.slice(0, 27) + "…" : r.name}`,
      `${r.name}\n${r.url || "(no URL)"} ${r.licence ? "(" + r.licence + ")" : ""}\n${r.lufs} LUFS, played ${r.gain_db >= 0 ? "+" : ""}${r.gain_db} dB` +
      (r.short_db ? ` (${r.short_db} dB short of ours: its peak)` : ""),
      b => { abSel = k; showABRef(e); abPlay(r.wav, r.gain_db, b, "ref " + (k + 1)); });
    if (k === abSel) b.classList.add("sel");
  });
  add("■ stop", "stop the A/B playback", () => abStop());
  showKeep(name, e.num, e);
  imgs.forEach(im => { im.style.display = ""; });
  $("ab-ours").src = e.ours.png;
  $("ab-ours-cap").textContent = `ours: ${e.ours.kind}, ${e.ours.lufs} LUFS, seed 1`;
  showABRef(e);
}
function showABRef(e) {
  document.querySelectorAll("#ab-buttons button").forEach((b, k) => b.classList.toggle("sel", k === abSel + 1));
  const r = e.refs[abSel];
  if (!r) { $("ab-ref").removeAttribute("src"); $("ab-ref").style.display = "none"; $("ab-ref-cap").textContent = "no reference"; return; }
  $("ab-ref").style.display = ""; $("ab-ref").src = r.png;
  $("ab-ref-cap").textContent = `reference ${abSel + 1}: the loudest 10 s, ${r.gain_db >= 0 ? "+" : ""}${r.gain_db} dB to match` + (r.short_db ? ` (${r.short_db} dB short)` : "");
  const info = $("ab-info");
  info.textContent = `Reference ${abSel + 1}: ${r.name}. `;
  if (r.url) { const a = document.createElement("a"); a.href = r.url; a.target = "_blank"; a.rel = "noopener"; a.textContent = r.url; info.appendChild(a); }
  if (r.licence) info.append(` (${r.licence})`);
  info.append(`. Both spectrograms: 10 s, 20 Hz-20 kHz (log), ${ab.scale_db[0]} .. ${ab.scale_db[1]} dBFS per bin, the reference after its gain.`);
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
  if (i < 0) { $("params-title").textContent = `Parameters: layer ${ed + 1} is off (pick an engine above)`; if (srcEng >= 0) showSource(); return; }
  $("params-title").textContent = `Parameters: layer ${ed + 1}, ${S(M._txw_name(i))} (state ${M._txw_state_bytes(i)} B)`;
  const v = (id, d) => (id in L.vals ? L.vals[id] : d);
  const set = (id, f) => x => { L.vals[id] = x; f(x); };
  for (let p = 0; p < M._txw_nparams(); p++)
    slider(box, S(M._txw_pname(p)), S(M._txw_phelp(p)), 0, M._txw_pmax(p), v(p, M._txw_pdef(p)),
           set(p, x => { M._txw_param(p, x); if (i === srcEng) showSource(); }), false, p);
  slider(box, "PARTICLES", "shared: the hero-pool limit (density against CPU)", 1, M._txw_particles_max(),
         v(-1, M._txw_particles_def()), set(-1, x => M._txw_param(-1, x)), true, -1);
  slider(box, "LEVEL", "shared: this layer's level, 100 = unity", 0, 127, v(-2, 100), set(-2, x => M._txw_param(-2, x)), true, -2);
  if (srcEng >= 0) showSource();
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

// ---- EFFECTS (tx_fx.h): on the mix DRIVE (group 0), FILTER (1: a band, after the drive), COMPRESSOR (4), GRAIN DELAY
// (3), REVERB (2; TYPE ROOM is the old CHAMBER). The values are kept in this browser (textura.fx; a layer's own FILTER
// of before is dropped)
const fxv = { rv: {}, gd: {}, cp: {}, dr: {}, fl: {} };
function fxSave() { try { localStorage.setItem("textura.fx", JSON.stringify({ ...fxv, cmp: fxv.cp, cp: undefined })); } catch (e) { } }
function fxBox(box, g, vals, apply) {
  box.textContent = "";
  for (let p = 0; p < M._txw_fx_n(g); p++) {
    const v = p in vals ? vals[p] : M._txw_fx_pdef(g, p);
    slider(box, S(M._txw_fx_pname(g, p)), S(M._txw_fx_phelp(g, p)), 0, M._txw_fx_pmax(g, p), v,
           x => { vals[p] = x; apply(p, x); fxSave(); }, false, `fx${g}-${p}`);
  }
}
function buildChamber() {                             // (the old CHAMBER's place: now the effects)
  let saved = null;
  try { saved = JSON.parse(localStorage.getItem("textura.fx") || "null"); } catch (e) { }
  if (saved && saved.gd) Object.assign(fxv.gd, saved.gd);
  if (saved && saved.cmp) Object.assign(fxv.cp, saved.cmp);   // ("cmp": the COMPRESSOR's six; the old one-knob COMP's "cp" dropped)
  else if (saved && saved.lane && saved.lane[0] && saved.lane[0][4]) Object.assign(fxv.cp, saved.lane[0][4]);   // (one
                                                      // version's per-layer COMPRESSOR: layer 1's onto the mix's)
  if (saved && saved.dr) Object.assign(fxv.dr, saved.dr);
  else if (saved && saved.lane) {                     // (the per-layer DRIVE of before: the layer with the most AMOUNT)
    const d = saved.lane.filter(l => l && l[0] && l[0][0] > 0).sort((a, b) => b[0][0] - a[0][0])[0];
    if (d) Object.assign(fxv.dr, d[0]);
  }
  if (saved && saved.rv && (saved.lane || saved.fl)) Object.assign(fxv.rv, saved.rv);
  if (saved && saved.fl) Object.assign(fxv.fl, saved.fl);
  try { if (!saved && localStorage.getItem("textura.chamber") === "1") fxv.rv = { 0: 1, 1: 127 }; } catch (e) { }   // CHAMBER on: ROOM
  for (const p in fxv.rv) M._txw_fx_param(2, 0, +p, fxv.rv[p]);
  for (const p in fxv.gd) M._txw_fx_param(3, 0, +p, fxv.gd[p]);
  for (const p in fxv.cp) M._txw_fx_param(4, 0, +p, fxv.cp[p]);
  for (const p in fxv.dr) M._txw_fx_param(0, 0, +p, fxv.dr[p]);
  for (const p in fxv.fl) M._txw_fx_param(1, 0, +p, fxv.fl[p]);
  fxBox($("fx-dr"), 0, fxv.dr, (p, x) => M._txw_fx_param(0, 0, p, x));
  fxBox($("fx-fl"), 1, fxv.fl, (p, x) => M._txw_fx_param(1, 0, p, x));
  fxBox($("fx-cp"), 4, fxv.cp, (p, x) => M._txw_fx_param(4, 0, p, x));
  fxBox($("fx-gd"), 3, fxv.gd, (p, x) => M._txw_fx_param(3, 0, p, x));
  fxBox($("fx-rv"), 2, fxv.rv, (p, x) => M._txw_fx_param(2, 0, p, x));
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

// ---- SOURCE (engine "source"): its preset, ACTIVITY and PROXIMITY in a panel of their own; and LANE MODE, the keyboard
// as on the FM-1: F3 .. G5. A landscape is one family of sounds (up to 12, picked in the Sounds row; up to 4 play at
// once, one per lane); the black keys' groups (3, 2, 3, 2) select the lanes (F#5 nothing), the 16 white keys the
// selected lane's two faders, F3 off .. G5 a full cloud. No key changes the landscape.
let SRC_PRESETS = ["rain on water", "stone cloud", "cicadas", "water drops"];   // (read from the PRESET help line)
// a family's sounds: [name, source preset, TYPE] (TYPE reaches the preset's family members), or "#engine" (a classic
// engine, by its own name), or [name, "#engine"] (a classic engine under the sound's name)
const FAMILIES = [
  ["Water", [["rain on water", "rain on water", 0], ["lake lapping", "water", 120], ["water drops", "water", 0],
    ["cave drips", "water", 16], ["eaves dripping", "water", 32], ["brook", "water", 48], ["bubbling spring", "water", 64],
    ["waterfall", "water", 80], ["waves on sand", "water", 96], ["waves on shingle", "water", 112]]],
  ["Stones & sand", [["stones", "stone cloud", 0], ["rockfall", "stone cloud", 32], ["ringing rocks", "stone cloud", 100],
    ["gravel", "gravel", 0], ["marbles", "gravel", 96], ["sand", "sand", 0], ["singing dune", "sand", 96],
    ["minerals", "minerals", 0], ["stone friction", "minerals", 96], ["stone clinks", "#stonecloud"]]],
  ["Ice & glass", [["icicles", "ice", 8], ["ice in a glass", "ice", 24], ["frost tinkling", "ice", 40], ["snow underfoot", "ice", 56],
    ["lake ice", "ice", 72], ["frozen lake booming", "ice", 104], ["thin ice underfoot", "ice", 120], ["glass pane", "glass", 0],
    ["bottles", "glass", 64], ["shards settling", "glass", 110]]],
  ["Fire", [["campfire", "fire", 0], ["bonfire", "fire", 12], ["fireplace", "fire", 20], ["embers", "fire", 28],
    ["kindling", "fire", 36], ["pine fire", "fire", 44], ["leaves, paper", "fire", 52], ["torch", "fire", 60], ["flame", "fire", 96],
    ["#sparks"]]],
  ["Wood", [["spruce", "wood", 9], ["pine", "wood", 27], ["maple", "wood", 46], ["oak", "wood", 64], ["green branch", "wood", 82],
    ["tree trunk", "wood", 100], ["twigs underfoot", "wood", 118], ["rain stick", "#rainstick"],
    ["crumpled paper", "#crumple"]]],
  ["Wind", [["wind", "wind", 0], ["breeze in grass", "wind", 9], ["wind in pines", "wind", 16], ["leafy trees", "wind", 22],
    ["open moor", "wind", 28], ["sea wind", "wind", 35], ["ridge gale", "wind", 41], ["gusty autumn", "wind", 48],
    ["winter storm", "wind", 54], ["desert wind", "wind", 61], ["wind chimes", "#chimes"]]],
  ["Storm", [["rain on water", "rain on water", 0], ["rain on a roof", "rain on water", 50], ["#storm"], ["#thunder"], ["ridge gale", "wind", 41],
    ["winter storm", "wind", 54], ["hail on a roof", "rain on water", 103], ["hail on a car", "rain on water", 110],
    ["hail in leaves", "rain on water", 117], ["sleet on a window", "rain on water", 124], ["thunder sheet", "#plate"],
    ["spring echo", "#spring"]]],
  ["Insects", [["field cricket", "crickets", 10], ["tree cricket", "crickets", 53], ["mole cricket", "crickets", 74],
    ["cicada", "cicadas", 0], ["small cicada", "cicadas", 20], ["cicada chorus", "cicadas", 64], ["katydid", "katydids", 16],
    ["grasshopper", "katydids", 80], ["honeybee", "bees", 10], ["bumblebee", "bees", 53],
    ["cicada up close", "#cicadas"]]],
  ["Songbirds", ["songbird", "blackbird", "song thrush", "nightingale", "robin", "wren", "hermit thrush", "skylark",
    "wood thrush", "reed warbler"].map(n => [n, n, 0])],
  ["Wild birds", ["chirper", "crow", "gull", "curlew", "oystercatcher", "cuckoo", "woodpecker", "wood pigeon", "nightjar",
    "bittern"].map(n => [n, n, 0])],
];
// each sound's own level, dB (a trim on its lane, before the pan; the FM-1's landscapes.h carries it too): its loudness
// played alone on the firmware's host build (BS.1770 integrated), the median over the everyday keys (HOW MANY 1 .. 3 x
// HOW ACTIVE rare .. middle), brought to -28 LUFS, but no further than its loudest 1 % of 50 ms peaks reaching -2 dBFS
// (the crackling and clicking sounds stay under the target and keep their crest); -24 .. +20 dB. tools/snd_level.py
// measures and writes them
const LEVEL_DB = {
  "Water": {"rain on water": 12, "lake lapping": 3, "water drops": 5, "cave drips": -7, "eaves dripping": -4,
    "brook": -1, "bubbling spring": 2, "waterfall": -2, "waves on sand": 0, "waves on shingle": 1},
  "Stones & sand": {"stones": -4, "rockfall": 1, "ringing rocks": -12, "gravel": -6, "marbles": 8, "sand": -2,
    "singing dune": 2, "minerals": -12, "stone friction": -1, "stone clinks": -3},
  "Ice & glass": {"icicles": -14, "ice in a glass": -24, "frost tinkling": -18, "snow underfoot": -17, "lake ice": -10,
    "frozen lake booming": -8, "thin ice underfoot": -6, "glass pane": -9, "bottles": -5, "shards settling": 1},
  "Fire": {"campfire": -22, "bonfire": -16, "fireplace": -18, "embers": -11, "kindling": -20, "pine fire": -18,
    "leaves, paper": -18, "torch": -13, "flame": -11, "sparks": 3},
  "Wood": {"spruce": 3, "pine": 2, "maple": 3, "oak": 3, "green branch": 1, "tree trunk": -4, "twigs underfoot": -10,
    "rain stick": -5, "crumpled paper": -2},
  "Wind": {"wind": -5, "breeze in grass": -8, "wind in pines": -5, "leafy trees": -6, "open moor": -5, "sea wind": -7,
    "ridge gale": -8, "gusty autumn": -3, "winter storm": -7, "desert wind": 3, "wind chimes": -5},
  "Storm": {"rain on water": 12, "rain on a roof": 2, "storm": 0, "thunder": 5, "ridge gale": -8, "winter storm": -7,
    "hail on a roof": 6, "hail on a car": 8, "hail in leaves": 6, "sleet on a window": 10, "thunder sheet": -11,
    "spring echo": -8},
  "Insects": {"field cricket": 7, "tree cricket": 0, "mole cricket": -2, "cicada": -3, "small cicada": -5,
    "cicada chorus": -2, "katydid": 13, "grasshopper": 18, "honeybee": -4, "bumblebee": -4, "cicada up close": -8},
  "Songbirds": {"songbird": 9, "blackbird": 9, "song thrush": 9, "nightingale": 7, "robin": 5, "wren": 9,
    "hermit thrush": 8, "skylark": 6, "wood thrush": 7, "reed warbler": 5},
  "Wild birds": {"chirper": 10, "crow": 3, "gull": 12, "curlew": 15, "oystercatcher": 10, "cuckoo": 10,
    "woodpecker": 12, "wood pigeon": 12, "nightjar": 14, "bittern": 11},
};
function trimQ12(f, i) {                             // the sound's trim, Q12 (4096 = 0 dB): as tools/gen_landscapes.py
  const s = FAMILIES[f][1][i], d = s ? (LEVEL_DB[FAMILIES[f][0]] || {})[s[0].replace("#", "")] || 0 : 0;
  return Math.round(4096 * Math.pow(10, d / 20));
}
let fam = 0;
const laneSnd = [-1, -1, -1, -1];                    // each lane's sound in the family (-1: none)
// the two faders: the 7 white keys F3 .. E4 HOW MANY (off, 1, 2, 4, 10, 19, 32: each key sounds the same more; the
// source's COUNT knob, n = 1 + 63 x^2.5: COUNT_N the count shown), the 9 F4 .. G5
// HOW ACTIVE each one is (rare .. busy: the generator's ACTIVITY, 0 .. 127, kept as a step on the 0 .. 14 scale)
const COUNT_V = [0, 1, 20, 39, 58, 77, 96], COUNT_N = [0, 1, 2, 4, 10, 19, 32], NCNT = 7, NACT = 9;
const laneCnt = [0, 0, 0, 0];
const sndVol = {};                                   // each sound's volume (the lane's LEVEL, 100 = unity), by landscape and sound
function volKey(k) { return `${fam}:${laneSnd[k]}`; }
function laneVol(k, v) {                             // a lane's volume: kept with its sound
  if (v !== undefined && laneSnd[k] >= 0) sndVol[volKey(k)] = v;
  const x = laneSnd[k] >= 0 && volKey(k) in sndVol ? sndVol[volKey(k)] : 100;
  M._txw_slot(k); M._txw_param(-2, x); M._txw_slot(ed); layers[k].vals[-2] = x;
  M._txw_trim(k, laneSnd[k] >= 0 ? trimQ12(fam, laneSnd[k]) : 4096);
  return x;
}
function actStep(a) { return 1 + Math.round(a * 13 / (NACT - 1)); }        // HOW ACTIVE 0 .. 8 -> step 1 .. 14
function actOf(step) { return Math.max(0, Math.round((step - 1) * (NACT - 1) / 13)); }
// step 1 .. 14 -> ACTIVITY 0 .. 127: the key's, in equal steps (0, 16, 32 .. 127; the source and the generator are
// perceptually even in it: lo + (hi - lo) x^2.5, so each key sounds the same more)
function actVal(step) { return step <= 0 ? 0 : Math.floor((actOf(step) * 254 + NACT - 1) / (2 * (NACT - 1))); }
const BLACKS = [], WHITES = [];
const LANE_LO = 53, LANE_HI = 79;                  // lane mode: F3 .. G5 (16 white keys: the faders; 11 black: the lanes)
const BLACK_LANE = [0, 0, 0, 1, 1, 2, 2, 2, 3, 3, -1];   // the n-th black key's lane: its group (3, 2, 3, 2; F#5 none)
for (let n = LANE_LO; n <= LANE_HI; n++) ([1, 3, 6, 8, 10].includes(n % 12) ? BLACKS : WHITES).push(n);
let srcEng = -1, srcIds = null, laneMode = false, lane = 0;
const laneRun = [false, false, false, false], srcHeld = [false, false, false, false];
const laneMute = [false, false, false, false];       // a black key held 600 ms: the lane muted (it plays on, its output gated)
const MUTE_HOLD_MS = 600;                             // (the firmware's NT_HOLD_MS)
let muteHold = 0;
function laneMuteSet(k, on) {                         // the mix's gate: a short ramp, the lane keeps running (as the firmware)
  laneMute[k] = on;
  if (M) M._txw_gate(k, on ? 0 : 1);
}
function srcFindIds(k) {                              // the source's parameter ids by name (read through layer k)
  if (srcIds) return;
  M._txw_slot(k);
  srcIds = {};
  for (let p = 0; p < M._txw_nparams(); p++) srcIds[S(M._txw_pname(p))] = p;
  const help = S(M._txw_phelp(srcIds.PRESET));           // "the source: 0 rain on water | 1 stone cloud | ..."
  const names = help.slice(help.indexOf(":") + 1).split("|").map(t => t.trim().replace(/^\d+\s+/, ""));
  if (names.length > 1) SRC_PRESETS = names;
  setTimeout(presetGrid, 0);
  M._txw_slot(ed);
}
function srcGet(k, name) {
  const id = srcIds[name], L = layers[k];
  if (id in L.vals) return L.vals[id];
  M._txw_slot(k); const d = M._txw_pdef(id); M._txw_slot(ed);
  return d;
}
function srcSet(k, name, v) {                         // a source parameter of layer k, the sliders kept in step
  const id = srcIds[name];
  M._txw_slot(k); M._txw_param(id, v); M._txw_slot(ed);
  layers[k].vals[id] = v;
  if (k === ed) {
    const inp = document.querySelector(`#sliders input[data-id="${id}"]`);
    if (inp) { inp.value = v; inp.previousElementSibling.querySelector("span:last-child").textContent = v; }
  }
}
function srcHold(k, on) {                             // the source running without a held key (Run, a lane)
  if (srcHeld[k] === on) return;
  srcHeld[k] = on;
  const q = layers[k].eng !== srcEng ? QUICK[engName(k)] : null;
  if (q && q.chord) for (const d of q.chord) M._txw_key_slot(k, 60 + d, on ? 1 : 0, on ? 1 : 0);   // (a chord: hung at
  else M._txw_key_slot(k, 60, on ? 100 : 0, on ? 1 : 0);                                           // velocity 1)
}
function srcLayer(k, preset) {                        // put the source on layer k (if it is not there yet)
  if (layers[k].eng !== srcEng) { const e = ed; editLayer(k); selectEngine(srcEng); srcHeld[k] = false; editLayer(e); }
  srcFindIds(k);
  if (preset !== undefined) srcSet(k, "PRESET", preset);
}
function showSource() {
  const on = layers[ed].eng === srcEng;
  $("src-panel").hidden = !on && !laneMode;
  if (!on) { $("src-layer").textContent = laneMode ? `(layer ${ed + 1} is not a source)` : ""; showLanes(); hostBlock(); return; }
  srcFindIds(ed);
  $("src-layer").textContent = `(layer ${ed + 1})`;
  gridMark();
  const it = srcGet(ed, "ACTIVITY"), pr = srcGet(ed, "PROXIMITY");
  $("src-intens").value = it; $("src-intens-v").textContent = it;
  $("src-intens").parentElement.style.display = laneMode ? "none" : "";   // (in lane mode the lanes' bars are the fader)
  $("src-prox").value = pr; $("src-prox-v").textContent = pr;
  $("src-run").classList.toggle("primary", srcHeld[ed]);
  showLanes();
  hostBlock();
}
// THE HOSTED BLOCK (dev view): the engine the edited layer's source hosts now, with its own parameters set directly. A
// moved slider is an override: it holds over the source's own mapping until the sound changes (the source clears it).
let hostKey = "";
function hostBlock() {                                // build (a new engine or layer) or refresh (values, overrides)
  const box = $("host-sliders"), n = window.txTest.ready && srcEng >= 0 && layers[ed].eng === srcEng ? M._txw_host_n() : 0;
  $("host-box").hidden = !n;
  if (!n) { hostKey = ""; box.innerHTML = ""; return; }
  const name = S(M._txw_host_name()), key = `${ed}/${name}`;
  if (key !== hostKey) {
    hostKey = key; box.innerHTML = ""; $("host-name").textContent = `"${name}"`;
    for (let p = 0; p < n; p++)
      slider(box, S(M._txw_host_pname(p)), S(M._txw_host_phelp(p)), 0, M._txw_host_pmax(p), M._txw_host_value(p),
             v => { M._txw_host_param(p, v); hostBlock(); }, false, p);
  }
  box.querySelectorAll("input").forEach(inp => {
    const p = +inp.dataset.id, v = M._txw_host_value(p);
    inp.parentElement.classList.toggle("ov", !!M._txw_host_over(p));
    if (document.activeElement !== inp && +inp.value !== v) { inp.value = v; inp.previousElementSibling.querySelector("span:last-child").textContent = v; }
  });
}
$("host-reset").onclick = () => { for (let p = 0; p < M._txw_host_n(); p++) M._txw_host_param(p, -1); hostBlock(); };
setInterval(hostBlock, 500);
window.txTest.host = () => {
  hostBlock();
  const out = { shown: !$("host-box").hidden, name: S(M._txw_host_name()), params: {} };
  $("host-sliders").querySelectorAll("input").forEach(inp => {
    const p = +inp.dataset.id;
    out.params[S(M._txw_host_pname(p))] = { id: p, slider: +inp.value, value: M._txw_host_value(p), over: !!M._txw_host_over(p) };
  });
  return out;
};
window.txTest.hostSet = (name, v) => {               // through the slider, as a hand would
  hostBlock();
  for (const inp of $("host-sliders").querySelectorAll("input"))
    if (S(M._txw_host_pname(+inp.dataset.id)) === name) { inp.value = v; inp.oninput(); return true; }
  return false;
};

// QUICK LANES: any of the old engines in a lane. The player keeps it sounding on its own (a held note, or presses
// re-triggered at the step's rate for the one-shot engines) and the 15 steps move the engine's own density / rate /
// amount parameters: each [name, value at C4, value at C6], linear in the knob (the knobs are log in their units), gliding
// over ~1 s. "every": seconds between presses at C4 and at C6 (the press rate r4 + (r6 - r4) x^2.5 between them, exponential
// gaps): the one-shot engines. The stone cloud's DENSITY and the sparks' RATE are perceptually even in their rates (14 =
// 0.44 hits/s as 10 was on the old log scale; sparks 4 .. 105 = 0.2 .. 313 /s as 10 .. 120 were). "turn": held, and
// turned over again at those gaps (key up and down at once: the rain stick, whose seeds run out). "press": a press
// (a call) of that many seconds (0.5 .. 1.5 x) instead of 150 ms. "chord": the keys held (semitones over 60, each at
// velocity 1: the wind chimes' tubes hung, the wind strikes them at random) instead of key 60. "lin": the sound's own
// measured curve (tx_core_lin.h tx_lin_tab): the place on that path (0 .. 127) at the keys' 0, 8 .. 120, 127, so each
// step sounds the same more (its loudness and sounding share measured along the path, fitted as tools/gen_lin.py does).
const QUICK = {
  test: { p: [["RATE", 10, 120]] },
  drip: { p: [["RATE", 20, 127]] },
  candle: { p: [["SPUTTER", 40, 127], ["FLOW", 5, 60]] },
  sparks: { p: [["RATE", 4, 105], ["BURST", 20, 80]] },
  gravel: { p: [["AMOUNT", 20, 120], ["RATE", 30, 110]] },
  rainstick: { p: [["SEEDS", 10, 127]], every: [8, 1.5], turn: 1,
    lin: [0, 13, 18, 24, 47, 54, 59, 64, 68, 72, 76, 81, 86, 98, 106, 111, 127] },
  mineral: { p: [["SPEED", 20, 110], ["PRESSURE", 30, 100]] },
  pebble: { p: [["SPEED", 20, 115]] },
  thunder: { p: [["DIST", 110, 25], ["ENERGY", 40, 100]], every: [40, 8] },
  bees: { p: [["SWARM", 0, 127]] },
  sandsong: { p: [["FLOW", 40, 127]] },
  aeolian: { p: [["WIND", 20, 127], ["GUST", 20, 80]] },
  sand: { p: [["ORIFICE", 25, 110]] },
  lakeice: { p: [["RATE", 0, 110]] },
  spring: { p: [], every: [20, 0.25],
    lin: [0, 20, 26, 30, 36, 41, 45, 48, 51, 54, 57, 60, 63, 70, 81, 92, 112] },
  chimes: { p: [["WIND", 10, 127], ["GUST", 30, 90]], chord: [12, 14, 16, 19, 21],
    lin: [0, 2, 4, 6, 9, 11, 13, 15, 18, 21, 25, 31, 38, 46, 56, 73, 104] },
  rainsurf: { p: [["RATE", 10, 127], ["AREA", 30, 100]] },
  rockfall: { p: [["VOLUME", 30, 110]], every: [120, 6] },
  plate: { p: [["CRASH", 40, 127]], every: [20, 0.35],
    lin: [0, 20, 24, 28, 31, 37, 42, 45, 48, 51, 53, 56, 62, 72, 84, 96, 120] },
  rainwater: { p: [["RATE", 10, 127], ["AREA", 60, 120]] },
  stream: { p: [["FLOW", 10, 127], ["ROAR", 0, 90]] },
  surf: { p: [["HEIGHT", 10, 127], ["CHOP", 30, 90]] },
  storm: { p: [["INTENSITY", 0, 127]] },
  crickets: { p: [["COUNT", 1, 32], ["ACTIVITY", 0, 127]] },
  cicadas: { p: [["CHORUS", 0, 127]], every: [15, 0.6], press: 4,
    lin: [0, 29, 34, 37, 41, 43, 45, 47, 50, 52, 55, 57, 61, 64, 72, 80, 88] },
  katydids: { p: [["COUNT", 1, 16], ["ACTIVITY", 0, 127]] },
  crumple: { p: [["RATE", 10, 120]],
    lin: [0, 12, 19, 28, 37, 44, 54, 61, 68, 76, 86, 92, 99, 107, 116, 122, 127] },
  fire: { p: [["MOIST", 20, 120], ["SIZE", 20, 100], ["ROAR", 30, 120]] },
  stonedrum: { p: [], every: [12, 0.12] },
  stonecloud: { p: [["DENSITY", 14, 127]],
    lin: [0, 26, 32, 37, 43, 49, 55, 59, 63, 67, 70, 76, 83, 90, 99, 111, 127] },
  minerals: { p: [], every: [10, 0.15] },
  glass: { p: [["ENERGY", 60, 127]], every: [40, 2] },
  wood: { p: [["LOAD", 110, 15], ["CREAK", 15, 70]], every: [30, 2] },
  ice: { p: [["AMOUNT", 20, 127]], every: [30, 1.5] },
};
const quick = [0, 1, 2, 3].map(() => ({ step: 7, cur: {}, timer: 0 }));
function engName(k) { return layers[k].eng >= 0 ? S(M._txw_name(layers[k].eng)) : ""; }
function pidOf(k, name) {                             // a parameter id of layer k's engine, by name (-1: none)
  M._txw_slot(k);
  let id = -1;
  for (let p = 0; p < M._txw_nparams(); p++) if (S(M._txw_pname(p)) === name) id = p;
  M._txw_slot(ed);
  return id;
}
function quickPos(k) {                               // a classic engine has no population: how many and how active
  const c = laneMode ? Math.max(0, laneCnt[k] - 1) / (NCNT - 2) : 1;   // together move its density, 0 .. 1
  const x = laneMode ? Math.min(1, 0.5 * c + 0.5 * quick[k].step / 14) : quick[k].step / 14, q = QUICK[engName(k)];
  return q && q.lin ? linTab(q.lin, x * 127) / 127 : x;
}
function linTab(t, u) {                              // a measured curve (tx_core_lin.h tx_lin_tab): the place at u = 0, 8 .. 120, 127
  if (u <= 0) return t[0];
  if (u >= 127) return t[16];
  const i = Math.min(15, Math.floor(u / 8)), w = i < 15 ? 8 : 7;
  return t[i] + (t[i + 1] - t[i]) * (u - 8 * i) / w;
}
function quickTargets(k) {                            // the parameter values for layer k's step
  const q = QUICK[engName(k)], t = {};
  const x = quickPos(k);
  if (q) for (const [name, lo, hi] of q.p) t[name] = lo + (hi - lo) * x;
  return t;
}
function quickTick() {                                // ~20 / s: the running quick lanes glide toward their step (~1 s)
  for (let k = 0; k < 4; k++) {
    if (!laneMode || !laneRun[k] || layers[k].eng < 0 || layers[k].eng === srcEng) continue;
    if (gen[k].on) quick[k].step = genState(k)[0] * 14 / 127;   // the generator's wandering activity
    const t = quickTargets(k), c = quick[k].cur;
    for (const name in t) {
      const now = name in c ? c[name] : t[name], nx = Math.abs(t[name] - now) < 0.5 ? t[name] : now + (t[name] - now) * 0.4;
      if (Math.round(nx) !== Math.round(now) || !(name in c)) {
        const id = pidOf(k, name);
        if (id >= 0) { M._txw_slot(k); M._txw_param(id, Math.round(nx)); M._txw_slot(ed); layers[k].vals[id] = Math.round(nx); }
      }
      c[name] = nx;
    }
  }
}
setInterval(quickTick, 50);
function quickPress(k) {                              // a one-shot engine's next press, at the step's rate
  const q = QUICK[engName(k)];
  if (!q || !q.every || !laneRun[k]) return;
  if (q.turn) { M._txw_key_slot(k, 60, 0, 0); M._txw_key_slot(k, 60, 100, 1); }   // (held: turned over again)
  else {                                              // a press: 150 ms, or a call of its own length (0.5 .. 1.5 x)
    M._txw_key_slot(k, 60, 100, 1);
    setTimeout(() => M._txw_key_slot(k, 60, 0, 0), q.press ? q.press * 1000 * (0.5 + Math.random()) : 150);
  }
  quickNext(k);
}
function quickNext(k, first) {                        // the next press after a random (exponential) gap at the step's rate
  const q = QUICK[engName(k)];
  if (!q || !q.every) return;
  const [a, b] = q.every, x = quickPos(k), mean = 1 / (1 / a + (1 / b - 1 / a) * Math.pow(x, 2.5));   // (the press rate
  // perceptually even between C4's and C6's)
  let g = Math.max(150, -Math.log(1 - Math.random()) * mean * 1000);
  if (first && g > 2650) g = 150 + Math.random() * 2500;   // (the first strike within ~2.5 s: the rarest step is never silent)
  quick[k].timer = setTimeout(() => quickPress(k), g);
}
// THE LANE GENERATOR (tx_core_gen, in tx_web.c): when a lane's Notes are on, it sets the source's ACTIVITY along a
// wandering, remembering trajectory around the fader and presses pitched notes; for an old engine the quick mapping
// follows its activity. The fader (the white keys) is its ACTIVITY.
const gen = [0, 1, 2, 3].map(() => ({ on: true, step: 7, vals: {} }));
let genIds = null;
function genFindIds() {
  if (genIds) return;
  genIds = {};
  for (let i = 0; i < M._txw_gen_n(); i++) genIds[S(M._txw_gen_pname(i))] = i;
}
function genSet(k, name, v) { genFindIds(); gen[k].vals[name] = v; M._txw_gen_param(k, genIds[name], v); }
function genGet(k, name) { genFindIds(); return name in gen[k].vals ? gen[k].vals[name] : M._txw_gen_pdef(genIds[name]); }
function genState(k) { const p = M._txw_gen_state(k) >> 2; return Array.from(M.HEAP32.subarray(p, p + 5)); }
function genRun(k) {                                  // the generator follows the lane: on while it runs with Notes on
  const seed = new Uint32Array(1); crypto.getRandomValues(seed);
  genSet(k, "ACTIVITY", actVal(gen[k].step));
  M._txw_gen_mute(k, layers[k].eng !== srcEng ? 1 : 0);   // a classic engine: its activity through QUICK, no notes
  M._txw_gen_on(k, laneMode && laneRun[k] && gen[k].on ? 1 : 0, seed[0] || 1);
}
function laneStart(k, on) {                           // a lane plays on its own: the source or a held engine held, a one-shot re-pressed
  clearTimeout(quick[k].timer); quick[k].timer = 0;
  laneRun[k] = on;
  genRun(k);
  const q = QUICK[engName(k)];
  if (on && layers[k].eng !== srcEng) { quick[k].cur = {}; quickTick(); }   // a classic engine: its settings first, no glide
  if (q && q.every && !q.turn && layers[k].eng !== srcEng) { srcHold(k, false); if (on) quickNext(k, true); }   // (its first
  else { srcHold(k, on); if (on && q && q.turn && layers[k].eng !== srcEng) quickNext(k); }   // strike after a gap; a turned
  // engine is held from the start and turned over again at the gaps)
}
function laneSelect(i) {
  if (i < 0 || i > 3) return;                        // (4 lanes: 4 sounds at once)
  lane = i;
  if (layers[i].eng < 0) srcLayer(i, 0);             // an empty lane gets the source; any engine can be put there
  editLayer(i);
  showLanes();
  genPanel();
  gridMark();
}
function soundLoad(k, i) {                            // the family's sound i into lane k (stopped, at nothing)
  const [nm, pre, type] = FAMILIES[fam][1][i], cls = sndEng(FAMILIES[fam][1][i]);
  if (laneRun[k]) laneStart(k, false);
  gen[k].step = 0; quick[k].step = 0; laneCnt[k] = 0;
  if (cls) {
    let e = -1;
    for (let q = 0; q < M._txw_count(); q++) if (S(M._txw_name(q)) === cls) e = q;
    if (e >= 0) { const e0 = ed; editLayer(k); selectEngine(e); editLayer(e0); }
  } else {
    srcLayer(k);
    const p = SRC_PRESETS.indexOf(pre);
    if (p >= 0) srcSet(k, "PRESET", p);
    if (srcIds.TYPE !== undefined) srcSet(k, "TYPE", type);
  }
  laneSnd[k] = i;
  laneVol(k);
}
function sndEng(s) {                                 // a classic engine's sound ("#engine", or [name, "#engine"]): its engine
  return s[0].startsWith("#") ? s[0].slice(1) : s[1] && s[1].startsWith("#") ? s[1].slice(1) : null;
}
function soundName(i) { const s = FAMILIES[fam][1][i]; return s ? s[0].replace("#", "") : "–"; }
function soundKey(i) {                                // the family's sound i: select its lane (loading it into a free one)
  let k = laneSnd.indexOf(i);
  if (k >= 0 && k === lane && laneRun[k]) { laneCount(k, 0); return; }   // tapped again: off
  if (k < 0) {                                        // a free lane, else the smallest playing one gives way
    k = [0, 1, 2, 3].find(j => !laneRun[j] && j !== lane) ?? [0, 1, 2, 3].find(j => !laneRun[j]);
    if (k === undefined) k = [0, 1, 2, 3].reduce((a, j) => laneCntOf(j) * 16 + laneStepOf(j) < laneCntOf(a) * 16 + laneStepOf(a) ? j : a, 0);
    soundLoad(k, i);
  }
  laneSelect(k);
}
function setFamily(f) {                               // a landscape: one family; every lane stopped, its first sounds loaded
  fam = f;
  for (let k = 0; k < 4; k++) { if (laneRun[k] || srcHeld[k]) laneStart(k, false); laneSnd[k] = -1; laneMuteSet(k, false); }
  for (let k = 0; k < 4 && k < FAMILIES[fam][1].length; k++) soundLoad(k, k);
  famBar();
  laneSelect(0);
  showSource();
}
function famBar() {                                   // the landscape's families, and its sounds as the black keys
  const box = $("fam-box");
  if (!box) return;
  box.hidden = !laneMode;
  box.textContent = "";
  const row = document.createElement("div");
  row.className = "grp";
  row.insertAdjacentHTML("beforeend", `<span class="small">Landscape</span>`);
  FAMILIES.forEach(([nm], f) => {
    const b = document.createElement("button");
    b.textContent = nm; b.classList.toggle("on", f === fam);
    b.onclick = () => { if (!ctx) startAudio(); setFamily(f); };
    row.appendChild(b);
  });
  box.appendChild(row);
  const row2 = document.createElement("div");
  row2.className = "grp snd";
  row2.insertAdjacentHTML("beforeend", `<span class="small">Sounds</span>`);
  FAMILIES[fam][1].forEach((_, i) => {
    const b = document.createElement("button"), k = laneSnd.indexOf(i);
    b.textContent = soundName(i);
    b.dataset.i = i;
    b.classList.toggle("on", k >= 0 && laneRun[k]); b.classList.toggle("sel", k >= 0 && k === lane);
    b.onclick = () => { if (!ctx) startAudio(); soundKey(i); };
    row2.appendChild(b);
  });
  box.appendChild(row2);
}
function laneKey(n) {                                // a black key: its group's lane selected; a white key: F3 .. E4 how
  const b = BLACKS.indexOf(n), w = WHITES.indexOf(n);   // many (F3 off), F4 .. G5 how active
  if (b >= 0) {                                      // (held MUTE_HOLD_MS: the lane muted / unmuted, noteOff cancels)
    const k = BLACK_LANE[b];
    if (k < 0) return;
    laneSelect(k);
    clearTimeout(muteHold);
    muteHold = setTimeout(() => { muteHold = 0; if (layers[k].eng >= 0) { laneMuteSet(k, !laneMute[k]); showLanes(); } }, MUTE_HOLD_MS);
    return;
  }
  if (w < 0) return;
  if (w < NCNT) laneCount(lane, w); else if (w - NCNT < NACT) laneStep(lane, actStep(w - NCNT));
}
function laneCount(k, c) {                            // HOW MANY: 0 off (new events stop, the tails ring), 1 .. 6: 1 .. 32
  laneCnt[k] = c;
  if (layers[k].eng === srcEng && srcIds && srcIds.COUNT !== undefined) srcSet(k, "COUNT", COUNT_V[c]);
  if (c <= 0) { if (laneRun[k]) laneStart(k, false); showSource(); return; }
  if (laneRun[k] && layers[k].eng !== srcEng) quickTick();
  if (!laneRun[k]) laneStep(k, actStep(0));         // (started from off: rare, so the first key is 1 on both)
  else showSource();
}
function laneCntOf(i) { return laneRun[i] ? laneCnt[i] : 0; }
function laneStep(k, w) {                             // HOW ACTIVE, as a step on the 0 .. 14 scale (0: off)
  if (w <= 0) { laneCount(k, 0); return; }
  if (!laneCnt[k]) {                                 // a sound off: the activity keys start it with one
    laneCnt[k] = 1;
    if (layers[k].eng === srcEng && srcIds && srcIds.COUNT !== undefined) srcSet(k, "COUNT", COUNT_V[1]);
  }
  const lane_ = lane; lane = k;
  if (layers[lane].eng < 0) srcLayer(lane, 0);
  gen[lane].step = w;
  if (gen[lane].on) genSet(lane, "ACTIVITY", actVal(w));
  if (layers[lane].eng === srcEng) { srcFindIds(lane); if (!gen[lane].on) srcSet(lane, "ACTIVITY", actVal(w)); }
  else quick[lane].step = w;
  if (!laneRun[lane]) laneStart(lane, true);
  else if (layers[lane].eng !== srcEng) quickTick();
  lane = lane_;
  showSource();
}
function laneStepOf(i) {                              // the fader's setting (with Notes on, the activity wanders around it)
  if (!laneRun[i]) return 0;                         // (stopped: nothing)
  if (gen[i].on) return gen[i].step;
  if (layers[i].eng === srcEng) return srcIds ? 1 + Math.round(srcGet(i, "ACTIVITY") * 13 / 127) : 0;
  return quick[i].step;
}
function showLanes() {                                // the LEDs: the bar on the white keys, the lanes on the black ones
  document.querySelectorAll("#kb .key").forEach(k => k.classList.remove("lane-sel", "lane-glow", "lane-bar", "lane-blink"));
  const box = $("lanes-box");
  box.textContent = "";
  famBar();
  $("src-grid").hidden = laneMode;                   // (lane mode: the landscape's sounds; otherwise every sound)
  if (!laneMode) return;
  for (let i = 0; i < 4; i++) {
    for (const kb of BLACKS.filter((_, b) => BLACK_LANE[b] === i).map(n => document.querySelector(`#kb .key[data-n="${n}"]`))) {
      if (!kb) continue;
      kb.classList.toggle("lane-sel", i === lane); kb.classList.toggle("lane-glow", laneRun[i] && (i !== lane || laneMute[i]));
      kb.classList.toggle("lane-blink", laneRun[i] && !laneMute[i]);   // (muted: a steady glow)   // a playing sound's key blinks: 1.6 s (rare) .. 0.12 s (busy)
      if (laneRun[i]) kb.style.setProperty("--blink", `${(1 / (1 / 1.6 + (1 / 0.12 - 1 / 1.6) * Math.pow(actOf(laneStepOf(i)) / (NACT - 1), 2.5))).toFixed(2)}s`);   // (its rate
      // on the keys' curve)
    }
    const d = document.createElement("div");
    d.className = "lane" + (i === lane ? " sel" : "") + (laneRun[i] ? " run" : "") + (laneMute[i] ? " muted" : "");
    d.dataset.k = i;
    d.innerHTML = `<span class="lab"></span><span class="bars"><span class="small">how many</span>` +
      `<span class="abar cbar" title="how many: click or drag (F3 off, then 1, 2, 4, 10, 19, 32)"></span>` +
      `<span class="small">how active</span><span class="abar" title="how active each one is: click or drag (F4 rare .. G5 busy)"><i class="mk"></i></span>` +
      `<span class="small">volume</span><input type="range" class="vol" min="0" max="127" aria-label="volume"></span>`;
    const [cbar, bar] = d.querySelectorAll(".abar");
    for (let w = 0; w < NCNT; w++) cbar.insertAdjacentHTML("beforeend", `<b data-w="${w}">${w ? 1 << (w - 1) : "off"}</b>`);
    for (let w = 0; w < NACT; w++) bar.insertAdjacentHTML("beforeend", `<b data-w="${w}"></b>`);
    d.querySelector(".lab").textContent = laneLabel(i);
    d.querySelector(".lab").onclick = () => laneSelect(i);
    const fader = (el, n, fn) => {                    // a bar as a fader: the same as its white keys
      const set = ev => {
        const r = el.getBoundingClientRect(), w = Math.max(0, Math.min(n - 1, Math.floor((ev.clientX - r.left) / r.width * n)));
        if (lane !== i) laneSelect(i);
        fn(w);
      };
      el.addEventListener("pointerdown", ev => { ev.preventDefault(); if (!ctx) startAudio(); el.setPointerCapture(ev.pointerId); set(ev); });
      el.addEventListener("pointermove", ev => { if (ev.buttons & 1) set(ev); });
    };
    fader(cbar, NCNT, w => laneCount(i, w));
    const vol = d.querySelector(".vol");
    vol.value = laneSnd[i] >= 0 && volKey(i) in sndVol ? sndVol[volKey(i)] : 100;
    vol.title = `volume ${vol.value} (100 = as tuned)`;
    vol.oninput = () => { laneVol(i, +vol.value); vol.title = `volume ${vol.value} (100 = as tuned)`; };
    fader(bar, NACT, w => laneStep(i, actStep(w)));
    box.appendChild(d);
    laneBar(i);
  }
  if (laneRun[lane]) {                               // the selected lane's two bars on the white keys (F3 off: none lit)
    for (let w = 1; w <= laneCnt[lane]; w++) document.querySelector(`#kb .key[data-n="${WHITES[w]}"]`)?.classList.add("lane-bar");
    for (let w = 0, a = actOf(laneStepOf(lane)); w <= a; w++) document.querySelector(`#kb .key[data-n="${WHITES[NCNT + w]}"]`)?.classList.add("lane-bar");
  }
}
function setLaneMode(on) {
  allOff();
  laneMode = on;
  buildKeyboard();                                   // lane mode shows the FM-1's F3 .. G5, otherwise the chosen keyboard
  keyLabels();
  $("lanes").checked = on;
  if (on) setFamily(fam);
  else for (let i = 0; i < 4; i++) { if (laneRun[i] || srcHeld[i]) laneStart(i, false); M._txw_gen_on(i, 0, 1); laneMuteSet(i, false); }
  showSource();
}
function genPanel() {                                // the selected lane's generator: Notes on / off, its knobs, a live readout
  const box = $("gen-sliders");
  if (!box) return;
  genFindIds();
  box.textContent = "";
  $("gen-on").checked = gen[lane].on;
  for (let i = 0; i < M._txw_gen_n(); i++) {
    const nm = S(M._txw_gen_pname(i));
    if (nm === "ACTIVITY") continue;                 // (the fader)
    slider(box, nm, S(M._txw_gen_phelp(i)), 0, M._txw_gen_pmax(i), genGet(lane, nm), v => genSet(lane, nm, v), false, "gen" + i);
  }
}
function laneBar(i) {                                // a lane's activity bar: the set step lit, the generator's activity as a marker
  const d = document.querySelector(`#lanes-box .lane[data-k="${i}"]`);
  if (!d) return;
  const [cbar, bar] = d.querySelectorAll(".abar"), c = laneCntOf(i), a = laneRun[i] ? actOf(laneStepOf(i)) : -1;
  cbar.querySelectorAll("b").forEach(b => b.classList.toggle("on", +b.dataset.w <= c && c > 0));
  bar.querySelectorAll("b").forEach(b => b.classList.toggle("on", +b.dataset.w <= a));
  const mk = d.querySelector(".mk"), g = gen[i].on && laneRun[i] ? genState(i)[0] : -1;   // the generator's wander
  mk.style.display = g < 0 ? "none" : "";
  if (g >= 0) mk.style.left = `${(g / 127) * 100}%`;
}
function laneLabel(i) {                              // a lane's row: preset, activity step, running, the governor
  const pre = layers[i].eng < 0 ? "–" : laneSnd[i] >= 0 ? soundName(laneSnd[i]) : layers[i].eng === srcEng && srcIds ? SRC_PRESETS[srcGet(i, "PRESET")]
    : engName(i) + (QUICK[engName(i)] ? "" : " (no activity map)");
  const gc = M._txw_gov_cur(i), gu = M._txw_gov_user(i);
  return `${laneMute[i] ? "MUTED · " : ""}${pre} · ${laneRun[i] ? `${COUNT_N[laneCnt[i]]} × activity ${actOf(laneStepOf(i)) + 1}/${NACT}` : "off"}` +
    (laneRun[i] && gc < gu ? ` · CPU: ${gc}/${gu} voices` : "");
}
function genReadout() {
  if (!laneMode || !$("gen-now")) return;
  document.querySelectorAll("#lanes-box .lane").forEach(d => { const k = +d.dataset.k; d.querySelector(".lab").textContent = laneLabel(k); d.classList.toggle("run", laneRun[k]); laneBar(k); });
  const st = genState(lane);
  $("gen-now").textContent = gen[lane].on && laneRun[lane]
    ? `lane ${lane + 1}: activity now ${st[0]} (fader ${actVal(gen[lane].step)}), pitch centre +${(st[1] / 10).toFixed(1)}, ${st[2]} notes${st[3] > 50 ? `, recalling (${(st[3] / 10).toFixed(0)} %)` : ""}`
    : `lane ${lane + 1}: ${laneRun[lane] ? "notes off" : "stopped"}`;
  const cut = [0, 1, 2, 3].filter(k => laneRun[k] && M._txw_gov_cur(k) < M._txw_gov_user(k))
    .map(k => `lane ${k + 1} ${M._txw_gov_cur(k)}/${M._txw_gov_user(k)}`);
  $("gen-now").textContent += cut.length ? ` · CPU governor: heroes ${cut.join(", ")}` : "";
}
setInterval(genReadout, 250);
// the sounds as a grid, grouped by nature: a click sets the selected lane's sound (a SOURCE preset, or a classic engine)
const BIRDS = ["songbird", "chirper", "crow", "gull", "blackbird", "song thrush", "nightingale", "robin", "wren", "hermit thrush", "skylark", "wood thrush", "reed warbler", "curlew", "oystercatcher", "cuckoo", "woodpecker", "wood pigeon", "nightjar", "bittern"];
const GROUPS = [["Water", ["rain on water", "water"]], ["Earth", ["stone cloud", "sand", "gravel", "minerals", "glass", "ice", "wood"]],
                ["Air", ["wind"]], ["Fire", ["fire"]], ["Insects", ["crickets", "cicadas", "katydids", "bees"]],
                ["Birds", BIRDS], ["Classic", ["#thunder", "#surf"]]];
function presetGrid() {
  const box = $("src-grid");
  if (!box || !srcIds) return;
  box.textContent = "";
  for (const [g, names] of GROUPS) {
    const row = document.createElement("div");
    row.className = "grp";
    row.insertAdjacentHTML("beforeend", `<span class="small">${g}</span>`);
    for (const nm of names) {
      const b = document.createElement("button");
      b.textContent = nm.replace("#", ""); b.dataset.nm = nm;
      b.onclick = () => laneSound(lane, nm);
      row.appendChild(b);
    }
    box.appendChild(row);
  }
  gridMark();
}
function gridMark() {                                  // the selected lane's sound lit
  const e = layers[lane].eng, cur = e === srcEng && srcIds ? SRC_PRESETS[srcGet(lane, "PRESET")] : e >= 0 ? "#" + engName(lane) : "";
  document.querySelectorAll("#src-grid button").forEach(b => b.classList.toggle("on", b.dataset.nm === cur));
}
function laneSound(k, nm) {
  if (!ctx) startAudio();
  laneSnd[k] = -1;
  if (nm.startsWith("#")) {                            // a classic engine in the lane (a quick lane)
    let i = -1;
    for (let q = 0; q < M._txw_count(); q++) if (S(M._txw_name(q)) === nm.slice(1)) i = q;
    if (i < 0) return;
    const e = ed; editLayer(k); selectEngine(i); editLayer(e);
  } else {
    srcLayer(k);
    const p = SRC_PRESETS.indexOf(nm);
    if (p >= 0) srcSet(k, "PRESET", p);
  }
  if (laneMode && !laneRun[k]) laneStart(k, true);
  laneSelect(k);
  showSource();
}
function startLandscape() {                         // the landscape's first three sounds at once, low: 2 / 1 / 1 of them, rare
  if (!ctx) startAudio();
  if (!laneMode) setLaneMode(true);
  [[2, 1], [1, 0], [1, 0]].forEach(([c, a], k) => { if (laneSnd[k] >= 0) { laneCount(k, c); laneStep(k, actStep(a)); } });   // (low: a start)
  laneSelect(0);
  showSource();
}
function srcInit() {
  for (let i = 0; i < M._txw_count(); i++) if (S(M._txw_name(i)) === "source") srcEng = i;
  if (srcEng < 0) return;
  $("src-intens").oninput = () => { $("src-intens-v").textContent = $("src-intens").value; srcSet(ed, "ACTIVITY", +$("src-intens").value); };
  $("src-prox").oninput = () => { $("src-prox-v").textContent = $("src-prox").value; srcSet(ed, "PROXIMITY", +$("src-prox").value); };
  $("src-run").onclick = () => { if (!ctx) startAudio(); srcHold(ed, !srcHeld[ed]); if (laneMode) laneRun[ed] = srcHeld[ed]; showSource(); };
  $("lanes").onchange = () => { if (!ctx) startAudio(); setLaneMode($("lanes").checked); };
  $("landscape").onclick = startLandscape;
  $("gen-on").onchange = () => { gen[lane].on = $("gen-on").checked; genRun(lane); showSource(); };
  showSource();
}

// ---- notes (several sources may hold the same note)
function noteOn(n, vel) {                            // to every layer on (shared) or whose zone holds the note
  if (!M || n < 0 || n > 127) return;
  if (laneMode) { if (n >= LANE_LO && n <= LANE_HI) laneKey(n); return; }   // lane mode: the keys are the lanes' faders
  const h = down.get(n) || { c: 0, to: [], vel: 100 };
  h.vel = Math.max(1, Math.min(127, vel));
  if (!h.c) h.to = [0, 1, 2, 3].filter(k => layers[k].eng >= 0 && (keyMode === "shared" || (n >= layers[k].lo && n <= layers[k].hi)));
  h.c++;
  down.set(n, h);
  for (const k of h.to) M._txw_key_slot(k, n, Math.max(1, Math.min(127, vel)), 1);   // a re-press: another burst
  lightKey(n, true);
}
function noteOff(n) {
  if (laneMode && BLACKS.includes(n) && muteHold) { clearTimeout(muteHold); muteHold = 0; }   // (let go: a select only)
  if (laneMode && !down.has(n)) return;
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
  if (e.target.tagName === "INPUT" && e.target.type === "text") return;   // typing a note: the letters are text
  const c = e.code;
  if (c === "KeyZ" || c === "KeyX") { e.preventDefault(); if (!e.repeat) octave(c === "KeyZ" ? -1 : 1); return; }
  if (c === "KeyC" || c === "KeyV") { e.preventDefault(); if (!e.repeat) velStep(c === "KeyC" ? -1 : 1); return; }
  if (c === "BracketLeft" || c === "BracketRight" || c === "Minus" || c === "Equal" ||
      ((c === "ArrowLeft" || c === "ArrowRight") && e.target.tagName !== "INPUT" && e.target.tagName !== "SELECT")) {
    e.preventDefault();
    if (!e.repeat || c.startsWith("Arrow")) stepEngine(c === "BracketLeft" || c === "Minus" || c === "ArrowLeft" ? -1 : 1);
    return;
  }
  if (laneMode && /^Digit[0-9]$/.test(c)) { e.preventDefault(); if (!e.repeat) setFamily((+c.slice(5) + 9) % 10); return; }
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
  const kb = $("kb"), whites = [], lo = laneMode ? LANE_LO : kbLo, hi = laneMode ? LANE_HI : kbHi;
  kb.textContent = "";
  for (let n = lo; n <= hi; n++) if (![1, 3, 6, 8, 10].includes(n % 12)) whites.push(n);
  const w = 100 / whites.length;
  for (let n = lo; n <= hi; n++) {
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

// ---- Web MIDI: notes with velocity, CC 20-27 -> the first eight parameters, CC 7 -> level, CC 64 sustain (the notes
// released under the pedal sound on until it lifts), the mod wheel (CC 1) -> ACTIVITY and the pitch wheel -> PROXIMITY
// when the edited layer's engine has them (the wheel's centre, 64, is the preset's own distance), and MIDI LEARN: any CC
// or the pitch wheel onto any slider of the edited layer, by the parameter's name (so a map survives an engine change
// when the new engine has that parameter); kept in this browser (localStorage textura.midimap).
const MAP_KEY = "textura.midimap", MIDI_DEF = { cc1: "ACTIVITY", pb: "PROXIMITY" };
let midiMap = {}, learn = null, sustain = false;     // learn: null, "armed" (pick a slider), or a parameter name
const sustained = new Set();                          // notes released while the pedal was down
try { midiMap = JSON.parse(localStorage.getItem(MAP_KEY) || "{}") || {}; } catch (e) { midiMap = {}; }
function saveMidiMap() { try { localStorage.setItem(MAP_KEY, JSON.stringify(midiMap)); } catch (e) { } showMidiMap(); }
function showMidiMap() {
  const m = Object.entries(midiMap).map(([k, v]) => `${k === "pb" ? "pitch wheel" : "CC " + k.slice(2)} → ${v}`);
  $("midimap").textContent = learn === "armed" ? "MIDI learn: click a slider" :
    learn ? `MIDI learn: move a control for ${learn}` : (m.length ? "MIDI map: " + m.join(", ") : "");
  $("learn").classList.toggle("primary", !!learn);
}
function sliderName(inp) {                            // a slider's parameter name (PARTICLES / LEVEL for the shared ones)
  const id = +inp.dataset.id;
  return id === -1 ? "PARTICLES" : id === -2 ? "LEVEL" : S(M._txw_pname(id));
}
function paramByName(name) {                          // the edited layer's slider id for a parameter name, or null
  if (name === "PARTICLES") return -1;
  if (name === "LEVEL") return -2;
  if (layers[ed].eng < 0) return null;
  for (let p = 0; p < M._txw_nparams(); p++) if (S(M._txw_pname(p)) === name) return p;
  return null;
}
function midiControl(key, v) {                        // a CC ("cc<N>") or the pitch wheel ("pb"), value 0..127
  if (learn && learn !== "armed") { midiMap[key] = learn; learn = null; saveMidiMap(); }
  const name = midiMap[key] || MIDI_DEF[key];
  if (!name) return false;
  const id = paramByName(name);
  if (id === null) return false;
  const inp = document.querySelector(`#sliders input[data-id="${id}"]`);
  setSlider(id, inp ? Math.round(v * +inp.max / 127) : v);
  return true;
}
function setSustain(on) {
  sustain = on;
  if (!on) { for (const n of [...sustained]) noteOff(n); sustained.clear(); }
}
$("learn").onclick = () => { learn = learn ? null : "armed"; showMidiMap(); };
$("unlearn").onclick = () => { midiMap = {}; learn = null; saveMidiMap(); };
document.addEventListener("pointerdown", e => {     // MIDI learn: the slider clicked is the target
  if (learn !== "armed" || !e.target.matches || !e.target.matches("#sliders input[type=range]")) return;
  learn = sliderName(e.target); showMidiMap();
}, true);
$("kblayout").onchange = () => {
  const smk = $("kblayout").value === "smk";
  allOff();
  kbLo = smk ? SMK_LO : KEY_LO; kbHi = smk ? SMK_HI : KEY_HI;
  try { localStorage.setItem("textura.kblayout", smk ? "smk" : "fm1"); } catch (e) { }
  buildKeyboard(); keyLabels();
};
try { if (localStorage.getItem("textura.kblayout") === "smk") { $("kblayout").value = "smk"; kbLo = SMK_LO; kbHi = SMK_HI; } } catch (e) { }
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
  if (t === 0x90 && d2 > 0) {
    if (!ctx) startAudio();
    if (sustained.delete(d1)) noteOff(d1);            // re-struck under the pedal: a new press
    noteOn(d1, d2);
  }
  else if (t === 0x80 || (t === 0x90 && d2 === 0)) { if (sustain && down.has(d1)) sustained.add(d1); else noteOff(d1); }
  else if (t === 0xE0) midiControl("pb", d2);         // the pitch wheel's upper 7 bits (centre 64)
  else if (t === 0xB0) {
    if (d1 === 64) setSustain(d2 >= 64);
    else if (d1 === 120 || d1 === 123) { setSustain(false); allOff(); }
    else if (midiMap["cc" + d1] || (learn && learn !== "armed")) midiControl("cc" + d1, d2);
    else if (d1 >= 20 && d1 < 28 && d1 - 20 < M._txw_nparams()) setSlider(d1 - 20, d2);
    else if (d1 === 7) setSlider(-2, d2);
    else midiControl("cc" + d1, d2);                  // the defaults (CC 1)
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
  if (M) {                                            // the grain delay: grains sounding, the effects' cap
    const gs = M._txw_fx_grains();
    { const gr = M._txw_comp_gr(); $("fx-cp-state").textContent = gr < 0 ? "· off" : `· gain reduction ${(gr / 10).toFixed(1)} dB`; }
    $("fx-gd-state").textContent = gs < 0 ? "" : `· ${gs} grain${gs === 1 ? "" : "s"} sounding, at most ${M._txw_fx_cap()} now (the effects' CPU)`;
  }
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
