"use strict";
// AI Motion UI: pick image -> place joints -> describe motion -> generate -> preview -> send to VEGAS.

const PARENT = {
  pelvis: null, chest: "pelvis", neck: "chest", head: "neck",
  l_shoulder: "neck", l_elbow: "l_shoulder", l_wrist: "l_elbow",
  r_shoulder: "neck", r_elbow: "r_shoulder", r_wrist: "r_elbow",
  l_hip: "pelvis", l_knee: "l_hip", l_ankle: "l_knee", l_toe: "l_ankle",
  r_hip: "pelvis", r_knee: "r_hip", r_ankle: "r_knee", r_toe: "r_ankle",
};
const LABEL = {
  pelvis: "Kalça merkezi", chest: "Göğüs", neck: "Boyun", head: "Baş",
  l_shoulder: "Sol omuz", l_elbow: "Sol dirsek", l_wrist: "Sol bilek",
  r_shoulder: "Sağ omuz", r_elbow: "Sağ dirsek", r_wrist: "Sağ bilek",
  l_hip: "Sol kalça", l_knee: "Sol diz", l_ankle: "Sol ayak bileği", l_toe: "Sol ayak ucu",
  r_hip: "Sağ kalça", r_knee: "Sağ diz", r_ankle: "Sağ ayak bileği", r_toe: "Sağ ayak ucu",
};
// Template poses inside the character's bounding box (x right, y down, 0..1).
// Front view: the character's LEFT side is on the image's RIGHT.
const TEMPLATE_FRONT = {
  head: [.5, .08], neck: [.5, .2], chest: [.5, .3], pelvis: [.5, .52],
  l_shoulder: [.66, .21], l_elbow: [.70, .36], l_wrist: [.72, .5],
  r_shoulder: [.34, .21], r_elbow: [.30, .36], r_wrist: [.28, .5],
  l_hip: [.58, .52], l_knee: [.59, .73], l_ankle: [.6, .94], l_toe: [.62, .99],
  r_hip: [.42, .52], r_knee: [.41, .73], r_ankle: [.4, .94], r_toe: [.38, .99],
};
function templateSide(dir) { // dir = +1 facing right, -1 facing left
  const t = {
    head: [.5 + .06 * dir, .08], neck: [.5, .2], chest: [.5, .3], pelvis: [.5, .52],
    l_shoulder: [.5 - .01 * dir, .21], l_elbow: [.5 - .02 * dir, .36], l_wrist: [.5, .5],
    r_shoulder: [.5 + .01 * dir, .21], r_elbow: [.5 + .02 * dir, .36], r_wrist: [.5 + .02 * dir, .5],
    l_hip: [.5 - .01 * dir, .52], l_knee: [.5, .73], l_ankle: [.49, .94], l_toe: [.49 + .1 * dir, .99],
    r_hip: [.5 + .01 * dir, .52], r_knee: [.51, .73], r_ankle: [.51, .94], r_toe: [.51 + .1 * dir, .99],
  };
  return t;
}

const $ = (id) => document.getElementById(id);
const S = { job: null, img: null, imgW: 0, imgH: 0, bbox: null, joints: {}, scale: 1, ox: 0, oy: 0,
  drag: null, hover: null, state: null, poll: null, maskData: null, imageData: null, layerData: [], layerNames: [] };

async function api(path, body) {
  const opt = body === undefined ? {} : { method: "POST", headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body) };
  const r = await fetch(path, opt);
  const data = await r.json().catch(() => ({ error: `HTTP ${r.status}` }));
  if (!r.ok) throw new Error(data.error || `HTTP ${r.status}`);
  return data;
}
function showError(msg) { const e = $("error"); e.textContent = msg || ""; e.hidden = !msg; }
const gib = (b) => (b / 1024 ** 3).toFixed(1) + " GB";

// ------------------------------------------------------------------ state / chips
async function refreshState() {
  try { S.state = await api("/api/state"); } catch (e) { return; }
  const st = S.state;
  const g = $("chip-gpu");
  if (st.gpu.available) {
    g.textContent = `${st.gpu.name} · ${gib(st.gpu.total_bytes)} · profil ${st.auto_profile}`;
    g.className = "chip ok";
  } else { g.textContent = `GPU yok: ${st.gpu.reason}`; g.className = "chip bad"; }
  const v = $("chip-vegas");
  const vmap = { idle: ["VEGAS boşta", "ok"], playing: ["VEGAS oynatıyor – AI duraklar", "warn"],
    rendering: ["VEGAS render alıyor – AI duraklar", "warn"], not_running: ["VEGAS kapalı", "ok"],
    unknown: ["VEGAS durumu bilinmiyor – AI bekler", "bad"] };
  const [vt, vc] = vmap[st.vegas.state] || [st.vegas.state, ""];
  v.textContent = vt; v.title = st.vegas.detail; v.className = "chip " + vc;
  const m = $("chip-models");
  const hyOk = st.models.hymotion.length === 0, wanOk = st.models.wan.length === 0;
  m.textContent = `HY-Motion ${hyOk ? "✓" : "✗"} · Wan2.2 ${wanOk ? "✓" : "✗"} · önizleme ${st.puppet_render ? "✓" : "iskelet"}`;
  m.className = "chip " + (hyOk ? "ok" : "bad");
  m.title = [...st.models.hymotion, ...st.models.wan].join("\n");
  $("detect").hidden = !st.detector;
  const p = st.vegas.project;
  if (p && p.width && !S.projectTouched) {
    $("pw").value = p.width; $("ph").value = p.height; if (p.fps) $("pfps").value = p.fps;
    $("proj-hint").textContent = `Proje boyutu VEGAS'tan alındı: ${p.width}×${p.height} @ ${p.fps}`;
  }
  renderSysinfo();
  updateGo();
}

function renderSysinfo() {
  const st = S.state; if (!st) return;
  const prof = st.profiles[$("profile").value === "auto" ? st.auto_profile : $("profile").value];
  const rows = [["Kök klasör", st.root]];
  if (prof) {
    rows.push(["Profil", prof.label]);
    for (const [k, f] of Object.entries(prof.expected))
      rows.push([k, `${f.gb} GB (${{ official: "resmi", computed: "hesaplanan", measured: "ölçülen" }[f.basis]}) – ${f.note}`]);
    for (const n of prof.notes) rows.push(["Not", n]);
  }
  const ms = st.measured;
  if (ms && ms.hymotion) {
    for (const dt of ["fp16", "fp32"]) {
      const r = ms.hymotion[dt]; if (!r) continue;
      rows.push([`Ölçülen DiT ${dt}`, r.ok ? JSON.stringify(r.peak_vram_gib) : r.error]);
    }
    if (ms.hymotion.fp16_vs_fp32_cm) rows.push(["FP16 ~ FP32 farkı", `maks ${ms.hymotion.fp16_vs_fp32_cm.max} cm, ort ${ms.hymotion.fp16_vs_fp32_cm.mean} cm`]);
  } else rows.push(["Ölçüm", "Henüz yok: 'python -m aimotion.selftest' çalıştırın"]);
  $("sysinfo").innerHTML = "<table>" + rows.map(([a, b]) =>
    `<tr><td>${esc(a)}</td><td>${esc(String(b)).replace(/\n/g, "<br>")}</td></tr>`).join("") + "</table>" +
    [["HY-Motion (pin modu)", st.models.hymotion], ["Wan2.2-TI2V-5B (kare modu)", st.models.wan]]
      .filter(([, m]) => m.length)
      .map(([n, m]) => `<details><summary>${esc(n)}: ${m.length} eksik parça (install.ps1 indirir)</summary>` +
        `<ul>${m.map((x) => `<li>${esc(x)}</li>`).join("")}</ul></details>`).join("");
}
const esc = (s) => s.replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
$("profile").onchange = renderSysinfo;
for (const id of ["pw", "ph", "pfps"]) $(id).oninput = () => { S.projectTouched = true; };

// ------------------------------------------------------------------ image + joints editor
const cv = $("editor"), ctx = cv.getContext("2d");
function fit() {
  const maxW = 520, maxH = 540;
  S.scale = Math.min(maxW / S.imgW, maxH / S.imgH);
  cv.width = Math.round(S.imgW * S.scale); cv.height = Math.round(S.imgH * S.scale);
}
function draw() {
  ctx.clearRect(0, 0, cv.width, cv.height);
  if (!S.img) { ctx.fillStyle = "#9aa0ab"; ctx.fillText("Bir karakter resmi seçin", 20, 30); return; }
  ctx.drawImage(S.img, 0, 0, cv.width, cv.height);
  const P = (n) => [S.joints[n][0] * S.scale, S.joints[n][1] * S.scale];
  ctx.lineWidth = 3;
  for (const [c, p] of Object.entries(PARENT)) {
    if (!p || !S.joints[c] || !S.joints[p]) continue;
    ctx.strokeStyle = c.startsWith("l_") ? "#4fb3ff" : c.startsWith("r_") ? "#ff8a4f" : "#ffd23f";
    const [x1, y1] = P(p), [x2, y2] = P(c);
    ctx.beginPath(); ctx.moveTo(x1, y1); ctx.lineTo(x2, y2); ctx.stroke();
  }
  for (const n of Object.keys(S.joints)) {
    const [x, y] = P(n);
    ctx.beginPath(); ctx.arc(x, y, n === S.hover ? 7 : 5, 0, 2 * Math.PI);
    ctx.fillStyle = n.startsWith("l_") ? "#4fb3ff" : n.startsWith("r_") ? "#ff8a4f" : "#ffd23f";
    ctx.fill(); ctx.strokeStyle = "#000"; ctx.lineWidth = 1.5; ctx.stroke();
  }
  if (S.hover) {
    const [x, y] = P(S.hover);
    ctx.font = "12px Segoe UI, sans-serif";
    const t = LABEL[S.hover], w = ctx.measureText(t).width + 8;
    ctx.fillStyle = "rgba(0,0,0,.75)"; ctx.fillRect(x + 10, y - 20, w, 18);
    ctx.fillStyle = "#fff"; ctx.fillText(t, x + 14, y - 7);
  }
}
function pick(ev) {
  const r = cv.getBoundingClientRect();
  const x = (ev.clientX - r.left) * (cv.width / r.width), y = (ev.clientY - r.top) * (cv.height / r.height);
  let best = null, bd = 12;
  for (const [n, [jx, jy]] of Object.entries(S.joints)) {
    const d = Math.hypot(jx * S.scale - x, jy * S.scale - y);
    if (d < bd) { bd = d; best = n; }
  }
  return { best, x: x / S.scale, y: y / S.scale };
}
cv.addEventListener("pointerdown", (ev) => { const p = pick(ev); if (p.best) { S.drag = p.best; cv.setPointerCapture(ev.pointerId); } });
cv.addEventListener("pointermove", (ev) => {
  const p = pick(ev);
  if (S.drag) { S.joints[S.drag] = [Math.max(0, Math.min(S.imgW, p.x)), Math.max(0, Math.min(S.imgH, p.y))]; S.hover = S.drag; }
  else S.hover = p.best;
  draw();
});
cv.addEventListener("pointerup", () => { S.drag = null; updateGo(); });

function placeTemplate() {
  const v = $("view").value;
  const t = v === "90" || v === "45" ? templateSide(1) : v === "-90" || v === "-45" ? templateSide(-1) : TEMPLATE_FRONT;
  const [x0, y0, x1, y1] = S.bbox;
  S.joints = {};
  for (const [n, [u, w]] of Object.entries(t)) S.joints[n] = [x0 + u * (x1 - x0), y0 + w * (y1 - y0)];
  draw(); updateGo();
}
$("tpl").onclick = placeTemplate;

const readAsDataURL = (f) => new Promise((ok, bad) => { const r = new FileReader(); r.onload = () => ok(r.result); r.onerror = bad; r.readAsDataURL(f); });
async function upload() {
  if (!S.imageData) return;
  showError("");
  try {
    const res = await api("/api/upload", { image: S.imageData, mask: S.maskData, layers: S.layerData,
      layer_names: S.layerNames });
    S.job = res.job_id; S.imgW = res.width; S.imgH = res.height; S.bbox = res.bbox;
    $("alpha-warn").hidden = res.has_alpha;
    $("layers-hint").textContent = res.layers ? `${res.layers} parça katmanı yüklendi (arkadan öne dosya adına göre).` : $("layers-hint").textContent;
    const img = new Image();
    img.onload = () => { S.img = img; fit(); if (!Object.keys(S.joints).length) placeTemplate(); else draw(); };
    img.src = `/files/${S.job}/character.png?t=${Date.now()}`;
    $("tpl").disabled = false; $("detect").disabled = false;
    S.joints = {};
  } catch (e) { showError(e.message); }
}
$("file").onchange = async (ev) => { const f = ev.target.files[0]; if (!f) return; S.imageData = await readAsDataURL(f); await upload(); };
$("layers").onchange = async (ev) => {
  const files = [...ev.target.files].sort((a, b) => a.name.localeCompare(b.name));
  S.layerData = await Promise.all(files.map(readAsDataURL));
  S.layerNames = files.map((f) => f.name);
  if (S.imageData) await upload();
};
$("mask").onchange = async (ev) => { const f = ev.target.files[0]; if (!f) return; S.maskData = await readAsDataURL(f); await upload(); };
$("rigjson").onchange = async (ev) => {
  const f = ev.target.files[0]; if (!f || !S.job) return;
  try {
    const rig = await api("/api/rig-import", { job_id: S.job, json: await f.text() });
    S.joints = rig.joints; draw(); updateGo();
  } catch (e) { showError(e.message); }
  ev.target.value = "";
};
$("detect").onclick = async () => {
  try { const rig = await api("/api/rig-detect", { job_id: S.job }); S.joints = rig.joints; draw(); updateGo(); }
  catch (e) { showError(e.message); }
};

// ------------------------------------------------------------------ generate
const REQUIRED = ["pelvis", "neck", "head", "l_shoulder", "r_shoulder", "l_hip", "r_hip", "l_knee", "r_knee", "l_ankle", "r_ankle"];
function updateGo() {
  const busy = S.state && S.state.busy;
  const kind = $("kind").value;
  const haveJoints = REQUIRED.every((n) => S.joints[n]);
  $("go").disabled = !S.job || busy || (kind === "pins" && !haveJoints);
  $("go").title = !S.job ? "Önce resim seçin" : busy ? "Başka bir üretim sürüyor" : (kind === "pins" && !haveJoints) ? "Eklemleri yerleştirin" : "";
}
$("kind").onchange = updateGo;

function retargetOptions() {
  const v = $("view").value;
  return { view_yaw_deg: v === "" ? null : Number(v), root_motion: $("root").checked, foot_lock: $("footlock").checked };
}

$("go").onclick = async () => {
  showError("");
  try {
    await api("/api/generate", {
      job_id: S.job, kind: $("kind").value, prompt: $("prompt").value, duration_s: Number($("duration").value),
      candidates: Number($("cands").value), profile: $("profile").value,
      rig: { width: S.imgW, height: S.imgH, joints: S.joints, source: "ui" },
      stage: { width: Number($("pw").value), height: Number($("ph").value), fps: Number($("pfps").value), center_x: Number($("cx").value), char_height: Number($("ch").value) },
      retarget: retargetOptions(),
    });
    $("results").innerHTML = "";
    startPolling();
  } catch (e) { showError(e.message); }
};
$("cancel").onclick = () => S.job && api(`/api/job/${S.job}/cancel`, {});

function startPolling() {
  clearInterval(S.poll);
  $("progress").hidden = false; $("cancel").hidden = false;
  S.poll = setInterval(pollJob, 1000); pollJob();
}
async function pollJob() {
  let st; try { st = await api(`/api/job/${S.job}`); } catch (e) { return; }
  const pr = $("progress"), bar = pr.querySelector(".bar");
  pr.classList.toggle("indet", st.progress == null && ["running", "queued"].includes(st.state));
  pr.classList.toggle("paused", st.state === "paused");
  bar.style.width = st.progress != null ? Math.round(st.progress * 100) + "%" : "0";
  pr.querySelector(".label").textContent = `${st.stage || st.state}${st.progress != null ? " · " + Math.round(st.progress * 100) + "%" : ""}`;
  if (["done", "error", "cancelled"].includes(st.state)) {
    clearInterval(S.poll); $("cancel").hidden = true;
    if (st.state === "error") showError(st.error);
    if (st.state === "done") { pr.hidden = true; renderResults(st); }
    refreshState();
  }
}

function renderResults(st) {
  const box = $("results"); box.innerHTML = "";
  for (const r of st.results || []) {
    const el = document.createElement("div"); el.className = "cand";
    const isTest = (r.label || "").startsWith("TEST");
    const view = r.view ? ` · görünüm: ${r.view.reason} (${Math.round(r.view.yaw_deg)}°)` : "";
    el.innerHTML = `
      <div><span class="badge ${isTest ? "test" : "ai"}">${esc(r.label || "")}</span></div>
      <video controls loop autoplay muted playsinline src="/files/${S.job}/${r.dir}/${r.preview}?t=${Date.now()}"></video>
      <div class="meta">${r.frames} kare @ ${Number(r.fps).toFixed(3)} fps${view}${r.preview_kind === "skeleton" ? " · iskelet önizlemesi (puppet_render yok)" : ""}</div>
      ${(r.view && r.view.warnings || []).map((w) => `<div class="warn">${esc(w)}</div>`).join("")}
      <div class="row">
        ${st.job.kind === "pins" ? `<button class="btn ghost" data-a="retarget">Görünüm/ayak ayarlarını uygula</button>` : ""}
        <label class="check">${st.job.kind === "pins" ? `<input type="checkbox" data-a="bake"> PNG kareleri de yaz` : ""}</label>
        <button class="btn primary" data-a="export">VEGAS'a aktar</button>
      </div>
      <div class="meta" data-a="msg"></div>`;
    const msg = el.querySelector('[data-a="msg"]');
    const rt = el.querySelector('[data-a="retarget"]');
    if (rt) rt.onclick = async () => {
      msg.textContent = "Yeniden hedefleniyor (model çalışmaz)…";
      try {
        const info = await api(`/api/job/${S.job}/retarget`, { seed: r.seed, options: retargetOptions() });
        Object.assign(r, info); renderResults(st);
      } catch (e) { msg.textContent = e.message; }
    };
    el.querySelector('[data-a="export"]').onclick = async () => {
      const bake = el.querySelector('[data-a="bake"]');
      msg.textContent = "Aktarılıyor…";
      try {
        const res = await api(`/api/job/${S.job}/export`, { seed: r.seed, bake: !!(bake && bake.checked) });
        msg.innerHTML = `Hazır: <code>${esc(res.manifest)}</code><br>VEGAS'ta <b>Tools → Extensions → AI Motion: Import Latest Result</b> (eklenti yoksa <b>Tools → Scripting → Run Script… → AIMotion_Import.cs</b>). Sonuç, imleç konumunda yeni bir video parçasına eklenir.`;
      } catch (e) { msg.textContent = e.message; }
    };
    box.appendChild(el);
  }
}

draw();
refreshState();
setInterval(refreshState, 3000);
