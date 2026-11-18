/**
 * PlayStation 5 YouTube Uploader (ps5-yt-uploader v1.0.0)
 * Embedded Dashboard Application
 * 100% Self-Contained Vanilla JS (Zero External Dependencies)
 */

(function () {
  'use strict';

  const APP_NAME = 'ps5-yt-uploader';
  const APP_VERSION = '1.0.0';

  // --- Embedded QR Code Generator (ISO/IEC 18004 Minimal Implementation) ---
  const QRMath = {
    glog: function (n) {
      if (n < 1) throw new Error("glog(" + n + ")");
      return QRMath.LOG_TABLE[n];
    },
    gexp: function (n) {
      while (n < 0) n += 255;
      while (n >= 255) n -= 255;
      return QRMath.EXP_TABLE[n];
    },
    EXP_TABLE: new Array(256),
    LOG_TABLE: new Array(256)
  };
  for (let i = 0; i < 8; i++) QRMath.EXP_TABLE[i] = 1 << i;
  for (let i = 8; i < 256; i++) {
    QRMath.EXP_TABLE[i] = QRMath.EXP_TABLE[i - 4] ^ QRMath.EXP_TABLE[i - 5] ^ QRMath.EXP_TABLE[i - 6] ^ QRMath.EXP_TABLE[i - 8];
  }
  for (let i = 0; i < 255; i++) QRMath.LOG_TABLE[QRMath.EXP_TABLE[i]] = i;

  function QRPolynomial(num, shift) {
    let offset = 0;
    while (offset < num.length && num[offset] === 0) offset++;
    this.num = new Array(num.length - offset + shift);
    for (let i = 0; i < num.length - offset; i++) this.num[i] = num[i + offset];
  }
  QRPolynomial.prototype = {
    get: function (i) { return this.num[i]; },
    getLength: function () { return this.num.length; },
    multiply: function (e) {
      let num = new Array(this.getLength() + e.getLength() - 1);
      for (let i = 0; i < num.length; i++) num[i] = 0;
      for (let i = 0; i < this.getLength(); i++) {
        for (let j = 0; j < e.getLength(); j++) {
          num[i + j] ^= QRMath.gexp(QRMath.glog(this.get(i)) + QRMath.glog(e.get(j)));
        }
      }
      return new QRPolynomial(num, 0);
    },
    mod: function (e) {
      if (this.getLength() - e.getLength() < 0) return this;
      let ratio = QRMath.glog(this.get(0)) - QRMath.glog(e.get(0));
      let num = new Array(this.getLength());
      for (let i = 0; i < this.getLength(); i++) num[i] = this.get(i);
      for (let i = 0; i < e.getLength(); i++) {
        num[i] ^= QRMath.gexp(QRMath.glog(e.get(i)) + ratio);
      }
      return new QRPolynomial(num, 0).mod(e);
    }
  };

  const QRRSBlock = {
    RS_BLOCK_TABLE: [
      [1, 26, 19], [1, 26, 16], [1, 26, 13], [1, 26, 9],
      [1, 44, 34], [1, 44, 28], [1, 44, 22], [1, 44, 16],
      [1, 70, 55], [1, 70, 44], [2, 35, 17], [2, 35, 13],
      [1, 100, 80], [2, 50, 32], [2, 50, 24], [4, 25, 9],
      [1, 134, 108], [2, 67, 43], [2, 33, 15, 2, 34, 16], [2, 33, 11, 2, 34, 12],
      [2, 86, 68], [4, 43, 27], [4, 43, 19], [4, 43, 15],
      [2, 98, 78], [4, 49, 31], [2, 32, 14, 4, 33, 15], [4, 39, 13, 1, 40, 14],
      [2, 121, 97], [2, 60, 38, 2, 61, 39], [4, 40, 18, 2, 41, 19], [4, 40, 14, 2, 41, 15],
      [2, 146, 116], [3, 58, 36, 2, 59, 37], [4, 36, 16, 4, 37, 17], [4, 36, 12, 4, 37, 13],
      [2, 86, 68, 2, 87, 69], [4, 69, 43, 1, 70, 44], [6, 43, 19, 2, 44, 20], [6, 43, 15, 2, 44, 16]
    ],
    getRSBlocks: function (typeNumber, errorCorrectLevel) {
      let rsBlock = QRRSBlock.RS_BLOCK_TABLE[(typeNumber - 1) * 4 + errorCorrectLevel];
      if (!rsBlock) throw new Error("bad rs block: " + typeNumber + "/" + errorCorrectLevel);
      let length = rsBlock.length / 3;
      let list = [];
      for (let i = 0; i < length; i++) {
        let count = rsBlock[i * 3 + 0];
        let totalCount = rsBlock[i * 3 + 1];
        let dataCount = rsBlock[i * 3 + 2];
        for (let j = 0; j < count; j++) list.push({ totalCount: totalCount, dataCount: dataCount });
      }
      return list;
    }
  };

  const QRUtil = {
    PATTERN_POSITION_TABLE: [
      [], [6, 18], [6, 22], [6, 26], [6, 30], [6, 34],
      [6, 22, 38], [6, 24, 42], [6, 26, 46], [6, 28, 50], [6, 30, 54]
    ],
    G15: (1 << 10) | (1 << 8) | (1 << 5) | (1 << 4) | (1 << 2) | (1 << 1) | (1 << 0),
    G15_MASK: (1 << 14) | (1 << 12) | (1 << 10) | (1 << 4) | (1 << 1),
    getBCHTypeInfo: function (data) {
      let d = data << 10;
      while (QRUtil.getBCHDigit(d) - QRUtil.getBCHDigit(QRUtil.G15) >= 0) {
        d ^= (QRUtil.G15 << (QRUtil.getBCHDigit(d) - QRUtil.getBCHDigit(QRUtil.G15)));
      }
      return ((data << 10) | d) ^ QRUtil.G15_MASK;
    },
    getBCHDigit: function (data) {
      let digit = 0;
      while (data !== 0) { digit++; data >>>= 1; }
      return digit;
    },
    getPatternPosition: function (typeNumber) {
      return QRUtil.PATTERN_POSITION_TABLE[typeNumber - 1] || [];
    },
    getMask: function (maskPattern, i, j) {
      switch (maskPattern) {
        case 0: return (i + j) % 2 === 0;
        case 1: return i % 2 === 0;
        case 2: return j % 3 === 0;
        case 3: return (i + j) % 3 === 0;
        case 4: return (Math.floor(i / 2) + Math.floor(j / 3)) % 2 === 0;
        case 5: return ((i * j) % 2) + ((i * j) % 3) === 0;
        case 6: return (((i * j) % 2) + ((i * j) % 3)) % 2 === 0;
        case 7: return (((i * j) % 3) + ((i + j) % 2)) % 2 === 0;
        default: throw new Error("bad maskPattern: " + maskPattern);
      }
    },
    getErrorCorrectPolynomial: function (errorCorrectLength) {
      let a = new QRPolynomial([1], 0);
      for (let i = 0; i < errorCorrectLength; i++) {
        a = a.multiply(new QRPolynomial([1, QRMath.gexp(i)], 0));
      }
      return a;
    },
    getLengthInBits: function (mode, type) {
      if (1 <= type && type < 10) return (mode === 1) ? 10 : (mode === 2) ? 9 : 8;
      return (mode === 1) ? 12 : (mode === 2) ? 11 : 16;
    },
    getLostPoint: function (qrCode) {
      let moduleCount = qrCode.getModuleCount();
      let lostPoint = 0;
      for (let row = 0; row < moduleCount; row++) {
        for (let col = 0; col < moduleCount; col++) {
          let sameCount = 0;
          let dark = qrCode.isDark(row, col);
          for (let r = -1; r <= 1; r++) {
            if (row + r < 0 || moduleCount <= row + r) continue;
            for (let c = -1; c <= 1; c++) {
              if (col + c < 0 || moduleCount <= col + c) continue;
              if (r === 0 && c === 0) continue;
              if (dark === qrCode.isDark(row + r, col + c)) sameCount++;
            }
          }
          if (sameCount > 5) lostPoint += (3 + sameCount - 5);
        }
      }
      return lostPoint;
    }
  };

  function QRBitBuffer() {
    this.buffer = [];
    this.length = 0;
  }
  QRBitBuffer.prototype = {
    put: function (num, length) {
      for (let i = 0; i < length; i++) {
        let bit = ((num >>> (length - i - 1)) & 1) === 1;
        let bufIndex = Math.floor(this.length / 8);
        if (this.buffer.length <= bufIndex) this.buffer.push(0);
        if (bit) this.buffer[bufIndex] |= (0x80 >>> (this.length % 8));
        this.length++;
      }
    },
    putBit: function (bit) {
      let bufIndex = Math.floor(this.length / 8);
      if (this.buffer.length <= bufIndex) this.buffer.push(0);
      if (bit) this.buffer[bufIndex] |= (0x80 >>> (this.length % 8));
      this.length++;
    }
  };

  function QRCodeModel(typeNumber, errorCorrectLevel) {
    this.typeNumber = typeNumber;
    this.errorCorrectLevel = errorCorrectLevel;
    this.modules = null;
    this.moduleCount = 0;
    this.dataList = [];
  }
  QRCodeModel.prototype = {
    addData: function (data) {
      this.dataList.push(data);
    },
    isDark: function (row, col) {
      return this.modules[row][col];
    },
    getModuleCount: function () { return this.moduleCount; },
    make: function () {
      if (this.typeNumber < 1) {
        let typeNumber = 1;
        for (typeNumber = 1; typeNumber < 40; typeNumber++) {
          let rsBlocks = QRRSBlock.getRSBlocks(typeNumber, this.errorCorrectLevel);
          let totalDataCount = 0;
          for (let i = 0; i < rsBlocks.length; i++) totalDataCount += rsBlocks[i].dataCount;
          let buffer = new QRBitBuffer();
          for (let i = 0; i < this.dataList.length; i++) {
            let d = this.dataList[i];
            buffer.put(4, 4); // 8bit byte mode
            buffer.put(d.length, QRUtil.getLengthInBits(4, typeNumber));
            for (let j = 0; j < d.length; j++) buffer.put(d.charCodeAt(j), 8);
          }
          if (buffer.length <= totalDataCount * 8) break;
        }
        this.typeNumber = typeNumber;
      }
      this.makeImpl(false, this.getBestMaskPattern());
    },
    makeImpl: function (test, maskPattern) {
      this.moduleCount = this.typeNumber * 4 + 17;
      this.modules = new Array(this.moduleCount);
      for (let r = 0; r < this.moduleCount; r++) {
        this.modules[r] = new Array(this.moduleCount);
        for (let c = 0; c < this.moduleCount; c++) this.modules[r][c] = null;
      }
      this.setupFinder(0, 0);
      this.setupFinder(this.moduleCount - 7, 0);
      this.setupFinder(0, this.moduleCount - 7);
      this.setupAlignment();
      this.setupTiming();
      this.setupTypeInfo(test, maskPattern);
      let data = QRCodeModel.createData(this.typeNumber, this.errorCorrectLevel, this.dataList);
      this.mapData(data, maskPattern);
    },
    setupFinder: function (row, col) {
      for (let r = -1; r <= 7; r++) {
        if (row + r <= -1 || this.moduleCount <= row + r) continue;
        for (let c = -1; c <= 7; c++) {
          if (col + c <= -1 || this.moduleCount <= col + c) continue;
          let dark = (0 <= r && r <= 6 && (c === 0 || c === 6)) ||
                     (0 <= c && c <= 6 && (r === 0 || r === 6)) ||
                     (2 <= r && r <= 4 && 2 <= c && c <= 4);
          this.modules[row + r][col + c] = dark;
        }
      }
    },
    setupTiming: function () {
      for (let r = 8; r < this.moduleCount - 8; r++) {
        if (this.modules[r][6] === null) this.modules[r][6] = (r % 2 === 0);
      }
      for (let c = 8; c < this.moduleCount - 8; c++) {
        if (this.modules[6][c] === null) this.modules[6][c] = (c % 2 === 0);
      }
    },
    setupAlignment: function () {
      let pos = QRUtil.getPatternPosition(this.typeNumber);
      for (let i = 0; i < pos.length; i++) {
        for (let j = 0; j < pos.length; j++) {
          let row = pos[i], col = pos[j];
          if (this.modules[row][col] !== null) continue;
          for (let r = -2; r <= 2; r++) {
            for (let c = -2; c <= 2; c++) {
              let dark = (r === -2 || r === 2 || c === -2 || c === 2 || (r === 0 && c === 0));
              this.modules[row + r][col + c] = dark;
            }
          }
        }
      }
    },
    setupTypeInfo: function (test, maskPattern) {
      let data = (this.errorCorrectLevel << 3) | maskPattern;
      let bits = QRUtil.getBCHTypeInfo(data);
      for (let i = 0; i < 15; i++) {
        let mod = (!test && ((bits >> i) & 1) === 1);
        if (i < 6) this.modules[i][8] = mod;
        else if (i < 8) this.modules[i + 1][8] = mod;
        else this.modules[this.moduleCount - 15 + i][8] = mod;
      }
      for (let i = 0; i < 15; i++) {
        let mod = (!test && ((bits >> i) & 1) === 1);
        if (i < 8) this.modules[8][this.moduleCount - i - 1] = mod;
        else if (i < 9) this.modules[8][15 - i] = mod;
        else this.modules[8][15 - i - 1] = mod;
      }
      this.modules[this.moduleCount - 8][8] = !test;
    },
    getBestMaskPattern: function () {
      let minLost = 0, pattern = 0;
      for (let i = 0; i < 8; i++) {
        this.makeImpl(true, i);
        let lost = QRUtil.getLostPoint(this);
        if (i === 0 || minLost > lost) {
          minLost = lost;
          pattern = i;
        }
      }
      return pattern;
    },
    mapData: function (data, maskPattern) {
      let inc = -1, row = this.moduleCount - 1, bitIndex = 7, byteIndex = 0;
      for (let col = this.moduleCount - 1; col > 0; col -= 2) {
        if (col === 6) col--;
        while (true) {
          for (let c = 0; c < 2; c++) {
            if (this.modules[row][col - c] === null) {
              let dark = false;
              if (byteIndex < data.length) dark = (((data[byteIndex] >>> bitIndex) & 1) === 1);
              let mask = QRUtil.getMask(maskPattern, row, col - c);
              if (mask) dark = !dark;
              this.modules[row][col - c] = dark;
              bitIndex--;
              if (bitIndex === -1) { byteIndex++; bitIndex = 7; }
            }
          }
          row += inc;
          if (row < 0 || this.moduleCount <= row) {
            row -= inc; inc = -inc; break;
          }
        }
      }
    }
  };

  QRCodeModel.createData = function (typeNumber, errorCorrectLevel, dataList) {
    let rsBlocks = QRRSBlock.getRSBlocks(typeNumber, errorCorrectLevel);
    let buffer = new QRBitBuffer();
    for (let i = 0; i < dataList.length; i++) {
      let d = dataList[i];
      buffer.put(4, 4);
      buffer.put(d.length, QRUtil.getLengthInBits(4, typeNumber));
      for (let j = 0; j < d.length; j++) buffer.put(d.charCodeAt(j), 8);
    }
    let totalDataCount = 0;
    for (let i = 0; i < rsBlocks.length; i++) totalDataCount += rsBlocks[i].dataCount;
    if (buffer.length + 4 <= totalDataCount * 8) buffer.put(0, 4);
    while (buffer.length % 8 !== 0) buffer.putBit(false);
    while (buffer.length < totalDataCount * 8) {
      buffer.put(0xec, 8);
      if (buffer.length < totalDataCount * 8) buffer.put(0x11, 8);
    }

    let offset = 0, maxDc = 0, maxEc = 0;
    let dcList = new Array(rsBlocks.length);
    let ecList = new Array(rsBlocks.length);
    for (let r = 0; r < rsBlocks.length; r++) {
      let dcCount = rsBlocks[r].dataCount;
      let ecCount = rsBlocks[r].totalCount - dcCount;
      maxDc = Math.max(maxDc, dcCount);
      maxEc = Math.max(maxEc, ecCount);
      dcList[r] = new Array(dcCount);
      for (let i = 0; i < dcCount; i++) dcList[r][i] = 0xff & buffer.buffer[i + offset];
      offset += dcCount;
      let rsPoly = QRUtil.getErrorCorrectPolynomial(ecCount);
      let rawPoly = new QRPolynomial(dcList[r], rsPoly.getLength() - 1);
      let modPoly = rawPoly.mod(rsPoly);
      ecList[r] = new Array(rsPoly.getLength() - 1);
      for (let i = 0; i < ecList[r].length; i++) {
        let modIndex = i + modPoly.getLength() - ecList[r].length;
        ecList[r][i] = (modIndex >= 0) ? modPoly.get(modIndex) : 0;
      }
    }

    let totalCodeCount = 0;
    for (let i = 0; i < rsBlocks.length; i++) totalCodeCount += rsBlocks[i].totalCount;
    let result = new Array(totalCodeCount);
    let idx = 0;
    for (let i = 0; i < maxDc; i++) {
      for (let r = 0; r < rsBlocks.length; r++) {
        if (i < dcList[r].length) result[idx++] = dcList[r][i];
      }
    }
    for (let i = 0; i < maxEc; i++) {
      for (let r = 0; r < rsBlocks.length; r++) {
        if (i < ecList[r].length) result[idx++] = ecList[r][i];
      }
    }
    return result;
  };

  function renderQRCodeSVG(text, size) {
    size = size || 220;
    let qr = new QRCodeModel(0, 1); // Auto version, EC Level L
    qr.addData(text);
    qr.make();
    let count = qr.getModuleCount();
    let margin = 2;
    let totalSize = count + margin * 2;
    let pathD = "";
    for (let r = 0; r < count; r++) {
      for (let c = 0; c < count; c++) {
        if (qr.isDark(r, c)) {
          pathD += "M" + (c + margin) + "," + (r + margin) + "h1v1h-1z ";
        }
      }
    }
    return '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ' + totalSize + ' ' + totalSize + '" ' +
           'width="' + size + '" height="' + size + '" shape-rendering="crispEdges">' +
           '<rect width="' + totalSize + '" height="' + totalSize + '" fill="#ffffff"/>' +
           '<path d="' + pathD.trim() + '" fill="#060910"/>' +
           '</svg>';
  }

  // --- App State ---
  const state = {
    authenticated: false,
    channelName: '',
    channelId: '',
    autoUpload: true,
    defaultPrivacy: 'UNLISTED',
    activeUploads: 0,
    clips: [],
    uploadQueue: [],
    currentTab: 'gallery',
    selectedClip: null,
    focusedIndex: 0
  };

  // --- Utility Helpers ---
  function formatBytes(bytes) {
    if (!bytes || bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB', 'TB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i];
  }

  function formatTimestamp(epochSec) {
    if (!epochSec) return 'Unknown Date';
    const d = new Date(epochSec * 1000);
    return d.toLocaleString(undefined, {
      year: 'numeric',
      month: 'short',
      day: 'numeric',
      hour: '2-digit',
      minute: '2-digit'
    });
  }

  function escapeHtml(str) {
    if (str === null || str === undefined) return '';
    return String(str)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  function getBaseFilename(path) {
    if (!path) return '';
    const parts = path.split('/');
    return parts[parts.length - 1] || path;
  }

  // --- REST API Client ---
  async function apiFetch(endpoint, options) {
    try {
      const resp = await fetch(endpoint, options);
      if (!resp.ok) {
        let errText = resp.statusText;
        try {
          const errJson = await resp.json();
          if (errJson && errJson.error) errText = errJson.error;
        } catch (_) {}
        throw new Error(errText);
      }
      return await resp.json();
    } catch (err) {
      console.warn('API error (' + endpoint + '):', err.message);
      throw err;
    }
  }

  async function loadStatus() {
    try {
      const data = await apiFetch('/api/status');
      state.authenticated = !!data.authenticated;
      state.channelName = data.channel_name || data.channel_title || '';
      state.channelId = data.channel_id || '';
      state.autoUpload = !!data.auto_upload;
      state.defaultPrivacy = data.default_privacy || 'UNLISTED';

      updateStatusUI();
    } catch (e) {
      console.error('Failed to load status:', e);
    }
  }

  async function loadClips() {
    const loadingEl = document.getElementById('clips-loading');
    const emptyEl = document.getElementById('clips-empty');
    const gridEl = document.getElementById('clips-grid');
    const counterEl = document.getElementById('clips-counter');

    if (loadingEl) loadingEl.style.display = 'block';
    if (emptyEl) emptyEl.style.display = 'none';

    try {
      const clips = await apiFetch('/api/clips');
      state.clips = Array.isArray(clips) ? clips : [];
      renderClipsGrid();
      if (counterEl) {
        counterEl.textContent = state.clips.length + ' clip' + (state.clips.length === 1 ? '' : 's') + ' available on PS5';
      }
    } catch (e) {
      if (counterEl) counterEl.textContent = 'Failed to load clips: ' + e.message;
      if (emptyEl) emptyEl.style.display = 'block';
    } finally {
      if (loadingEl) loadingEl.style.display = 'none';
    }
  }

  async function submitAuth(cookies, sessionToken, channelName, channelId) {
    return await apiFetch('/api/auth', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        cookies: cookies,
        session_token: sessionToken,
        channel_name: channelName,
        channel_id: channelId
      })
    });
  }

  async function submitConfig(autoUpload, defaultPrivacy) {
    return await apiFetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        auto_upload: autoUpload,
        default_privacy: defaultPrivacy
      })
    });
  }

  async function submitUpload(filepath, title, description, privacy) {
    return await apiFetch('/api/upload', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        filepath: filepath,
        title: title,
        description: description,
        privacy: privacy
      })
    });
  }

  // --- UI Update & Rendering ---
  function updateStatusUI() {
    // Header Status
    const authStatusEl = document.getElementById('auth-status-indicator');
    const authTextEl = document.getElementById('auth-status-text');
    if (authStatusEl && authTextEl) {
      if (state.authenticated) {
        authStatusEl.className = 'status-pill status-pill-connected';
        authTextEl.textContent = state.channelName ? state.channelName : 'Connected';
      } else {
        authStatusEl.className = 'status-pill status-pill-disconnected';
        authTextEl.textContent = 'Not Connected';
      }
    }

    // Pairing Info
    const infoStatus = document.getElementById('auth-info-status');
    const infoChannel = document.getElementById('auth-info-channel');
    const infoId = document.getElementById('auth-info-id');
    if (infoStatus) {
      infoStatus.textContent = state.authenticated ? 'Connected & Ready' : 'Not Authenticated';
      infoStatus.className = 'auth-val ' + (state.authenticated ? 'text-success' : 'text-warning');
    }
    if (infoChannel) infoChannel.textContent = state.channelName || (state.authenticated ? 'YouTube Channel' : 'None');
    if (infoId) infoId.textContent = state.channelId || '-';

    // Queue Metrics
    const metricChannel = document.getElementById('metric-channel-name');
    const metricChannelId = document.getElementById('metric-channel-id');
    if (metricChannel) metricChannel.textContent = state.channelName || (state.authenticated ? 'Connected' : 'Not Authenticated');
    if (metricChannelId) metricChannelId.textContent = state.channelId ? ('ID: ' + state.channelId) : 'Pair account to upload';

    // Settings Form
    const chkAuto = document.getElementById('chk-auto-upload');
    const selPrivacy = document.getElementById('sel-default-privacy');
    if (chkAuto) chkAuto.checked = !!state.autoUpload;
    if (selPrivacy && state.defaultPrivacy) selPrivacy.value = state.defaultPrivacy;
  }

  function renderClipsGrid() {
    const gridEl = document.getElementById('clips-grid');
    const emptyEl = document.getElementById('clips-empty');
    if (!gridEl) return;

    gridEl.innerHTML = '';

    if (!state.clips || state.clips.length === 0) {
      if (emptyEl) emptyEl.style.display = 'block';
      return;
    }
    if (emptyEl) emptyEl.style.display = 'none';

    state.clips.forEach(function (clip, idx) {
      const card = document.createElement('div');
      card.className = 'clip-card';

      const filename = getBaseFilename(clip.filepath);
      const titleId = clip.title_id || 'PS5-CAPTURE';
      const sizeStr = formatBytes(clip.size_bytes);
      const dateStr = formatTimestamp(clip.timestamp);

      const safeFilename = escapeHtml(filename);
      const safeTitleId = escapeHtml(titleId);
      const safeSizeStr = escapeHtml(sizeStr);
      const safeDateStr = escapeHtml(dateStr);

      card.innerHTML =
        '<div class="clip-thumbnail-area">' +
          '<div class="clip-game-icon">🎮</div>' +
          '<span class="clip-badge">' + safeTitleId + '</span>' +
          '<span class="clip-duration-tag">MP4</span>' +
        '</div>' +
        '<div class="clip-card-body">' +
          '<div class="clip-title-id" title="' + safeFilename + '">' + safeFilename + '</div>' +
          '<div class="clip-metadata-list">' +
            '<div class="clip-meta-row"><span>Game:</span><span class="clip-meta-val">' + safeTitleId + '</span></div>' +
            '<div class="clip-meta-row"><span>Recorded:</span><span class="clip-meta-val">' + safeDateStr + '</span></div>' +
            '<div class="clip-meta-row"><span>File Size:</span><span class="clip-meta-val">' + safeSizeStr + '</span></div>' +
          '</div>' +
          '<div class="clip-card-actions">' +
            '<button class="btn btn-primary btn-upload focusable" data-clip-index="' + idx + '" tabindex="0">' +
              '<span class="btn-icon">▲</span> Upload to YouTube' +
            '</button>' +
          '</div>' +
        '</div>';

      gridEl.appendChild(card);
    });

    // Wire upload button events
    const buttons = gridEl.querySelectorAll('.btn-upload');
    buttons.forEach(function (btn) {
      btn.addEventListener('click', function () {
        const index = parseInt(this.getAttribute('data-clip-index'), 10);
        openUploadModal(state.clips[index]);
      });
    });

    refreshFocusables();
  }

  function renderQueueList() {
    const listEl = document.getElementById('queue-list');
    const emptyEl = document.getElementById('queue-empty');
    const activeCountEl = document.getElementById('metric-active-count');
    const watcherStateEl = document.getElementById('metric-watcher-state');
    const badgeCountEl = document.getElementById('queue-badge-count');

    if (!listEl) return;
    listEl.innerHTML = '';

    const activeItems = state.uploadQueue.filter(function (item) {
      return item.status === 'queued' || item.status === 'uploading';
    });

    if (activeCountEl) activeCountEl.textContent = activeItems.length;
    if (watcherStateEl) watcherStateEl.textContent = activeItems.length > 0 ? 'Uploading...' : 'Idle';

    if (badgeCountEl) {
      if (activeItems.length > 0) {
        badgeCountEl.textContent = activeItems.length;
        badgeCountEl.style.display = 'inline-block';
      } else {
        badgeCountEl.style.display = 'none';
      }
    }

    if (!state.uploadQueue || state.uploadQueue.length === 0) {
      if (emptyEl) emptyEl.style.display = 'block';
      return;
    }
    if (emptyEl) emptyEl.style.display = 'none';

    state.uploadQueue.forEach(function (item) {
      const itemEl = document.createElement('div');
      itemEl.className = 'queue-item';

      let statusClass = 'status-' + item.status;
      let statusLabel = item.status.toUpperCase();
      let linkHtml = '';

      const safeTitle = escapeHtml(item.title || item.filename);
      const safeFilename = escapeHtml(item.filename);

      if (item.status === 'completed' && item.videoId) {
        const safeVid = escapeHtml(item.videoId);
        linkHtml = '<a class="youtube-link" href="https://youtu.be/' + safeVid + '" target="_blank" rel="noopener">▶ View on YouTube (youtu.be/' + safeVid + ')</a>';
      } else if (item.status === 'queued') {
        linkHtml = '<span>Waiting for worker slot...</span>';
      } else if (item.status === 'uploading') {
        statusLabel = 'Uploading in background on console...';
        linkHtml = '<span class="text-accent">Upload is streaming directly from PS5 SSD. Check on-screen notifications for completion.</span>';
      } else if (item.status === 'failed') {
        linkHtml = '<span class="text-danger">Upload failed: ' + escapeHtml(item.error || 'Server error') + '</span>';
      }

      itemEl.innerHTML =
        '<div class="queue-item-header">' +
          '<div class="queue-item-title">' + safeTitle + '</div>' +
          '<div class="queue-item-status ' + statusClass + '">' + statusLabel + '</div>' +
        '</div>' +
        '<div class="progress-bar-container">' +
          '<div class="progress-bar-fill ' + (item.status === 'uploading' ? 'indeterminate' : '') + '" style="width:' + item.progress + '%;"></div>' +
        '</div>' +
        '<div class="queue-item-meta">' +
          '<span>File: ' + safeFilename + '</span>' +
          linkHtml +
        '</div>';

      listEl.appendChild(itemEl);
    });

    refreshFocusables();
  }

  // --- Modal Logic ---
  function openUploadModal(clip) {
    if (!clip) return;
    state.selectedClip = clip;

    const modal = document.getElementById('upload-modal');
    const badge = document.getElementById('modal-clip-badge');
    const nameEl = document.getElementById('modal-clip-filename');
    const detailsEl = document.getElementById('modal-clip-details');
    const filepathInput = document.getElementById('modal-clip-filepath');
    const titleInput = document.getElementById('modal-input-title');
    const descInput = document.getElementById('modal-input-description');
    const privSelect = document.getElementById('modal-select-privacy');
    const alertBox = document.getElementById('modal-alert-box');

    if (alertBox) alertBox.style.display = 'none';

    const filename = getBaseFilename(clip.filepath);
    const titleId = clip.title_id || 'PS5-CAPTURE';
    const dateStr = formatTimestamp(clip.timestamp);
    const sizeStr = formatBytes(clip.size_bytes);

    if (badge) badge.textContent = titleId;
    if (nameEl) nameEl.textContent = filename;
    if (detailsEl) detailsEl.textContent = 'Size: ' + sizeStr + ' • Captured: ' + dateStr;
    if (filepathInput) filepathInput.value = clip.filepath;

    // Prefill title & description
    if (titleInput) {
      titleInput.value = titleId + ' Gameplay Capture - ' + dateStr;
    }
    if (descInput) {
      descInput.value = 'Recorded on PlayStation 5\nGame Title ID: ' + titleId + '\nCaptured: ' + dateStr;
    }
    if (privSelect) {
      privSelect.value = state.defaultPrivacy || 'UNLISTED';
    }

    if (modal) {
      modal.style.display = 'flex';
      refreshFocusables();
      if (titleInput) titleInput.focus();
    }
  }

  function closeUploadModal() {
    const modal = document.getElementById('upload-modal');
    if (modal) {
      modal.style.display = 'none';
      state.selectedClip = null;
      refreshFocusables();
    }
  }

  async function handleModalSubmit() {
    const filepath = document.getElementById('modal-clip-filepath').value;
    const title = document.getElementById('modal-input-title').value.trim();
    const description = document.getElementById('modal-input-description').value.trim();
    const privacy = document.getElementById('modal-select-privacy').value;
    const alertBox = document.getElementById('modal-alert-box');
    const submitBtn = document.getElementById('btn-modal-submit');

    if (!title) {
      if (alertBox) {
        alertBox.textContent = 'Please enter a video title.';
        alertBox.style.display = 'block';
      }
      return;
    }

    if (submitBtn) {
      submitBtn.disabled = true;
      submitBtn.innerHTML = '<span class="btn-icon">⏳</span> Queuing...';
    }

    try {
      const res = await submitUpload(filepath, title, description, privacy);
      closeUploadModal();

      // Track queued task locally in background uploading state
      const task = {
        id: Date.now().toString(),
        filepath: filepath,
        filename: getBaseFilename(filepath),
        title: title,
        privacy: privacy,
        status: 'uploading',
        progress: 100,
        videoId: null
      };
      state.uploadQueue.unshift(task);

      // Switch to queue view to show user the upload in progress
      switchTab('queue');
      renderQueueList();

    } catch (err) {
      if (alertBox) {
        alertBox.textContent = 'Upload failed: ' + err.message;
        alertBox.style.display = 'block';
      }
    } finally {
      if (submitBtn) {
        submitBtn.disabled = false;
        submitBtn.innerHTML = '<span class="btn-icon">▲</span> Upload to YouTube';
      }
    }
  }

  // --- Dynamic QR Code Setup ---
  function setupQRCode(customUrl) {
    const qrCanvas = document.getElementById('qr-code-canvas');
    const urlInput = document.getElementById('console-url-input');
    if (!qrCanvas) return;

    let fullUrl = customUrl;
    if (!fullUrl) {
      if (urlInput && urlInput.value && urlInput.value.trim() !== '') {
        fullUrl = urlInput.value.trim();
      } else {
        const host = window.location.host || 'localhost:8888';
        fullUrl = 'http://' + host + '/';
      }
    }

    if (urlInput && !customUrl) {
      urlInput.value = fullUrl;
    }

    try {
      const svgMarkup = renderQRCodeSVG(fullUrl, 200);
      qrCanvas.innerHTML = svgMarkup;
    } catch (err) {
      console.error('Failed to generate QR code:', err);
      qrCanvas.innerHTML = '<div style="color:#000;padding:1rem;">QR Code generation error</div>';
    }
  }

  // --- Tab Switching ---
  function switchTab(targetTab) {
    state.currentTab = targetTab;
    const tabs = document.querySelectorAll('.nav-tab');
    const panels = document.querySelectorAll('.view-panel');

    tabs.forEach(function (tab) {
      const isTarget = tab.getAttribute('data-tab') === targetTab;
      tab.classList.toggle('active', isTarget);
      tab.setAttribute('aria-selected', isTarget ? 'true' : 'false');
    });

    panels.forEach(function (panel) {
      const isTarget = panel.id === ('view-' + targetTab);
      panel.style.display = isTarget ? 'block' : 'none';
      panel.classList.toggle('active', isTarget);
    });

    if (targetTab === 'gallery') {
      loadClips();
    } else if (targetTab === 'queue') {
      renderQueueList();
    } else if (targetTab === 'pairing') {
      setupQRCode();
      loadStatus();
    }

    refreshFocusables();
  }

  // --- DualSense Controller & Keyboard TV Navigation ---
  let focusables = [];

  function refreshFocusables() {
    // Only query visible elements
    const selector = 'button:not([disabled]), input:not([disabled]):not([type="hidden"]), select:not([disabled]), textarea:not([disabled]), [tabindex="0"]';
    const elements = Array.from(document.querySelectorAll(selector));
    focusables = elements.filter(function (el) {
      return el.offsetParent !== null && !el.closest('[style*="display: none"]');
    });
  }

  function moveFocus(delta) {
    refreshFocusables();
    if (focusables.length === 0) return;

    let activeEl = document.activeElement;
    let idx = focusables.indexOf(activeEl);
    if (idx === -1) {
      idx = 0;
    } else {
      idx = (idx + delta + focusables.length) % focusables.length;
    }
    focusables[idx].focus();
  }

  function setupKeyboardNavigation() {
    window.addEventListener('keydown', function (e) {
      const modal = document.getElementById('upload-modal');
      const isModalOpen = modal && modal.style.display !== 'none';

      if (e.key === 'Escape') {
        if (isModalOpen) {
          closeUploadModal();
          e.preventDefault();
        }
        return;
      }

      // Couch Arrow Navigation (when not typing inside a textarea or text input)
      const isTextInput = e.target && (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA');

      if (e.key === 'ArrowDown' || e.key === 'ArrowRight') {
        if (!isTextInput || e.key === 'ArrowDown') {
          moveFocus(1);
          e.preventDefault();
        }
      } else if (e.key === 'ArrowUp' || e.key === 'ArrowLeft') {
        if (!isTextInput || e.key === 'ArrowUp') {
          moveFocus(-1);
          e.preventDefault();
        }
      }
    });
  }

  // DualSense Gamepad Poller
  let lastButtonPress = 0;
  function pollGamepad() {
    const gamepads = navigator.getGamepads ? navigator.getGamepads() : [];
    let pad = null;
    for (let i = 0; i < gamepads.length; i++) {
      if (gamepads[i]) { pad = gamepads[i]; break; }
    }

    const dualsenseBadge = document.getElementById('dualsense-status-text');
    if (pad) {
      if (dualsenseBadge) dualsenseBadge.textContent = 'DualSense: Connected';
      const now = Date.now();
      if (now - lastButtonPress > 200) { // 200ms debounce
        // Button 0 = Cross (Select)
        if (pad.buttons[0] && pad.buttons[0].pressed) {
          if (document.activeElement) document.activeElement.click();
          lastButtonPress = now;
        }
        // Button 1 = Circle (Cancel / Close)
        else if (pad.buttons[1] && pad.buttons[1].pressed) {
          closeUploadModal();
          lastButtonPress = now;
        }
        // Button 3 = Triangle (Refresh)
        else if (pad.buttons[3] && pad.buttons[3].pressed) {
          loadClips();
          lastButtonPress = now;
        }
        // Left analog stick axes support
        const axisX = (pad.axes && pad.axes.length > 0) ? pad.axes[0] : 0;
        const axisY = (pad.axes && pad.axes.length > 1) ? pad.axes[1] : 0;

        // D-Pad and Left Analog Stick navigation (buttons 12=Up, 13=Down, 14=Left, 15=Right)
        if ((pad.buttons[12] && pad.buttons[12].pressed) || axisY < -0.5) {
          moveFocus(-1);
          lastButtonPress = now;
        } else if ((pad.buttons[13] && pad.buttons[13].pressed) || axisY > 0.5) {
          moveFocus(1);
          lastButtonPress = now;
        } else if ((pad.buttons[14] && pad.buttons[14].pressed) || axisX < -0.5) {
          moveFocus(-1);
          lastButtonPress = now;
        } else if ((pad.buttons[15] && pad.buttons[15].pressed) || axisX > 0.5) {
          moveFocus(1);
          lastButtonPress = now;
        }
      }
    }

    requestAnimationFrame(pollGamepad);
  }

  // --- Event Listeners Initialization ---
  function initEvents() {
    // Nav tabs
    const tabs = document.querySelectorAll('.nav-tab');
    tabs.forEach(function (tab) {
      tab.addEventListener('click', function () {
        const target = this.getAttribute('data-tab');
        switchTab(target);
      });
    });

    // Refresh Clips
    const btnRefresh = document.getElementById('btn-refresh-clips');
    if (btnRefresh) btnRefresh.addEventListener('click', loadClips);

    const btnEmptyRefresh = document.getElementById('btn-empty-refresh');
    if (btnEmptyRefresh) btnEmptyRefresh.addEventListener('click', loadClips);

    const btnRefreshQueue = document.getElementById('btn-refresh-queue');
    if (btnRefreshQueue) btnRefreshQueue.addEventListener('click', function () {
      loadStatus();
      renderQueueList();
    });

    // Dynamic QR code update when user types LAN IP
    const urlInput = document.getElementById('console-url-input');
    if (urlInput) {
      const onUrlChanged = function () {
        let val = urlInput.value.trim();
        if (val) {
          if (!val.startsWith('http://') && !val.startsWith('https://')) {
            val = 'http://' + val;
          }
          setupQRCode(val);
        }
      };
      urlInput.addEventListener('input', onUrlChanged);
      urlInput.addEventListener('change', onUrlChanged);
    }

    // Copy URL button
    const btnCopyUrl = document.getElementById('btn-copy-url');
    if (btnCopyUrl) {
      btnCopyUrl.addEventListener('click', function () {
        const urlInput = document.getElementById('console-url-input');
        if (urlInput) {
          urlInput.select();
          document.execCommand('copy');
          btnCopyUrl.textContent = '✓ Copied!';
          setTimeout(function () { btnCopyUrl.textContent = '📋 Copy'; }, 2000);
        }
      });
    }

    // 1-Click Connect Session Button
    const btnConnectSession = document.getElementById('btn-connect-session');
    if (btnConnectSession) {
      btnConnectSession.addEventListener('click', async function () {
        btnConnectSession.disabled = true;
        btnConnectSession.textContent = 'Connecting...';
        try {
          await loadStatus();
          const authMsg = document.getElementById('auth-message-box');
          if (authMsg) {
            if (state.authenticated) {
              authMsg.className = 'alert-box alert-success';
              authMsg.textContent = 'Successfully connected to channel: ' + (state.channelName || 'YouTube Studio');
            } else {
              authMsg.className = 'alert-box alert-error';
              authMsg.textContent = 'No active session found. Please paste session cookies or token below.';
            }
            authMsg.style.display = 'block';
          }
        } finally {
          btnConnectSession.disabled = false;
          btnConnectSession.textContent = '✨ 1-Click Connect Current Session';
        }
      });
    }

    // Save Auth Form
    const btnSaveAuth = document.getElementById('btn-save-auth');
    if (btnSaveAuth) {
      btnSaveAuth.addEventListener('click', async function () {
        const cookies = document.getElementById('input-cookies').value.trim();
        const token = document.getElementById('input-session-token').value.trim();
        const chName = document.getElementById('input-channel-name').value.trim();
        const chId = document.getElementById('input-channel-id').value.trim();
        const authMsg = document.getElementById('auth-message-box');

        btnSaveAuth.disabled = true;
        btnSaveAuth.textContent = 'Saving...';
        try {
          const res = await submitAuth(cookies, token, chName, chId);
          if (authMsg) {
            authMsg.className = 'alert-box alert-success';
            authMsg.textContent = 'Authentication saved successfully!';
            authMsg.style.display = 'block';
          }
          await loadStatus();
        } catch (err) {
          if (authMsg) {
            authMsg.className = 'alert-box alert-error';
            authMsg.textContent = 'Failed to authenticate: ' + err.message;
            authMsg.style.display = 'block';
          }
        } finally {
          btnSaveAuth.disabled = false;
          btnSaveAuth.textContent = '💾 Save & Authenticate';
        }
      });
    }

    // Disconnect Auth
    const btnDisconnectAuth = document.getElementById('btn-disconnect-auth');
    if (btnDisconnectAuth) {
      btnDisconnectAuth.addEventListener('click', async function () {
        if (!confirm('Disconnect YouTube account credentials?')) return;
        const authMsg = document.getElementById('auth-message-box');
        try {
          await submitAuth('', '', '', '');
          document.getElementById('input-cookies').value = '';
          document.getElementById('input-session-token').value = '';
          document.getElementById('input-channel-name').value = '';
          document.getElementById('input-channel-id').value = '';
          if (authMsg) {
            authMsg.className = 'alert-box alert-success';
            authMsg.textContent = 'Disconnected YouTube account.';
            authMsg.style.display = 'block';
          }
          await loadStatus();
        } catch (e) {
          if (authMsg) {
            authMsg.className = 'alert-box alert-error';
            authMsg.textContent = 'Error disconnecting: ' + e.message;
            authMsg.style.display = 'block';
          }
        }
      });
    }

    // Save Settings
    const btnSaveSettings = document.getElementById('btn-save-settings');
    if (btnSaveSettings) {
      btnSaveSettings.addEventListener('click', async function () {
        const autoUpload = document.getElementById('chk-auto-upload').checked;
        const defaultPrivacy = document.getElementById('sel-default-privacy').value;
        const msgBox = document.getElementById('settings-message-box');

        btnSaveSettings.disabled = true;
        btnSaveSettings.textContent = 'Saving...';
        try {
          await submitConfig(autoUpload, defaultPrivacy);
          state.autoUpload = autoUpload;
          state.defaultPrivacy = defaultPrivacy;
          if (msgBox) {
            msgBox.className = 'alert-box alert-success';
            msgBox.textContent = 'Settings saved successfully!';
            msgBox.style.display = 'block';
          }
        } catch (err) {
          if (msgBox) {
            msgBox.className = 'alert-box alert-error';
            msgBox.textContent = 'Failed to save settings: ' + err.message;
            msgBox.style.display = 'block';
          }
        } finally {
          btnSaveSettings.disabled = false;
          btnSaveSettings.textContent = 'Save Settings';
        }
      });
    }

    // Modal buttons
    const btnModalClose = document.getElementById('btn-modal-close');
    if (btnModalClose) btnModalClose.addEventListener('click', closeUploadModal);

    const btnModalCancel = document.getElementById('btn-modal-cancel');
    if (btnModalCancel) btnModalCancel.addEventListener('click', closeUploadModal);

    const btnModalSubmit = document.getElementById('btn-modal-submit');
    if (btnModalSubmit) btnModalSubmit.addEventListener('click', handleModalSubmit);

    // Keyboard & Gamepad
    setupKeyboardNavigation();
    if (window.requestAnimationFrame) {
      requestAnimationFrame(pollGamepad);
    }
  }

  // --- Application Bootstrap ---
  window.addEventListener('DOMContentLoaded', function () {
    initEvents();
    setupQRCode();
    loadStatus();
    loadClips();

    // Background status polling every 3 seconds
    setInterval(function () {
      loadStatus();
    }, 3000);
  });

})();
