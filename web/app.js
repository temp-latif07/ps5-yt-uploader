// PS5 YouTube Uploader web UI. Talks only to the console; the console streams directly to YouTube.
"use strict";

const $ = (s) => document.querySelector(s);

// --- Compact ISO/IEC 18004 QR Code SVG Generator ---
const QR = (() => {
  const EXP = new Array(256), LOG = new Array(256);
  for (let i = 0; i < 8; i++) EXP[i] = 1 << i;
  for (let i = 8; i < 256; i++) EXP[i] = EXP[i - 4] ^ EXP[i - 5] ^ EXP[i - 6] ^ EXP[i - 8];
  for (let i = 0; i < 255; i++) LOG[EXP[i]] = i;
  const gexp = (n) => { while (n < 0) n += 255; while (n >= 255) n -= 255; return EXP[n]; };
  const glog = (n) => { if (n < 1) throw new Error("glog"); return LOG[n]; };
  function Poly(num, shift) {
    let off = 0; while (off < num.length && num[off] === 0) off++;
    this.num = new Array(num.length - off + shift).fill(0);
    for (let i = 0; i < num.length - off; i++) this.num[i] = num[i + off];
  }
  Poly.prototype.mul = function (e) {
    const n = new Array(this.num.length + e.num.length - 1).fill(0);
    for (let i = 0; i < this.num.length; i++)
      for (let j = 0; j < e.num.length; j++) n[i + j] ^= gexp(glog(this.num[i]) + glog(e.num[j]));
    return new Poly(n, 0);
  };
  Poly.prototype.mod = function (e) {
    if (this.num.length - e.num.length < 0) return this;
    const r = glog(this.num[0]) - glog(e.num[0]), n = this.num.slice();
    for (let i = 0; i < e.num.length; i++) n[i] ^= gexp(glog(e.num[i]) + r);
    return new Poly(n, 0).mod(e);
  };
  const RS = [[1, 26, 19], [1, 44, 34], [1, 70, 55], [1, 100, 80], [1, 134, 108], [2, 86, 68], [2, 98, 78], [2, 121, 97], [2, 146, 116], [2, 86, 68, 2, 87, 69]];
  const PAT = [[], [6, 18], [6, 22], [6, 26], [6, 30], [6, 34], [6, 22, 38], [6, 24, 42], [6, 26, 46], [6, 28, 50]];
  const G15 = (1 << 10) | (1 << 8) | (1 << 5) | (1 << 4) | (1 << 2) | (1 << 1) | 1, G15M = (1 << 14) | (1 << 12) | (1 << 10) | (1 << 4) | 1;
  const digit = (d) => { let n = 0; while (d) { n++; d >>>= 1; } return n; };
  const bch = (d) => { let v = d << 10; while (digit(v) - digit(G15) >= 0) v ^= G15 << (digit(v) - digit(G15)); return ((d << 10) | v) ^ G15M; };
  const maskFn = (m, i, j) => [
    (i + j) % 2 === 0, i % 2 === 0, j % 3 === 0, (i + j) % 3 === 0,
    (Math.floor(i / 2) + Math.floor(j / 3)) % 2 === 0, ((i * j) % 2) + ((i * j) % 3) === 0,
    (((i * j) % 2) + ((i * j) % 3)) % 2 === 0, (((i * j) % 3) + ((i + j) % 2)) % 2 === 0
  ][m];
  function blocks(type) {
    const t = RS[type - 1], list = [];
    for (let i = 0; i < t.length; i += 3) for (let j = 0; j < t[i]; j++) list.push({ total: t[i + 1], data: t[i + 2] });
    return list;
  }
  function encode(text, size) {
    let type = 1;
    while (type < RS.length) {
      const tot = blocks(type).reduce((a, b) => a + b.data, 0) * 8;
      if (4 + (type < 10 ? 8 : 16) + text.length * 8 <= tot) break;
      type++;
    }
    const blks = blocks(type), buf = []; let blen = 0;
    const put = (num, l) => {
      for (let i = 0; i < l; i++) {
        const bit = ((num >>> (l - i - 1)) & 1) === 1, bi = Math.floor(blen / 8);
        if (buf.length <= bi) buf.push(0);
        if (bit) buf[bi] |= 0x80 >>> (blen % 8);
        blen++;
      }
    };
    put(4, 4); put(text.length, type < 10 ? 8 : 16);
    for (let i = 0; i < text.length; i++) put(text.charCodeAt(i), 8);
    const totalData = blks.reduce((a, b) => a + b.data, 0);
    if (blen + 4 <= totalData * 8) put(0, 4);
    while (blen % 8 !== 0) put(0, 1);
    while (blen < totalData * 8) { put(0xec, 8); if (blen < totalData * 8) put(0x11, 8); }
    let off = 0, maxDc = 0, maxEc = 0;
    const dcs = [], ecs = [];
    for (const b of blks) {
      const dc = b.data, ec = b.total - dc;
      maxDc = Math.max(maxDc, dc); maxEc = Math.max(maxEc, ec);
      const d = buf.slice(off, off + dc); off += dc; dcs.push(d);
      let ep = new Poly([1], 0);
      for (let i = 0; i < ec; i++) ep = ep.mul(new Poly([1, gexp(i)], 0));
      const mod = new Poly(d, ep.num.length - 1).mod(ep);
      const e = new Array(ep.num.length - 1);
      for (let i = 0; i < e.length; i++) { const mi = i + mod.num.length - e.length; e[i] = mi >= 0 ? mod.num[mi] : 0; }
      ecs.push(e);
    }
    const data = [];
    for (let i = 0; i < maxDc; i++) for (const d of dcs) if (i < d.length) data.push(d[i]);
    for (let i = 0; i < maxEc; i++) for (const e of ecs) if (i < e.length) data.push(e[i]);
    const n = type * 4 + 17;
    const build = (mask, test) => {
      const m = Array.from({ length: n }, () => new Array(n).fill(null));
      const finder = (r0, c0) => {
        for (let r = -1; r <= 7; r++) if (r0 + r >= 0 && r0 + r < n)
          for (let c = -1; c <= 7; c++) if (c0 + c >= 0 && c0 + c < n)
            m[r0 + r][c0 + c] = (r >= 0 && r <= 6 && (c === 0 || c === 6)) || (c >= 0 && c <= 6 && (r === 0 || r === 6)) || (r >= 2 && r <= 4 && c >= 2 && c <= 4);
      };
      finder(0, 0); finder(n - 7, 0); finder(0, n - 7);
      const pos = PAT[type - 1] || [];
      for (const r0 of pos) for (const c0 of pos) if (m[r0][c0] === null)
        for (let r = -2; r <= 2; r++) for (let c = -2; c <= 2; c++) m[r0 + r][c0 + c] = Math.abs(r) === 2 || Math.abs(c) === 2 || (r === 0 && c === 0);
      for (let i = 8; i < n - 8; i++) { if (m[i][6] === null) m[i][6] = i % 2 === 0; if (m[6][i] === null) m[6][i] = i % 2 === 0; }
      const bits = bch((1 << 3) | mask);
      for (let i = 0; i < 15; i++) {
        const mod = !test && ((bits >> i) & 1) === 1;
        if (i < 6) m[i][8] = mod; else if (i < 8) m[i + 1][8] = mod; else m[n - 15 + i][8] = mod;
        if (i < 8) m[8][n - i - 1] = mod; else if (i < 9) m[8][15 - i] = mod; else m[8][15 - i - 1] = mod;
      }
      m[n - 8][8] = !test;
      let inc = -1, row = n - 1, bi = 7, xi = 0;
      for (let col = n - 1; col > 0; col -= 2) {
        if (col === 6) col--;
        while (true) {
          for (let c = 0; c < 2; c++) if (m[row][col - c] === null) {
            let dk = xi < data.length && (((data[xi] >>> bi) & 1) === 1);
            if (maskFn(mask, row, col - c)) dk = !dk;
            m[row][col - c] = dk;
            if (--bi === -1) { xi++; bi = 7; }
          }
          row += inc;
          if (row < 0 || row >= n) { row -= inc; inc = -inc; break; }
        }
      }
      return m;
    };
    let best = 0, bestLoss = 1e9;
    for (let mask = 0; mask < 8; mask++) {
      const m = build(mask, true); let loss = 0;
      for (let r = 0; r < n; r++) for (let c = 0; c < n; c++) {
        let same = 0;
        for (let dr = -1; dr <= 1; dr++) for (let dc = -1; dc <= 1; dc++)
          if ((dr || dc) && r + dr >= 0 && r + dr < n && c + dc >= 0 && c + dc < n && m[r][c] === m[r + dr][c + dc]) same++;
        if (same > 5) loss += 3 + same - 5;
      }
      if (loss < bestLoss) { bestLoss = loss; best = mask; }
    }
    const m = build(best, false), pad = 2, sz = n + pad * 2;
    let d = "";
    for (let r = 0; r < n; r++) for (let c = 0; c < n; c++) if (m[r][c]) d += `M${c + pad},${r + pad}h1v1h-1z`;
    return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${sz} ${sz}" width="${size}" height="${size}" shape-rendering="crispEdges"><rect width="${sz}" height="${sz}" fill="#fff"/><path d="${d}" fill="#111"/></svg>`;
  }
  return { svg: encode };
})();

const state = { tab: "clips", status: null, clips: [], jobs: [], pollTimer: 0 };

function el(tag, props, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(props || {})) {
    if (k === "class") e.className = v;
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else if (v !== undefined && v !== null && v !== false) e.setAttribute(k, v === true ? "" : v);
  }
  for (const c of children.flat()) if (c !== null && c !== undefined && c !== false) e.append(c.nodeType ? c : String(c));
  return e;
}

async function api(path, form) {
  const opts = form ? { method: "POST", headers: { "X-PS5YT": "1" }, body: new URLSearchParams(form) } : {};
  let res;
  try { res = await fetch("/api/" + path, opts); } catch (e) { setConn(false); throw new Error("console not reachable"); }
  setConn(true);
  let data = null;
  try { data = await res.json(); } catch (e) { /* non-JSON */ }
  if (!res.ok || !data || data.ok === false) {
    const err = new Error((data && data.error) || ("HTTP " + res.status));
    err.status = res.status; err.data = data; throw err;
  }
  return data;
}

function setConn(ok) {
  const c = $("#conn");
  c.textContent = ok ? (state.status ? "console " + state.status.ip : "connected") : "console offline";
  c.className = "pill " + (ok ? "ok" : "bad");
}

let toastTimer = 0;
function toast(msg) {
  const t = $("#toast");
  t.textContent = msg; t.classList.remove("hidden");
  clearTimeout(toastTimer); toastTimer = setTimeout(() => t.classList.add("hidden"), 3500);
}

function size(n) {
  if (n === undefined || n === null || n < 0) return "?";
  const u = ["B", "KB", "MB", "GB", "TB"]; let i = 0, v = n;
  while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
  return (i ? v.toFixed(v < 10 ? 2 : 1) : v) + " " + u[i];
}
function duration(s) {
  if (!isFinite(s) || s < 0) return "";
  s = Math.round(s);
  if (s < 60) return s + "s";
  if (s < 3600) return Math.floor(s / 60) + "m " + (s % 60) + "s";
  return Math.floor(s / 3600) + "h " + Math.floor((s % 3600) / 60) + "m";
}
function fmtDate(sec) {
  if (!sec) return "";
  const d = new Date(sec * 1000);
  return d.toISOString().slice(0, 16).replace("T", " ");
}
function consoleUrl() {
  const s = state.status;
  if (s && s.ip && s.ip !== "127.0.0.1") return "http://" + s.ip + ":" + s.port;
  return location.origin;
}

async function copyText(txt, okMsg) {
  try {
    await navigator.clipboard.writeText(txt);
    toast(okMsg || "Copied");
  } catch (e) {
    toast(txt);
  }
}

// ---------- status & auth ----------
async function loadStatus() {
  try {
    state.status = await api("status");
  } catch (e) {
    setConn(false);
    setTimeout(loadStatus, 3000);
    return;
  }
  setConn(true);
  renderStatus();
}

function renderStatus() {
  const s = state.status;
  const url = consoleUrl();
  $("#helper-cmd").textContent = "python3 tools/yt-login.py " + url;
  $("#qr-url").textContent = url;
  $("#login-qr").innerHTML = QR.svg(url, 156);
  $("#modal-qr").innerHTML = QR.svg(url, 220);
  $("#modal-qr-url").value = url;

  if (!s.logged_in) {
    $("#login").classList.remove("hidden");
    $("#app").classList.add("hidden");
    $("#cookie-file-path").textContent = s.cookie_file || "cookies.txt";
    $("#login-error").textContent = s.auth_error || "";
    clearTimeout(state.pollTimer);
    return;
  }
  $("#login").classList.add("hidden");
  $("#app").classList.remove("hidden");
  const ch = s.channel || {};
  $("#account").textContent = "Signed in as " + (ch.name || "YouTube") + (ch.id ? " (" + ch.id + ")" : "") + " · " + s.mode + " " + s.version;
  if (!$("#folder").value) $("#folder").value = s.captures_dir;
  applySettingsForm(s.settings);
  showTab(state.tab);
  pollJobs();
}

function applySettingsForm(st) {
  if (!st) return;
  $("#set-privacy").value = st.default_privacy || "UNLISTED";
  $("#set-mfk").value = st.made_for_kids ? "1" : "0";
  $("#set-chunk").value = String(st.chunk_mb || 8);
  $("#set-parallel").value = String(st.parallel || 1);
  $("#set-captures").value = st.captures_dir || "";
  $("#set-auto").checked = !!st.auto_upload;
}

$("#copy-cmd").addEventListener("click", () => copyText($("#helper-cmd").textContent, "Command copied"));
$("#btn-qr").addEventListener("click", () => {
  const url = consoleUrl();
  $("#modal-qr").innerHTML = QR.svg(url, 220);
  $("#modal-qr-url").value = url;
  $("#qr-modal").classList.remove("hidden");
});
$("#qr-close").addEventListener("click", () => $("#qr-modal").classList.add("hidden"));
$("#qr-modal").addEventListener("click", (ev) => { if (ev.target.id === "qr-modal") $("#qr-modal").classList.add("hidden"); });
$("#modal-qr-copy").addEventListener("click", () => copyText($("#modal-qr-url").value, "URL copied"));

$("#cookie-file").addEventListener("change", async (ev) => {
  const f = ev.target.files && ev.target.files[0];
  if (!f) return;
  $("#cookies").value = await f.text();
  toast("Loaded " + f.name);
});

$("#login-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const btn = ev.target.querySelector("button[type=submit]");
  $("#login-error").textContent = "";
  btn.disabled = true; btn.textContent = "Checking with YouTube...";
  try {
    state.status = await api("login", {
      cookies: $("#cookies").value,
      channel_id: $("#login-channel-id").value.trim(),
      delegated_session_id: $("#login-delegated").value.trim(),
      session_index: $("#login-sidx").value.trim(),
    });
    $("#cookies").value = "";
    renderStatus();
    loadClips();
  } catch (e) {
    $("#login-error").textContent = e.message;
  } finally {
    btn.disabled = false; btn.textContent = "Log in";
  }
});

$("#logout").addEventListener("click", async () => {
  if (!confirm("Log out and delete your stored YouTube cookies from the console?")) return;
  await api("logout", {}).catch((e) => toast(e.message));
  state.clips = [];
  loadStatus();
});

$("#quit").addEventListener("click", async () => {
  const active = state.jobs.filter((j) => ["queued", "starting", "uploading", "finalizing"].includes(j.state)).length;
  if (!confirm("Stop the server on the console?" + (active ? " " + active + " active upload(s) will pause and resume next time." : ""))) return;
  await api("quit", {}).catch(() => {});
  toast("Server stopped");
  setConn(false);
});

// ---------- tabs & clips ----------
function showTab(tab) {
  state.tab = tab;
  document.querySelectorAll("#tabs button").forEach((b) => b.classList.toggle("active", b.dataset.tab === tab));
  $("#clips-view").classList.toggle("hidden", tab !== "clips");
  $("#uploads-view").classList.toggle("hidden", tab !== "uploads");
  $("#settings-view").classList.toggle("hidden", tab !== "settings");
  if (tab === "clips" && !state.clips.length) loadClips();
  if (tab === "uploads") renderJobs();
}
document.querySelectorAll("#tabs button").forEach((b) => b.addEventListener("click", () => showTab(b.dataset.tab)));

async function loadClips() {
  const dir = $("#folder").value.trim();
  $("#clips-status").textContent = "Scanning clips...";
  try {
    const r = await api("clips" + (dir ? "?dir=" + encodeURIComponent(dir) : ""));
    state.clips = Array.isArray(r.clips) ? r.clips : [];
    renderClips();
  } catch (e) {
    $("#clips-status").textContent = "Could not load clips: " + e.message;
    $("#clips").replaceChildren();
  }
}

$("#refresh").addEventListener("click", loadClips);
$("#filter").addEventListener("input", renderClips);

function renderClips() {
  const q = $("#filter").value.trim().toLowerCase();
  const shown = q ? state.clips.filter((c) => (c.name + " " + c.title_id).toLowerCase().includes(q)) : state.clips;
  $("#clips-status").textContent = state.clips.length
    ? (shown.length === state.clips.length ? state.clips.length + " clip(s)" : shown.length + " of " + state.clips.length + " clips")
    : "No .mp4 or .webm clips found in this folder.";
  $("#clips").replaceChildren(...shown.map((c) => el("div", { class: "item" },
    el("div", { class: "item-name" }, c.name),
    el("div", { class: "meta" },
      c.title_id ? el("span", { class: "tag" }, c.title_id) : null,
      el("span", {}, size(c.size)),
      c.mtime ? el("span", {}, fmtDate(c.mtime)) : null,
      c.video_id ? el("a", { class: "yt-link", href: "https://youtu.be/" + c.video_id, target: "_blank", rel: "noopener" },
        "Uploaded (" + c.video_id + ")") : null),
    el("div", { class: "actions" },
      el("button", { class: "small", type: "button", onclick: () => openPreview(c) }, "Preview"),
      el("button", { class: "small primary", type: "button", onclick: () => openUpload(c) }, c.video_id ? "Upload again" : "Upload")))));
}

// ---------- upload modal ----------
function openUpload(c) {
  const st = (state.status && state.status.settings) || {};
  $("#up-path").value = c.path;
  $("#up-clip-info").textContent = c.name + " · " + size(c.size) + (c.title_id ? " · " + c.title_id : "");
  const baseTitle = c.name.replace(/\.[^.]+$/, "");
  $("#up-title").value = c.title_id ? c.title_id + " - " + baseTitle : baseTitle;
  $("#up-desc").value = "Recorded on PlayStation 5" + (c.title_id ? " (" + c.title_id + ")" : "");
  $("#up-privacy").value = st.default_privacy || "UNLISTED";
  $("#up-mfk").value = st.made_for_kids ? "1" : "0";
  $("#up-playlist").value = "";
  $("#up-error").textContent = "";
  syncMfkUi();
  $("#upload-modal").classList.remove("hidden");
  $("#up-title").focus();
}

function syncMfkUi() {
  const mfk = $("#up-mfk").value === "1";
  $("#up-playlist").disabled = mfk;
  if (mfk) $("#up-playlist").value = "";
  $("#up-mfk-note").classList.toggle("hidden", !mfk);
}
$("#up-mfk").addEventListener("change", syncMfkUi);
$("#up-close").addEventListener("click", () => $("#upload-modal").classList.add("hidden"));
$("#up-cancel").addEventListener("click", () => $("#upload-modal").classList.add("hidden"));
$("#upload-modal").addEventListener("click", (ev) => { if (ev.target.id === "upload-modal") $("#upload-modal").classList.add("hidden"); });

$("#upload-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const btn = $("#up-submit");
  $("#up-error").textContent = "";
  btn.disabled = true; btn.textContent = "Queuing...";
  try {
    await api("upload", {
      path: $("#up-path").value,
      title: $("#up-title").value.trim(),
      description: $("#up-desc").value,
      privacy: $("#up-privacy").value,
      made_for_kids: $("#up-mfk").value,
      playlist_id: $("#up-playlist").value.trim(),
    });
    $("#upload-modal").classList.add("hidden");
    toast("Queued upload");
    pollJobs();
    showTab("uploads");
  } catch (e) {
    $("#up-error").textContent = e.message;
  } finally {
    btn.disabled = false; btn.textContent = "Queue upload";
  }
});

// ---------- video preview ----------
function openPreview(c) {
  $("#prev-title").textContent = c.name;
  $("#prev-meta").textContent = (c.title_id ? c.title_id + " · " : "") + size(c.size);
  $("#prev-video").src = "/api/stream?path=" + encodeURIComponent(c.path);
  $("#preview-modal").classList.remove("hidden");
}
function closePreview() {
  const v = $("#prev-video");
  v.pause();
  v.removeAttribute("src");
  v.load();
  $("#preview-modal").classList.add("hidden");
}
$("#prev-close").addEventListener("click", closePreview);
$("#preview-modal").addEventListener("click", (ev) => { if (ev.target.id === "preview-modal") closePreview(); });

// ---------- uploads queue ----------
function updateBadge() {
  const active = state.jobs.filter((j) => ["queued", "starting", "uploading", "finalizing"].includes(j.state)).length;
  const b = $("#badge");
  b.textContent = active;
  b.classList.toggle("hidden", !active);
  return active;
}

async function pollJobs() {
  clearTimeout(state.pollTimer);
  if (!state.status || !state.status.logged_in) return;
  try {
    const r = await api("jobs");
    state.jobs = r.jobs || [];
  } catch (e) { /* retry below */ }
  const active = updateBadge();
  if (state.tab === "uploads") renderJobs();
  state.pollTimer = setTimeout(pollJobs, state.tab === "uploads" || active ? 1000 : 5000);
}

function jobAction(action, id) {
  return api("jobs/" + action, { id }).then(() => {
    pollJobs();
    if (action !== "clear") loadClips();
  }).catch((e) => toast(e.message));
}
$("#clear").addEventListener("click", () => jobAction("clear", 0));

function renderJobs() {
  const jobs = state.jobs.slice().reverse();
  $("#jobs-empty").classList.toggle("hidden", jobs.length > 0);
  $("#jobs").replaceChildren(...jobs.map((j) => {
    const pct = j.total > 0 ? Math.min(100, (j.done / j.total) * 100) : 0;
    const running = j.state === "uploading";
    const parts = [j.privacy || "UNLISTED"];
    if (j.total > 0) parts.push(size(j.done) + " of " + size(j.total) + " (" + pct.toFixed(1) + "%)");
    if (running && j.speed > 0) parts.push(size(j.speed) + "/s");
    if (running && j.speed > 0 && j.total > 0) parts.push(duration((j.total - j.done) / j.speed) + " left");
    const canPause = ["queued", "starting", "uploading"].includes(j.state);
    const canResume = ["paused", "failed", "cancelled"].includes(j.state);
    const canCancel = ["queued", "starting", "uploading", "paused", "failed"].includes(j.state);
    const stCls = j.state === "done" ? "ready" : j.state === "failed" ? "bad" : "busy";
    return el("div", { class: "item job " + j.state },
      el("div", { class: "item-name" }, j.title || j.name),
      el("div", { class: "meta" },
        el("span", { class: "state " + stCls }, j.state),
        el("span", {}, parts.join(" · "))),
      el("div", { class: "bar" }, el("div", { style: "width:" + (j.state === "done" ? 100 : pct) + "%" })),
      j.url ? el("div", { class: "meta" }, el("a", { class: "yt-link", href: j.url, target: "_blank", rel: "noopener" }, j.url)) : null,
      el("div", { class: "meta" }, el("code", {}, j.path)),
      j.err ? el("div", { class: "error small" }, j.err) : null,
      j.note ? el("div", { class: "muted small" }, j.note) : null,
      el("div", { class: "actions" },
        canPause ? el("button", { class: "small", type: "button", onclick: () => jobAction("pause", j.id) }, "Pause") : null,
        canResume ? el("button", { class: "small primary", type: "button", onclick: () => jobAction("resume", j.id) }, j.state === "failed" ? "Retry" : "Resume") : null,
        canCancel ? el("button", { class: "small danger", type: "button", onclick: () => jobAction("cancel", j.id) }, "Cancel") : null,
        !canPause ? el("button", { class: "small", type: "button", onclick: () => jobAction("remove", j.id) }, "Remove") : null));
  }));
}

// ---------- settings ----------
$("#settings-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  try {
    state.status = await api("settings", {
      default_privacy: $("#set-privacy").value,
      made_for_kids: $("#set-mfk").value,
      chunk_mb: $("#set-chunk").value,
      parallel: $("#set-parallel").value,
      captures_dir: $("#set-captures").value.trim(),
      auto_upload: $("#set-auto").checked ? "1" : "0",
    });
    $("#folder").value = state.status.captures_dir;
    applySettingsForm(state.status.settings);
    toast("Settings saved");
  } catch (e) {
    toast(e.message);
  }
});

// ---------- folder picker ----------
const picker = { path: "" };
$("#browse").addEventListener("click", () => openPicker($("#folder").value.trim() || state.status.captures_dir));
$("#picker-close").addEventListener("click", () => $("#picker").classList.add("hidden"));
$("#picker").addEventListener("click", (ev) => { if (ev.target.id === "picker") $("#picker").classList.add("hidden"); });
$("#picker-up").addEventListener("click", () => picker.parent && browse(picker.parent));
$("#picker-use").addEventListener("click", () => {
  if (!picker.path) return;
  $("#folder").value = picker.path;
  $("#picker").classList.add("hidden");
  loadClips();
});

async function openPicker(start) {
  $("#picker").classList.remove("hidden");
  let p = (start || "").replace(/\/+$/, "");
  for (let i = 0; i < 8 && p; i++) {
    if (await browse(p, true)) return;
    p = p.slice(0, p.lastIndexOf("/"));
  }
  const r = await api("dirs").catch(() => null);
  if (r && r.roots && r.roots.length) browse(r.roots[0]);
}

async function browse(path, quiet) {
  $("#picker-error").textContent = "";
  let r;
  try { r = await api("dirs?path=" + encodeURIComponent(path)); } catch (e) {
    if (!quiet) $("#picker-error").textContent = e.message;
    return false;
  }
  picker.path = r.path; picker.parent = r.parent;
  $("#picker-path").textContent = r.path;
  $("#picker-up").disabled = !r.parent;
  $("#picker-free").textContent = r.free !== undefined ? size(r.free) + " free" : "";
  $("#picker-roots").replaceChildren(...r.roots.map((root) => el("button", { type: "button", onclick: () => browse(root) },
    root.startsWith("/mnt/usb") ? "USB " + root.slice(8) : root.startsWith("/mnt/ext") ? "Ext " + root.slice(8) :
    root === "/user/av_content/video" ? "PS5 Captures" : root)));
  const items = r.dirs.map((d) => el("li", { onclick: () => browse(r.path.replace(/\/$/, "") + "/" + d) }, d + "/"));
  const files = (r.files || []).map((f) => el("li", { class: "file" }, el("span", {}, f.name), el("span", {}, size(f.size))));
  $("#picker-dirs").replaceChildren(...(items.length || files.length ? items.concat(files) : [el("li", { class: "empty" }, "Empty folder")]));
  return true;
}

loadStatus();
