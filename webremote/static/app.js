/* 壹页 · 串口遥控 — SSE live link + serial key injection.
   Keys live on the device mockup (chin bar + side rocker), mirroring hardware. */
"use strict";

const $ = (id) => document.getElementById(id);
const el = {
  led: $("led"), connText: $("connText"), portChip: $("portChip"), seqChip: $("seqChip"),
  screen: $("screen"), screenWrap: $("screenWrap"), screenEmpty: $("screenEmpty"),
  overlay: $("screenOverlay"), overlayTag: $("overlayTag"),
  metaSeq: $("metaSeq"), metaMs: $("metaMs"), metaCrc: $("metaCrc"),
  chargeLed: $("chargeLed"),
  holdMode: $("holdMode"), holdBadge: $("holdBadge"),
  holdMs: $("holdMs"), holdMsOut: $("holdMsOut"),
  settleMs: $("settleMs"), settleMsOut: $("settleMsOut"),
  followMs: $("followMs"), followMsOut: $("followMsOut"),
  autoShot: $("autoShot"), autoScroll: $("autoScroll"), orient: $("orient"),
  btnShot: $("btnShot"), btnReconnect: $("btnReconnect"), btnInfo: $("btnInfo"),
  infoBox: $("infoBox"), infoUrl: $("infoUrl"), infoDevice: $("infoDevice"), infoToken: $("infoToken"),
  log: $("log"), logCount: $("logCount"), logFilter: $("logFilter"),
  btnPause: $("btnPause"), btnClear: $("btnClear"),
  inText: $("inText"), inCount: $("inCount"), btnText: $("btnText"), inState: $("inState"),
  btnMic: $("btnMic"), micLabel: $("micLabel"), meterFill: $("meterFill"),
  micTime: $("micTime"), micHint: $("micHint"),
  sbPort: $("sbPort"), sbKey: $("sbKey"), sbSeq: $("sbSeq"), sbMs: $("sbMs"),
  portSel: $("portSel"), btnPorts: $("btnPorts"), btnConnect: $("btnConnect"),
  btnTourAll: $("btnTourAll"), btnTourStop: $("btnTourStop"),
  tourFill: $("tourFill"), tourList: $("tourList"), tourNote: $("tourNote"),
  toasts: $("toasts"),
};

const state = {
  seq: 0, shownSeq: -1, shownCrc: null, connected: null,
  paused: false, pending: 0, filter: "",
  lines: [], linesTotal: 0, keyChain: Promise.resolve(),
};

const KEYMAP = {
  ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right",
  Enter: "ok", Escape: "back", Backspace: "back", p: "power", P: "power",
};

function toast(msg, kind = "info", ms = 3200) {
  const t = document.createElement("div");
  t.className = `toast ${kind}`;
  t.textContent = msg;
  el.toasts.appendChild(t);
  setTimeout(() => {
    t.classList.add("out");
    setTimeout(() => t.remove(), 350);
  }, ms);
}

/* ---------- log ---------- */
function lineNode(line) {
  const div = document.createElement("div");
  const m = line.match(/^\[(\d+)\]\s*\[(\w+)\]\s*(.*)$/);
  if (m) {
    div.className = `log-line lv-${m[2]}`;
    const t = document.createElement("span");
    t.className = "t";
    t.textContent = `[${m[1]}] `;
    div.appendChild(t);
    div.appendChild(document.createTextNode(`[${m[2]}] ${m[3]}`));
  } else {
    div.className = "log-line";
    div.textContent = line;
  }
  return div;
}
function renderLog() {
  el.log.textContent = "";
  const f = state.filter.toLowerCase();
  const shown = f ? state.lines.filter((l) => l.toLowerCase().includes(f)) : state.lines;
  if (!shown.length) {
    const d = document.createElement("div");
    d.className = "log-empty";
    d.textContent = f ? "无匹配日志" : "等待串口数据…";
    el.log.appendChild(d);
  } else {
    const frag = document.createDocumentFragment();
    for (const l of shown.slice(-1200)) frag.appendChild(lineNode(l));
    el.log.appendChild(frag);
  }
  el.logCount.textContent = String(shown.length);
  if (el.autoScroll.checked && !state.paused) el.log.scrollTop = el.log.scrollHeight;
}
function pushLine(line) {
  state.lines.push(line);
  state.linesTotal++;
  if (state.lines.length > 4000) state.lines.splice(0, state.lines.length - 4000);
  const f = state.filter.toLowerCase();
  if (f && !line.toLowerCase().includes(f)) { el.logCount.textContent = String(state.lines.filter((l) => l.toLowerCase().includes(f)).length); return; }
  const empty = el.log.querySelector(".log-empty");
  if (empty) empty.remove();
  el.log.appendChild(lineNode(line));
  while (el.log.childElementCount > 1200) el.log.firstElementChild.remove();
  el.logCount.textContent = String(f ? state.lines.filter((l) => l.toLowerCase().includes(f)).length : state.lines.length);
  if (el.autoScroll.checked && !state.paused) el.log.scrollTop = el.log.scrollHeight;
}

/* ---------- screen ---------- */
function setCaptureMeta(seq, ms, crcOk) {
  el.metaSeq.textContent = `SEQ ${seq}`;
  el.metaMs.textContent = `${ms} ms`;
  el.metaCrc.textContent = crcOk ? "CRC OK" : "CRC BAD";
  el.metaCrc.style.color = crcOk ? "var(--moss)" : "var(--vermilion)";
  el.seqChip.textContent = `FRAME ${seq}`;
  el.sbSeq.textContent = `seq ${seq}`;
  el.sbMs.textContent = `${ms} ms`;
}
function sweep(tag) {
  el.overlayTag.textContent = tag;
  el.overlay.classList.remove("refresh");
  void el.overlay.offsetWidth;
  el.overlay.classList.add("refresh");
  setTimeout(() => el.overlay.classList.remove("refresh"), 700);
}
function loadScreen(seq, force = false, crc = null) {
  if (!force && seq === state.shownSeq) return;
  if (!force && crc && crc === state.shownCrc) return;
  const orient = el.orient.value;
  const url = `/api/screen.png?orient=${orient}&seq=${seq}&t=${Date.now()}`;
  const img = new Image();
  img.onload = () => {
    state.shownSeq = seq;
    state.shownCrc = crc;
    state.seq = Math.max(state.seq, seq);
    el.screen.src = url;
    el.screen.classList.add("on");
    el.screenEmpty.classList.add("off");
    sweep("REFRESH");
  };
  img.onerror = () => { if (seq > 0) toast("帧读取失败，可点「立即截图」重试", "warn"); };
  img.src = url;
}
function busy(on) {
  state.pending += on ? 1 : -1;
  if (state.pending < 0) state.pending = 0;
  el.chargeLed.classList.toggle("busy", state.pending > 0);
}

/* ---------- keys ---------- */
function flashKey(key) {
  const node = document.querySelector(`[data-key="${key}"]`);
  if (!node) return;
  node.classList.add("pressed");
  setTimeout(() => node.classList.remove("pressed"), 200);
}
function press(key) {
  flashKey(key);
  el.sbKey.textContent = `key ${key}`;
  const hold = el.holdMode.checked ? parseInt(el.holdMs.value, 10) : 0;
  const body = {
    key,
    holdMs: hold,
    settleMs: parseInt(el.settleMs.value, 10),
    followMs: parseInt(el.followMs.value, 10),
    shot: el.autoShot.checked,
  };
  busy(true);
  state.keyChain = state.keyChain.then(async () => {
    try {
      const r = await fetch("/api/key", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      const data = await r.json();
      if (!r.ok || !data.ok) throw new Error(data.error || `HTTP ${r.status}`);
      setCaptureMeta(data.seq, data.captureMs, data.crcOk);
      if (body.shot) loadScreen(data.seq, true, data.crc);
    } catch (err) {
      toast(`按键 ${key} 失败：${err.message}`, "err");
    } finally {
      busy(false);
    }
  });
}
document.querySelectorAll("[data-key]").forEach((btn) => {
  btn.addEventListener("click", () => press(btn.dataset.key));
});
window.addEventListener("keydown", (e) => {
  const tag = (e.target.tagName || "").toLowerCase();
  if (tag === "input" || tag === "select" || tag === "textarea") return;
  const key = KEYMAP[e.key];
  if (!key) return;
  e.preventDefault();
  press(key);
});

/* ---------- deck controls ---------- */
el.holdMs.addEventListener("input", () => { el.holdMsOut.textContent = `${el.holdMs.value}ms`; });
el.settleMs.addEventListener("input", () => { el.settleMsOut.textContent = `${el.settleMs.value}ms`; });
el.followMs.addEventListener("input", () => { el.followMsOut.textContent = `${el.followMs.value}ms`; });
el.holdMode.addEventListener("change", () => {
  el.holdBadge.textContent = el.holdMode.checked ? `长按 ${el.holdMs.value}ms` : "短按";
});
el.holdMs.addEventListener("input", () => {
  if (el.holdMode.checked) el.holdBadge.textContent = `长按 ${el.holdMs.value}ms`;
});
for (const sw of [el.autoShot, el.autoScroll]) {
  sw.addEventListener("change", () => {
    sw.closest(".param").querySelector("output").textContent = sw.checked ? "ON" : "OFF";
  });
}
el.orient.addEventListener("change", () => {
  el.screenWrap.classList.toggle("native", el.orient.value === "land");
  if (state.seq > 0) loadScreen(state.seq, true);
});
el.btnShot.addEventListener("click", async () => {
  el.btnShot.disabled = true;
  busy(true);
  sweep("CAPTURE");
  try {
    const r = await fetch("/api/shot", { method: "POST" });
    const data = await r.json();
    if (!r.ok || !data.ok) throw new Error(data.error || `HTTP ${r.status}`);
    setCaptureMeta(data.seq, data.captureMs, data.crcOk);
    loadScreen(data.seq, true, data.crc);
  } catch (err) {
    toast(`截图失败：${err.message}`, "err");
  } finally {
    el.btnShot.disabled = false;
    busy(false);
  }
});
el.btnReconnect.addEventListener("click", async () => {
  try {
    const r = await fetch("/api/reconnect", { method: "POST" });
    const data = await r.json();
    if (!data.ok) throw new Error(data.error || "reconnect failed");
    toast("串口重连中，设备将复位…", "warn");
  } catch (err) {
    toast(`重连失败：${err.message}`, "err");
  }
});
el.btnInfo.addEventListener("click", async () => {
  try {
    const r = await fetch("/api/info", { method: "POST" });
    const data = await r.json();
    if (!r.ok || !data.ok) throw new Error(data.error || `HTTP ${r.status}`);
    el.infoUrl.textContent = data.info.url || "—";
    el.infoDevice.textContent = data.info.deviceId || "—";
    el.infoToken.textContent = data.info.tokenSet === "1" || data.info.tokenSet === "true" ? "已配置（掩码）" : "未配置";
    el.infoBox.hidden = false;
    toast("云信息已读取", "ok");
  } catch (err) {
    toast(`云信息失败：${err.message}`, "err");
  }
});
el.logFilter.addEventListener("input", () => { state.filter = el.logFilter.value.trim(); renderLog(); });
el.btnPause.addEventListener("click", () => {
  state.paused = !state.paused;
  el.btnPause.textContent = state.paused ? "继续" : "暂停";
  el.btnPause.classList.toggle("active", state.paused);
  if (!state.paused && el.autoScroll.checked) el.log.scrollTop = el.log.scrollHeight;
});
el.btnClear.addEventListener("click", () => {
  state.lines = [];
  state.linesTotal = 0;
  renderLog();
});

/* ---------- input desk: text + microphone inject ---------- */
const MIC_CAP_MS = 8000;
let mic = null;

function inState(s) { el.inState.textContent = s; }
function voiceFollow() { return Math.max(parseInt(el.followMs.value, 10), 15000); }

el.inText.addEventListener("input", () => {
  el.inCount.textContent = `${el.inText.value.length}/400`;
});
el.inText.addEventListener("keydown", (e) => {
  if ((e.ctrlKey || e.metaKey) && e.key === "Enter") { e.preventDefault(); sendText(); }
});
async function sendText() {
  const text = el.inText.value.trim();
  if (!text) { toast("先输入文本", "warn"); return; }
  el.btnText.disabled = true;
  inState("发送中");
  busy(true);
  try {
    const r = await fetch("/api/text", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ text, followMs: voiceFollow() }),
    });
    const d = await r.json();
    if (!r.ok || !d.ok) throw new Error(d.error || `HTTP ${r.status}`);
    if (d.accepted) {
      inState("设备处理中");
      toast("文本已接受，等待设备回合上屏", "ok");
    } else {
      inState("被拒绝");
      toast("设备拒绝注入：请先进入语音输入界面（如 AI助手）", "warn", 5000);
    }
  } catch (err) {
    inState("失败");
    toast(`文本注入失败：${err.message}`, "err");
  } finally {
    el.btnText.disabled = false;
    busy(false);
  }
}
el.btnText.addEventListener("click", sendText);

function micAvailable() {
  return window.isSecureContext && navigator.mediaDevices && navigator.mediaDevices.getUserMedia;
}
if (!micAvailable()) {
  el.btnMic.disabled = true;
  el.micHint.textContent = "麦克风需要安全上下文（localhost 或 HTTPS）。当前访问方式下录音不可用，文本注入不受影响。";
}
async function micStart() {
  if (mic || !micAvailable()) return;
  let stream;
  try {
    stream = await navigator.mediaDevices.getUserMedia({
      audio: { echoCancellation: true, noiseSuppression: true, autoGainControl: true },
    });
  } catch (err) {
    toast("麦克风不可用或被拒绝授权", "err");
    el.micHint.textContent = `麦克风授权失败：${err.name || err.message}。可在浏览器地址栏权限设置中允许后重试。`;
    return;
  }
  const ctx = new AudioContext({ sampleRate: 16000 });
  await ctx.resume();
  const src = ctx.createMediaStreamSource(stream);
  const proc = ctx.createScriptProcessor(2048, 1, 1);
  const chunks = [];
  proc.onaudioprocess = (e) => {
    const f = e.inputBuffer.getChannelData(0);
    const i16 = new Int16Array(f.length);
    let sum = 0;
    for (let i = 0; i < f.length; i++) {
      const s = Math.max(-1, Math.min(1, f[i]));
      i16[i] = s < 0 ? s * 0x8000 : s * 0x7fff;
      sum += s * s;
    }
    chunks.push(i16);
    const rms = Math.sqrt(sum / f.length);
    el.meterFill.style.width = `${Math.min(100, Math.round(rms * 320))}%`;
  };
  src.connect(proc);
  proc.connect(ctx.destination);
  mic = { ctx, src, proc, stream, chunks, t0: performance.now(), raf: 0 };
  el.btnMic.classList.add("rec");
  el.btnMic.setAttribute("aria-pressed", "true");
  el.micLabel.textContent = "停止录音";
  inState("录音中");
  const tick = () => {
    if (!mic) return;
    const ms = performance.now() - mic.t0;
    el.micTime.textContent = `${(ms / 1000).toFixed(1)}s`;
    if (ms >= MIC_CAP_MS) { toast("已达 8 s 录音上限，自动上传", "warn"); micStop(); return; }
    mic.raf = requestAnimationFrame(tick);
  };
  mic.raf = requestAnimationFrame(tick);
}
function micStop(upload = true) {
  if (!mic) return;
  const m = mic;
  mic = null;
  cancelAnimationFrame(m.raf);
  m.proc.disconnect();
  m.src.disconnect();
  m.stream.getTracks().forEach((t) => t.stop());
  m.ctx.close();
  el.btnMic.classList.remove("rec");
  el.btnMic.setAttribute("aria-pressed", "false");
  el.micLabel.textContent = "开始录音";
  el.meterFill.style.width = "0%";
  if (!upload) { inState("就绪"); return; }
  const total = m.chunks.reduce((n, c) => n + c.length, 0);
  if (total < 1600) { toast("录音太短（<0.1 s）", "warn"); inState("就绪"); return; }
  const pcm = new Int16Array(total);
  let off = 0;
  for (const c of m.chunks) { pcm.set(c, off); off += c.length; }
  inState("上传中");
  busy(true);
  fetch(`/api/audio?followMs=${voiceFollow()}`, {
    method: "POST",
    headers: { "Content-Type": "application/octet-stream" },
    body: pcm.buffer,
  })
    .then(async (r) => {
      const d = await r.json();
      if (!r.ok || !d.ok) throw new Error(d.error || `HTTP ${r.status}`);
      if (d.accepted) {
        inState("设备处理中");
        toast(`音频已接受（${(d.samples / 16000).toFixed(1)} s），等待转写与答复上屏`, "ok");
      } else {
        inState("被拒绝");
        toast(d.reason ? `设备拒绝音频：${d.reason}` : "设备拒绝音频：请先进入语音输入界面且空闲", "warn", 6000);
      }
    })
    .catch((err) => { inState("失败"); toast(`音频注入失败：${err.message}`, "err"); })
    .finally(() => busy(false));
}
el.btnMic.addEventListener("click", () => (mic ? micStop() : micStart()));

/* ---------- serial port picker ---------- */
async function loadPorts(silent = false) {
  try {
    const r = await fetch("/api/ports");
    const d = await r.json();
    if (!d.ok) throw new Error(d.error || `HTTP ${r.status}`);
    el.portSel.textContent = "";
    if (!d.ports.length) {
      const o = document.createElement("option");
      o.value = "";
      o.textContent = "（未检测到串口）";
      el.portSel.appendChild(o);
    }
    for (const p of d.ports) {
      const o = document.createElement("option");
      o.value = p.device;
      const desc = (p.desc || "").replace(/\s*\(.*\)$/, "");
      o.textContent = `${p.device} · ${desc}`.slice(0, 52);
      el.portSel.appendChild(o);
    }
    const cur = d.current && d.ports.some((p) => p.device === d.current) ? d.current : (d.ports[0] || {}).device || "";
    el.portSel.value = cur;
    if (!silent) toast(`检测到 ${d.ports.length} 个串口`, "info", 2200);
  } catch (err) {
    if (!silent) toast(`串口列表失败：${err.message}`, "err");
  }
}
el.btnPorts.addEventListener("click", () => loadPorts());
el.btnConnect.addEventListener("click", async () => {
  const port = el.portSel.value;
  if (!port) { toast("没有可选串口", "warn"); return; }
  el.btnConnect.disabled = true;
  try {
    const r = await fetch("/api/connect", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ port }),
    });
    const d = await r.json();
    if (!r.ok || !d.ok) throw new Error(d.error || `HTTP ${r.status}`);
    toast(`切换到 ${port}，设备将复位…`, "warn");
  } catch (err) {
    toast(`连接失败：${err.message}`, "err");
  } finally {
    el.btnConnect.disabled = false;
  }
});

/* ---------- function tour ---------- */
const tour = { running: false, abort: false, done: 0 };
const TOUR_NOTE_DEFAULT = el.tourNote.textContent;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function tourRow(flow) {
  const row = document.createElement("div");
  row.className = "tour-row";
  row.id = `tr-${flow.id}`;
  const run = document.createElement("button");
  run.className = "tool tour-run";
  run.textContent = "运行";
  run.addEventListener("click", () => {
    if (tour.running) { toast("巡检进行中，请先停止", "warn"); return; }
    runFlows([flow]);
  });
  const name = document.createElement("div");
  name.className = "tour-name";
  name.textContent = flow.name;
  const grp = document.createElement("span");
  grp.className = "tour-group mono";
  grp.textContent = flow.group;
  name.appendChild(grp);
  const st = document.createElement("span");
  st.className = "tour-state mono";
  st.textContent = "未跑";
  const detail = document.createElement("div");
  detail.className = "tour-detail mono";
  row.append(run, name, st, detail);
  return row;
}
function setTourState(id, cls, text, detail = "") {
  const row = $(`tr-${id}`);
  if (!row) return;
  const st = row.querySelector(".tour-state");
  st.className = `tour-state mono ${cls}`;
  st.textContent = text;
  row.querySelector(".tour-detail").textContent = detail;
  if (cls === "run") row.scrollIntoView({ block: "nearest", behavior: "smooth" });
}
function lastMenuAct() {
  for (let i = state.lines.length - 1; i >= 0 && i >= state.lines.length - 160; i--) {
    const m = state.lines[i].match(/\[MENU\]\s*sel=\d+\s+act=(\d+)/);
    if (m) return parseInt(m[1], 10);
  }
  return null;
}
// Index marks break when the 4000-line cap splices the head off; a cumulative
// counter survives truncation: the last (total - mark) lines are exactly the
// ones pushed since the mark (or all of them if truncation ate older ones).
function linesSince(mark) {
  const n = state.linesTotal - mark;
  if (n <= 0) return [];
  return n >= state.lines.length ? state.lines : state.lines.slice(-n);
}
async function tourShot() {
  for (let attempt = 0; ; attempt++) {
    const r = await fetch("/api/shot", { method: "POST" });
    const d = await r.json();
    if (r.ok && d.ok) {
      setCaptureMeta(d.seq, d.captureMs, d.crcOk);
      loadScreen(d.seq, true, d.crc);
      return d.crc;
    }
    // loopTask can be inside a blocking relay HTTP call (up to 30 s) and miss
    // the SCREENSHOT command; one retry after a pause covers that window.
    if (attempt >= 1 || !/timed out/.test(d.error || "")) throw new Error(d.error || `HTTP ${r.status}`);
    await sleep(4000);
  }
}
async function tourKey(key, hold = 0) {
  const r = await fetch("/api/key", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ key, holdMs: hold, settleMs: 260, followMs: 0, shot: false }),
  });
  const d = await r.json();
  if (!r.ok || !d.ok) throw new Error(d.error || `HTTP ${r.status}`);
}
async function runFlow(flow) {
  setTourState(flow.id, "run", "运行中");
  el.tourNote.textContent = `巡检中：${flow.name} …`;
  let lastCrc = null;
  let logMark = state.linesTotal;
  const t0 = performance.now();
  for (const step of flow.steps) {
    if (tour.abort) throw new Error("已停止");
    if (step.cmd) {
      const r = await fetch("/api/debug", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ cmd: step.cmd }),
      });
      const d = await r.json();
      if (!r.ok || !d.ok) throw new Error(d.error || `HTTP ${r.status}`);
    } else if (step.k) {
      const n = step.n || 1;
      for (let i = 0; i < n; i++) {
        if (tour.abort) throw new Error("已停止");
        await tourKey(step.k, step.hold || 0);
        await sleep(step.gap || 220);
      }
    } else if (step.wait) {
      await sleep(step.wait);
    } else if (step.shot) {
      lastCrc = await tourShot();
    } else if (step.screen) {
      const crc = await tourShot();
      if (lastCrc !== null && crc === lastCrc) throw new Error("画面未变化");
      lastCrc = crc;
    } else if (step.log) {
      const re = new RegExp(step.log);
      const end = Date.now() + step.ms;
      let hit = false;
      while (Date.now() < end) {
        if (tour.abort) throw new Error("已停止");
        if (linesSince(logMark).some((l) => re.test(l))) { hit = true; break; }
        await sleep(150);
      }
      if (!hit) throw new Error(`日志未出现：${step.log}`);
      logMark = state.linesTotal;
    } else if (step.menu !== undefined) {
      let found = false;
      for (let i = 0; i < 24; i++) {
        if (tour.abort) throw new Error("已停止");
        if (lastMenuAct() === step.menu) { found = true; break; }
        await tourKey("down");
        await sleep(220);
      }
      if (!found) throw new Error(`菜单未定位到 act=${step.menu}`);
    } else if (step.text) {
      let d = null;
      for (let attempt = 0; attempt < 2; attempt++) {
        const r = await fetch("/api/text", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ text: step.text, followMs: 0 }),
        });
        d = await r.json();
        if (!r.ok || !d.ok) throw new Error(d.error || `HTTP ${r.status}`);
        if (d.accepted) break;
        await sleep(3000);
      }
      if (!d.accepted) throw new Error("设备拒绝文本注入");
    } else if (step.audio) {
      const buf = await (await fetch("/static/testvoice.pcm")).arrayBuffer();
      let d = null;
      for (let attempt = 0; attempt < 2; attempt++) {
        const r = await fetch("/api/audio?followMs=0", {
          method: "POST",
          headers: { "Content-Type": "application/octet-stream" },
          body: buf,
        });
        d = await r.json();
        if (!r.ok || !d.ok) throw new Error(d.error || `HTTP ${r.status}`);
        if (d.accepted) break;
        await sleep(3000);
      }
      if (!d.accepted) throw new Error("设备拒绝音频注入" + (d.reason ? `：${d.reason}` : ""));
    }
  }
  const secs = ((performance.now() - t0) / 1000).toFixed(1);
  setTourState(flow.id, "pass", "通过", `${secs}s`);
}
async function runFlows(flows) {
  tour.running = true;
  tour.abort = false;
  tour.done = 0;
  el.btnTourAll.disabled = true;
  el.btnTourStop.disabled = false;
  el.tourFill.style.width = "0%";
  for (const flow of flows) {
    if (tour.abort) { setTourState(flow.id, "", "未跑"); continue; }
    try {
      await runFlow(flow);
    } catch (err) {
      setTourState(flow.id, tour.abort ? "" : "fail", tour.abort ? "未跑" : "失败", err.message);
      if (!tour.abort) toast(`巡检「${flow.name}」失败：${err.message}`, "err", 4500);
    }
    tour.done += 1;
    el.tourFill.style.width = `${Math.round((tour.done / flows.length) * 100)}%`;
  }
  tour.running = false;
  el.btnTourAll.disabled = false;
  el.btnTourStop.disabled = true;
  el.tourNote.textContent = TOUR_NOTE_DEFAULT;
  if (!tour.abort) {
    const fails = flows.filter((f) => $(`tr-${f.id}`).querySelector(".tour-state.fail")).length;
    toast(fails ? `巡检完成：${flows.length - fails}/${flows.length} 通过` : `巡检完成：${flows.length} 项全部通过`, fails ? "warn" : "ok", 5000);
  } else {
    toast("巡检已停止", "warn");
  }
}
el.btnTourAll.addEventListener("click", () => {
  if (tour.running) return;
  if (!state.connected) { toast("串口未连接，无法巡检", "warn"); return; }
  runFlows(window.FLOWS);
});
el.btnTourStop.addEventListener("click", () => { tour.abort = true; });
for (const flow of window.FLOWS) el.tourList.appendChild(tourRow(flow));

/* ---------- status ---------- */
function resync() {
  fetch("/api/state")
    .then((r) => r.json())
    .then((s) => {
      setStatus(s);
      setCaptureMeta(s.seq, s.captureMs, s.crcOk);
      if (s.hasScreen) loadScreen(s.seq, true, s.crc);
    })
    .catch(() => {});
}
function setStatus(s) {
  const was = state.connected;
  state.connected = !!s.connected;
  el.led.className = `led ${state.connected ? "ok" : "bad"}`;
  el.connText.textContent = state.connected ? "串口已连接" : "串口断开";
  el.portChip.textContent = s.port || "/dev/ttyACM0";
  el.sbPort.textContent = `port ${s.port || "—"}`;
  if (s.port && [...el.portSel.options].some((o) => o.value === s.port)) el.portSel.value = s.port;
  if (was !== null && was !== state.connected) {
    toast(state.connected ? "串口已连接" : `串口断开${s.error ? "：" + s.error : ""}`, state.connected ? "ok" : "err");
    if (state.connected) resync();
  }
}

/* ---------- SSE ---------- */
function connect() {
  const es = new EventSource("/api/events");
  es.addEventListener("backlog", (e) => {
    state.lines = JSON.parse(e.data);
    state.linesTotal = state.lines.length;
    renderLog();
  });
  es.addEventListener("log", (e) => pushLine(JSON.parse(e.data)));
  es.addEventListener("status", (e) => setStatus(JSON.parse(e.data)));
  es.addEventListener("screen", (e) => {
    const d = JSON.parse(e.data);
    if (d.crcOk) {
      setCaptureMeta(d.seq, d.ms, true);
      loadScreen(d.seq, false, d.crc);
      if (el.inState.textContent === "设备处理中") inState("画面已更新");
    } else toast(d.error || "帧 CRC 校验失败", "warn");
  });
  es.addEventListener("key", (e) => {
    const d = JSON.parse(e.data);
    if (d.source === "device") {
      // Pressed on the reader itself; no virtual key to flash.
      el.sbKey.textContent = "物理按键";
      return;
    }
    flashKey(d.key);
    el.sbKey.textContent = `key ${d.key}${d.holdMs ? " hold " + d.holdMs + "ms" : ""}`;
  });
  es.onerror = () => {
    el.led.className = "led bad";
    el.connText.textContent = "通道重连中…";
  };
  es.onopen = () => resync();
}

loadPorts(true);
fetch("/api/state")
  .then((r) => r.json())
  .then((s) => {
    setStatus(s);
    setCaptureMeta(s.seq, s.captureMs, s.crcOk);
    if (s.hasScreen) loadScreen(s.seq, true, s.crc);
  })
  .catch(() => toast("无法读取服务状态", "err"))
  .finally(connect);