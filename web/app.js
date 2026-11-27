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

const state = {
  tab: "clips",
  status: null,
  clips: [],
  jobs: [],
  playlists: null,
  gameFilter: "all",
  statusFilter: "all",
  pollTimer: 0,
  statusTimer: 0,
  deviceTimer: 0,
  deviceGen: 0,
  deviceFlow: null,
  editingClient: false,
};

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
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  if (h > 0) return h + ":" + String(m).padStart(2, "0") + ":" + String(sec).padStart(2, "0");
  return m + ":" + String(sec).padStart(2, "0");
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
function folderLabel(p) {
  if (!p) return "PS5 Captures";
  if (p === "/user/av_contents/video" || p === "/user/av_contents" || p === "/user/av_content/video") return "PS5 Captures";
  if (p.startsWith("/mnt/usb")) return "USB " + p.slice(8);
  if (p.startsWith("/mnt/ext")) return "Ext " + p.slice(8);
  const parts = p.split("/").filter(Boolean);
  return parts.length ? parts[parts.length - 1] : p;
}

async function copyText(txt, okMsg) {
  try {
    await navigator.clipboard.writeText(txt);
    toast(okMsg || "Copied");
  } catch (e) {
    toast(txt);
  }
}

function stopStatusWatch() {
  clearTimeout(state.statusTimer);
  state.statusTimer = 0;
}

function stopDeviceFlow() {
  state.deviceGen++;
  clearTimeout(state.deviceTimer);
  state.deviceTimer = 0;
  state.deviceFlow = null;
}

// ---------- throttled 1-at-a-time 16:9 thumbnail generator ----------
const thumbCache = new Map();
const thumbQueue = [];
let thumbBusy = false;
let thumbObserver = null;

function enqueueThumb(path) {
  if (thumbCache.has(path) || thumbQueue.includes(path)) return;
  thumbQueue.push(path);
  pumpThumbs();
}

function applyThumbToDom(path) {
  const info = thumbCache.get(path);
  if (!info) return;
  for (const card of document.querySelectorAll("#clips .clip-card")) {
    if (card.dataset.path !== path) continue;
    const img = card.querySelector(".thumb-img");
    const ph = card.querySelector(".thumb-ph");
    const meta = card.querySelector(".thumb-meta");
    if (info.url && img) {
      img.src = info.url;
      img.classList.remove("hidden");
      if (ph) ph.classList.add("hidden");
    }
    if (info.dur > 0 && meta) {
      meta.textContent = duration(info.dur) + " · " + size(Number(card.dataset.size || 0));
    }
  }
}

function grabThumb(path) {
  return new Promise((resolve) => {
    const v = document.createElement("video");
    v.muted = true;
    v.playsInline = true;
    v.preload = "metadata";
    let done = false;
    let dur = 0;
    const finish = (url) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      v.onloadedmetadata = v.onseeked = v.onerror = null;
      v.pause();
      v.removeAttribute("src");
      try { v.load(); } catch (e) { /* ignore */ }
      thumbCache.set(path, { url, dur });
      applyThumbToDom(path);
      resolve();
    };
    const timer = setTimeout(() => finish(""), 6000);
    v.onloadedmetadata = () => {
      dur = isFinite(v.duration) && v.duration > 0 ? v.duration : 0;
      const target = dur > 1 ? Math.min(2, dur * 0.15) : 0.1;
      try { v.currentTime = target; } catch (e) { finish(""); }
    };
    v.onseeked = () => {
      let url = "";
      try {
        const w = 320, h = 180;
        const c = document.createElement("canvas");
        c.width = w; c.height = h;
        const ctx = c.getContext("2d");
        if (ctx && v.videoWidth && v.videoHeight) {
          ctx.drawImage(v, 0, 0, w, h);
          url = c.toDataURL("image/jpeg", 0.72);
        }
      } catch (e) { /* ignore canvas errors */ }
      finish(url);
    };
    v.onerror = () => finish("");
    v.src = "/api/stream?path=" + encodeURIComponent(path);
  });
}

async function pumpThumbs() {
  if (thumbBusy) return;
  thumbBusy = true;
  while (thumbQueue.length) {
    const p = thumbQueue.shift();
    if (!thumbCache.has(p)) await grabThumb(p);
  }
  thumbBusy = false;
}

// ---------- status & OAuth ----------
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
  $("#setup-qr").innerHTML = QR.svg(url, 168);
  $("#setup-qr-url").textContent = url;
  $("#oauth-file-path").textContent = s.oauth_file || "oauth.txt";

  if (!s.logged_in) {
    $("#login").classList.remove("hidden");
    $("#app").classList.add("hidden");
    $("#tabs").classList.add("hidden");
    clearTimeout(state.pollTimer);
    $("#login-error").textContent = s.auth_error || "";

    if (!s.has_client || state.editingClient) {
      stopDeviceFlow();
      stopStatusWatch();
      $("#oauth-setup").classList.remove("hidden");
      $("#oauth-signin").classList.add("hidden");
      $("#cancel-edit-client").classList.toggle("hidden", !s.has_client);
      state.statusTimer = setTimeout(loadStatus, 3000);
    } else {
      stopStatusWatch();
      $("#oauth-setup").classList.add("hidden");
      $("#oauth-signin").classList.remove("hidden");
      $("#client-id-hint").textContent = s.client_id_hint || "";
      if (!state.deviceFlow) startDeviceFlow(false);
    }
    return;
  }

  stopDeviceFlow();
  stopStatusWatch();
  state.editingClient = false;
  $("#login").classList.add("hidden");
  $("#app").classList.remove("hidden");
  $("#tabs").classList.remove("hidden");
  const ch = s.channel || {};
  $("#account").textContent = "Signed in as " + (ch.name || "YouTube") + (ch.id ? " (" + ch.id + ")" : "") + " · " + s.mode + " " + s.version;
  $("#set-client-hint").textContent = s.client_id_hint || "";
  if (!$("#folder").value) $("#folder").value = s.captures_dir;
  $("#browse").textContent = "Folder: " + folderLabel($("#folder").value || s.captures_dir);
  applySettingsForm(s.settings);
  if (state.playlists === null) loadPlaylists(false);
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

async function startDeviceFlow(force) {
  stopDeviceFlow();
  const gen = state.deviceGen;
  $("#login-error").textContent = "";
  $("#device-code").textContent = "----";
  $("#device-qr").innerHTML = "";
  $("#device-status").textContent = "Requesting Google sign-in code...";
  try {
    const r = await api("oauth/start", force ? { force: "1" } : {});
    if (gen !== state.deviceGen) return;
    if (r.already_logged_in) {
      stopDeviceFlow();
      await loadStatus();
      loadClips();
      return;
    }
    state.deviceFlow = r;
    $("#device-qr").innerHTML = QR.svg(r.verification_url, 200);
    $("#device-url").href = r.verification_url;
    $("#device-url").textContent = r.verification_url;
    $("#device-code").textContent = r.user_code;
    $("#device-status").textContent = "Waiting for approval on your phone or PC...";
    state.deviceTimer = setTimeout(() => pollDeviceFlow(gen), (r.interval || 5) * 1000);
  } catch (e) {
    if (gen !== state.deviceGen) return;
    $("#device-status").textContent = "Could not start sign-in";
    $("#login-error").textContent = e.message;
  }
}

async function pollDeviceFlow(gen) {
  if (gen !== state.deviceGen || !state.deviceFlow) return;
  clearTimeout(state.deviceTimer);
  try {
    const r = await api("oauth/poll", {});
    if (gen !== state.deviceGen) return;
    if (r.state === "done") {
      stopDeviceFlow();
      toast("Signed in to YouTube");
      await loadStatus();
      loadClips();
      return;
    }
    if (r.state === "pending" || r.state === "slow_down") {
      $("#device-status").textContent = r.state === "slow_down"
        ? "Waiting for approval on your phone or PC (backing off poll rate)..."
        : "Waiting for approval on your phone or PC...";
      state.deviceTimer = setTimeout(() => pollDeviceFlow(gen), (r.interval || 5) * 1000);
      return;
    }
    stopDeviceFlow();
    const msg = r.error || ("Sign-in " + (r.state || "failed"));
    $("#device-status").textContent = msg;
    $("#login-error").textContent = msg;
  } catch (e) {
    if (gen !== state.deviceGen) return;
    $("#device-status").textContent = "Waiting for console (" + e.message + ")...";
    state.deviceTimer = setTimeout(() => pollDeviceFlow(gen), 5000);
  }
}

function openQrModal(url, title, subtitle) {
  $("#qr-modal-title").textContent = title || "Open on phone or PC";
  $("#qr-modal-sub").textContent = subtitle || "Scan with your phone camera on the same Wi-Fi network:";
  $("#modal-qr").innerHTML = QR.svg(url, 220);
  $("#modal-qr-url").value = url;
  $("#qr-modal").classList.remove("hidden");
}

$("#btn-qr").addEventListener("click", () => openQrModal(consoleUrl(), "Open on phone or PC"));
$("#qr-close").addEventListener("click", () => $("#qr-modal").classList.add("hidden"));
$("#qr-modal").addEventListener("click", (ev) => { if (ev.target.id === "qr-modal") $("#qr-modal").classList.add("hidden"); });
$("#modal-qr-copy").addEventListener("click", () => copyText($("#modal-qr-url").value, "URL copied"));

$("#client-json-file").addEventListener("change", async (ev) => {
  const f = ev.target.files && ev.target.files[0];
  if (!f) return;
  const txt = await f.text();
  ev.target.value = "";
  $("#login-error").textContent = "";
  try {
    state.status = await api("oauth/config", { client_json: txt });
    state.editingClient = false;
    toast("Saved Client ID from " + f.name);
    renderStatus();
  } catch (e) {
    $("#login-error").textContent = e.message;
  }
});

$("#oauth-config-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const btn = ev.target.querySelector("button[type=submit]");
  $("#login-error").textContent = "";
  btn.disabled = true; btn.textContent = "Saving...";
  try {
    state.status = await api("oauth/config", {
      client_id: $("#client-id").value.trim(),
      client_secret: $("#client-secret").value.trim(),
    });
    $("#client-id").value = "";
    $("#client-secret").value = "";
    state.editingClient = false;
    renderStatus();
  } catch (e) {
    $("#login-error").textContent = e.message;
  } finally {
    btn.disabled = false; btn.textContent = "Save Client ID";
  }
});

$("#change-client").addEventListener("click", () => {
  state.editingClient = true;
  renderStatus();
});
$("#cancel-edit-client").addEventListener("click", () => {
  state.editingClient = false;
  renderStatus();
});
$("#new-device-code").addEventListener("click", () => startDeviceFlow(true));
$("#check-device-now").addEventListener("click", async () => {
  const btn = $("#check-device-now");
  btn.disabled = true; btn.textContent = "Checking...";
  try {
    if (state.deviceFlow) await pollDeviceFlow(state.deviceGen);
    else await loadStatus();
  } finally {
    btn.disabled = false; btn.textContent = "Check now";
  }
});
$("#copy-device-code").addEventListener("click", () => copyText($("#device-code").textContent, "Code copied"));

function syncAuthOnResume() {
  if (!state.status || !state.status.logged_in) {
    if (state.deviceFlow) pollDeviceFlow(state.deviceGen);
    else loadStatus();
  }
}
document.addEventListener("visibilitychange", () => { if (!document.hidden) syncAuthOnResume(); });
window.addEventListener("focus", syncAuthOnResume);

async function clearClientConfig() {
  if (!confirm("Reset and delete your stored Google Cloud Client ID and tokens from the console?")) return;
  stopDeviceFlow();
  try {
    state.status = await api("oauth/config/clear", {});
    state.clips = [];
    state.playlists = null;
    state.editingClient = false;
    toast("Client ID removed");
    renderStatus();
  } catch (e) {
    toast(e.message);
  }
}
$("#clear-client-login").addEventListener("click", clearClientConfig);
$("#reset-client").addEventListener("click", clearClientConfig);

$("#logout").addEventListener("click", async () => {
  if (!confirm("Sign out of YouTube? Your Client ID stays saved on the console for quick QR sign-in.")) return;
  await api("logout", {}).catch((e) => toast(e.message));
  state.clips = [];
  state.playlists = null;
  loadStatus();
});

$("#quit").addEventListener("click", async () => {
  const active = state.jobs.filter((j) => ["queued", "starting", "uploading", "finalizing"].includes(j.state)).length;
  if (!confirm("Stop the server on the console?" + (active ? " " + active + " active upload(s) will pause and resume next time." : ""))) return;
  await api("quit", {}).catch(() => {});
  toast("Server stopped");
  setConn(false);
});

// ---------- tabs & clips gallery ----------
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

function clipGameLabel(c) {
  return c.game_name || c.title_id || "Other captures";
}

function updateGameFilter() {
  const counts = new Map();
  for (const c of state.clips) {
    const label = clipGameLabel(c);
    counts.set(label, (counts.get(label) || 0) + 1);
  }
  const sel = $("#game-filter");
  const cur = state.gameFilter;
  const opts = [el("option", { value: "all" }, "All games (" + state.clips.length + ")")];
  for (const [label, cnt] of Array.from(counts.entries()).sort((a, b) => a[0].localeCompare(b[0]))) {
    opts.push(el("option", { value: label }, label + " (" + cnt + ")"));
  }
  sel.replaceChildren(...opts);
  sel.value = counts.has(cur) ? cur : "all";
  state.gameFilter = sel.value;
}

async function loadClips() {
  const dir = $("#folder").value.trim();
  $("#browse").textContent = "Folder: " + folderLabel(dir || (state.status && state.status.captures_dir));
  $("#clips-status").textContent = "Scanning captures...";
  try {
    const r = await api("clips" + (dir ? "?dir=" + encodeURIComponent(dir) : ""));
    state.clips = Array.isArray(r.clips) ? r.clips : [];
    updateGameFilter();
    renderClips();
  } catch (e) {
    $("#clips-status").textContent = "Could not load clips: " + e.message;
    $("#clips").replaceChildren();
  }
}

$("#refresh").addEventListener("click", loadClips);
$("#filter").addEventListener("input", renderClips);
$("#game-filter").addEventListener("change", () => {
  state.gameFilter = $("#game-filter").value;
  renderClips();
});
document.querySelectorAll("#status-filter button").forEach((b) => {
  b.addEventListener("click", () => {
    state.statusFilter = b.dataset.status;
    document.querySelectorAll("#status-filter button").forEach((x) => x.classList.toggle("active", x === b));
    renderClips();
  });
});

function renderClips() {
  const q = $("#filter").value.trim().toLowerCase();
  const shown = state.clips.filter((c) => {
    if (state.gameFilter !== "all" && clipGameLabel(c) !== state.gameFilter) return false;
    if (state.statusFilter === "pending" && c.video_id) return false;
    if (state.statusFilter === "uploaded" && !c.video_id) return false;
    if (q) {
      const hay = (c.name + " " + c.title_id + " " + (c.game_name || "")).toLowerCase();
      if (!hay.includes(q)) return false;
    }
    return true;
  });

  $("#clips-status").textContent = state.clips.length
    ? (shown.length === state.clips.length ? state.clips.length + " clip(s)" : shown.length + " of " + state.clips.length + " clips")
    : "No .mp4 or .webm clips found in this folder.";

  if (thumbObserver) {
    thumbObserver.disconnect();
  } else if ("IntersectionObserver" in window) {
    thumbObserver = new IntersectionObserver((entries) => {
      for (const entry of entries) {
        if (entry.isIntersecting) {
          thumbObserver.unobserve(entry.target);
          enqueueThumb(entry.target.dataset.path);
        }
      }
    }, { rootMargin: "240px" });
  }

  const cards = shown.map((c) => {
    const cached = thumbCache.get(c.path);
    const label = c.game_name || c.title_id || "";
    const durText = (cached && cached.dur > 0) ? duration(cached.dur) + " · " + size(c.size) : size(c.size);
    const card = el("div", { class: "clip-card", role: "button", tabindex: "0", "data-path": c.path, "data-size": String(c.size || 0) },
      el("div", { class: "thumb" },
        el("img", { class: cached && cached.url ? "thumb-img" : "thumb-img hidden", src: (cached && cached.url) || "", alt: "" }),
        el("div", { class: cached && cached.url ? "thumb-ph hidden" : "thumb-ph" }, label || "VIDEO"),
        label ? el("span", { class: "thumb-game" }, label) : null,
        c.video_id ? el("span", { class: "thumb-done" }, "Uploaded") : null,
        el("span", { class: "thumb-meta" }, durText)),
      el("div", { class: "clip-body" },
        el("div", { class: "clip-title", title: c.name }, c.name),
        el("div", { class: "clip-sub" },
          el("span", {}, c.mtime ? fmtDate(c.mtime) : ""),
          c.video_id
            ? el("a", { class: "yt-link", href: "https://youtu.be/" + c.video_id, target: "_blank", rel: "noopener" }, c.video_id)
            : el("span", {}, c.title_id || ""))));
    card.addEventListener("click", (ev) => {
      if (ev.target.closest("a")) return;
      openUpload(c);
    });
    card.addEventListener("keydown", (ev) => {
      if (ev.key === "Enter" || ev.key === " ") {
        ev.preventDefault();
        openUpload(c);
      }
    });
    return card;
  });

  $("#clips").replaceChildren(...cards);
  for (const card of cards) {
    if (thumbCache.has(card.dataset.path)) continue;
    if (thumbObserver) thumbObserver.observe(card);
    else enqueueThumb(card.dataset.path);
  }
}

// ---------- YouTube playlists ----------
function populatePlaylistSelect(selectedId) {
  const sel = $("#up-playlist-select");
  const custom = $("#up-playlist-custom");
  const list = Array.isArray(state.playlists) ? state.playlists : [];
  const opts = [el("option", { value: "" }, "None")];
  let matched = !selectedId;
  for (const pl of list) {
    if (pl.id === selectedId) matched = true;
    opts.push(el("option", { value: pl.id }, pl.title + " (" + pl.count + ")"));
  }
  opts.push(el("option", { value: "__custom__" }, "Enter custom playlist ID..."));
  sel.replaceChildren(...opts);

  if (!selectedId) {
    sel.value = "";
    custom.value = "";
    custom.classList.add("hidden");
  } else if (matched) {
    sel.value = selectedId;
    custom.value = "";
    custom.classList.add("hidden");
  } else {
    sel.value = "__custom__";
    custom.value = selectedId;
    custom.classList.remove("hidden");
  }
}

async function loadPlaylists(force) {
  const stEl = $("#up-playlist-status");
  if (stEl) {
    stEl.textContent = "Loading your playlists...";
    stEl.classList.remove("hidden");
  }
  try {
    const r = await api("playlists" + (force ? "?refresh=1" : ""));
    state.playlists = Array.isArray(r.playlists) ? r.playlists : [];
    const curVal = currentPlaylistValue();
    const defVal = curVal || ((state.status && state.status.settings && state.status.settings.last_playlist_id) || "");
    populatePlaylistSelect(defVal);
    if (stEl) {
      if (state.playlists.length) stEl.classList.add("hidden");
      else { stEl.textContent = "No playlists found on this channel."; stEl.classList.remove("hidden"); }
    }
  } catch (e) {
    state.playlists = state.playlists || [];
    if (stEl) {
      stEl.textContent = "Could not load playlists: " + e.message;
      stEl.classList.remove("hidden");
    }
  }
}

function currentPlaylistValue() {
  if ($("#up-mfk").value === "1") return "";
  const sel = $("#up-playlist-select").value;
  if (sel === "__custom__") return $("#up-playlist-custom").value.trim();
  return sel;
}

$("#up-playlist-select").addEventListener("change", () => {
  const isCustom = $("#up-playlist-select").value === "__custom__";
  $("#up-playlist-custom").classList.toggle("hidden", !isCustom);
  if (isCustom) $("#up-playlist-custom").focus();
});
$("#refresh-playlists").addEventListener("click", () => loadPlaylists(true));

// ---------- unified viewer + upload modal ----------
function openUpload(c) {
  const st = (state.status && state.status.settings) || {};
  $("#up-path").value = c.path;
  const gname = c.game_name || c.title_id || "";
  $("#up-clip-info").textContent = (gname ? gname + " · " : "") + size(c.size) + (c.mtime ? " · " + fmtDate(c.mtime) : "");

  const ytLink = $("#up-prev-yt");
  if (c.video_id) {
    ytLink.href = "https://youtu.be/" + c.video_id;
    ytLink.textContent = "Uploaded (" + c.video_id + ")";
    ytLink.classList.remove("hidden");
  } else {
    ytLink.classList.add("hidden");
  }

  const v = $("#up-video");
  v.src = "/api/stream?path=" + encodeURIComponent(c.path);

  const baseTitle = c.name.replace(/\.[^.]+$/, "");
  const datePart = c.mtime ? fmtDate(c.mtime).slice(0, 10) : baseTitle;
  $("#up-title").value = c.game_name ? c.game_name + " — " + datePart : (c.title_id ? c.title_id + " - " + baseTitle : baseTitle);
  $("#up-desc").value = "Recorded on PlayStation 5" + (gname ? " — " + gname : "");
  $("#up-privacy").value = st.default_privacy || "UNLISTED";
  $("#up-mfk").value = st.made_for_kids ? "1" : "0";
  $("#up-error").textContent = "";

  populatePlaylistSelect(st.made_for_kids ? "" : (st.last_playlist_id || ""));
  if (state.playlists === null) loadPlaylists(false);
  syncMfkUi();

  $("#upload-modal").classList.remove("hidden");
  $("#up-title").focus();
}

function closeUploadModal() {
  const v = $("#up-video");
  v.pause();
  v.removeAttribute("src");
  try { v.load(); } catch (e) { /* ignore */ }
  $("#upload-modal").classList.add("hidden");
}

function syncMfkUi() {
  const mfk = $("#up-mfk").value === "1";
  $("#up-playlist-select").disabled = mfk;
  $("#up-playlist-custom").disabled = mfk;
  $("#refresh-playlists").disabled = mfk;
  if (mfk) {
    $("#up-playlist-select").value = "";
    $("#up-playlist-custom").value = "";
    $("#up-playlist-custom").classList.add("hidden");
  }
  $("#up-mfk-note").classList.toggle("hidden", !mfk);
}
$("#up-mfk").addEventListener("change", syncMfkUi);
$("#up-close").addEventListener("click", closeUploadModal);
$("#up-cancel").addEventListener("click", closeUploadModal);
$("#upload-modal").addEventListener("click", (ev) => { if (ev.target.id === "upload-modal") closeUploadModal(); });

$("#upload-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const btn = $("#up-submit");
  $("#up-error").textContent = "";
  btn.disabled = true; btn.textContent = "Queuing...";
  try {
    const r = await api("upload", {
      path: $("#up-path").value,
      title: $("#up-title").value.trim(),
      description: $("#up-desc").value,
      privacy: $("#up-privacy").value,
      made_for_kids: $("#up-mfk").value,
      playlist_id: currentPlaylistValue(),
    });
    if (r.settings && state.status && state.status.settings) {
      Object.assign(state.status.settings, r.settings);
      applySettingsForm(state.status.settings);
    }
    closeUploadModal();
    toast("Queued upload");
    pollJobs();
    showTab("uploads");
  } catch (e) {
    $("#up-error").textContent = e.message;
  } finally {
    btn.disabled = false; btn.textContent = "Upload to YouTube";
  }
});

// ---------- uploads queue & active strip ----------
function updateBadgeAndStrip() {
  const activeJobs = state.jobs.filter((j) => ["queued", "starting", "uploading", "finalizing"].includes(j.state));
  const b = $("#badge");
  b.textContent = activeJobs.length;
  b.classList.toggle("hidden", !activeJobs.length);

  const strip = $("#active-strip");
  if (!activeJobs.length) {
    strip.classList.add("hidden");
  } else {
    const cur = activeJobs.find((j) => j.state === "uploading") || activeJobs[0];
    const pct = cur.total > 0 ? Math.min(100, (cur.done / cur.total) * 100) : 0;
    const speedTxt = cur.speed > 0 ? " · " + size(cur.speed) + "/s · " + duration((cur.total - cur.done) / cur.speed) + " left" : "";
    $("#active-strip-text").textContent =
      (activeJobs.length > 1 ? activeJobs.length + " uploads active · " : "Uploading ") +
      (cur.title || cur.name) + " (" + pct.toFixed(1) + "%" + speedTxt + ")";
    $("#active-strip-pct").style.width = pct.toFixed(1) + "%";
    strip.classList.remove("hidden");
  }
  return activeJobs.length;
}
$("#active-strip-open").addEventListener("click", () => showTab("uploads"));

async function pollJobs() {
  clearTimeout(state.pollTimer);
  if (!state.status || !state.status.logged_in) return;
  try {
    const r = await api("jobs");
    state.jobs = r.jobs || [];
  } catch (e) { /* retry below */ }
  const active = updateBadgeAndStrip();
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
        j.url ? el("button", { class: "small", type: "button", onclick: () => copyText(j.url, "YouTube link copied") }, "Copy link") : null,
        j.url ? el("button", { class: "small", type: "button", onclick: () => openQrModal(j.url, "Share uploaded video", "Scan to open this clip on your phone:") }, "Share QR") : null,
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
    $("#browse").textContent = "Folder: " + folderLabel(state.status.captures_dir);
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
  if (r && r.roots && r.roots.length) {
    for (let i = 0; i < r.roots.length; i++) {
      if (await browse(r.roots[i], i + 1 < r.roots.length)) return;
    }
  }
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
  $("#picker-roots").replaceChildren(...r.roots.map((root) => el("button", { type: "button", onclick: () => browse(root) }, folderLabel(root))));
  const items = r.dirs.map((d) => el("li", { onclick: () => browse(r.path.replace(/\/$/, "") + "/" + d) }, d + "/"));
  const files = (r.files || []).map((f) => el("li", { class: "file" }, el("span", {}, f.name), el("span", {}, size(f.size))));
  $("#picker-dirs").replaceChildren(...(items.length || files.length ? items.concat(files) : [el("li", { class: "empty" }, "Empty folder")]));
  return true;
}

loadStatus();
