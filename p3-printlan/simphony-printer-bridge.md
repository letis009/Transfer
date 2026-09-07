# simphony-printer-bridge

A virtual Epson TM-T88 Ethernet printer that Oracle Simphony talks to as a normal
**IP Printer** on TCP 9100, and which relays each slip to a real printer over
Ethernet, USB or USB-CDC.

Target: embedded Linux (Pi for bring-up, T113-S3 / RV1106 class SoC for the
production board). Runtime is Node.js, CommonJS, zero mandatory dependencies —
`serialport` and `usb` are optional and only loaded if the corresponding backend
is enabled.

---

## 1. Architecture

```
                 TCP 9100 (ESC/POS)
Simphony WS  ───────────────────────►  PosListener
(print ctrl)  ◄──── status bytes ────      │
                                           │  EscPosScanner
                                           │   ├─ strips + answers real-time queries
                                           │   ├─ skips over binary payloads safely
                                           │   └─ detects cut = job boundary
                                           ▼
                                       JobSpool  (disk-backed, ordered, retrying)
                                           │
                                           ▼
                                    Backend (one of)
                                     ├─ tcp     → real printer :9100
                                     ├─ usblp   → /dev/usb/lp0
                                     ├─ serial  → /dev/ttyACM0 (CDC-ACM)
                                     └─ file    → spool dir (dev/test)
                                           │
                                           ▼
                                    PrinterState  ──► fed back upstream
```

Two rules the whole design hangs on:

1. **The upstream socket must never block on the backend.** Status queries are
   answered from a cached `PrinterState` in the same tick they arrive.
2. **Status is reported truthfully.** If you always answer "ready", Simphony's
   backup-printer failover will never fire and slips will silently vanish.

---

## 2. Directory scaffold

```
simphony-printer-bridge/
├── package.json
├── README.md
├── config/
│   └── default.json
├── src/
│   ├── index.js                 # wiring + lifecycle
│   ├── config.js                # load + merge + env overrides
│   ├── log.js                   # leveled logger, journald-friendly
│   ├── escpos/
│   │   ├── constants.js         # opcodes + command-length tables
│   │   ├── scanner.js           # streaming, length-aware ESC/POS scanner
│   │   ├── status.js            # PrinterState + DLE EOT / GS r / ESC v encoders
│   │   └── responder.js         # query → reply bytes
│   ├── server/
│   │   └── posListener.js       # TCP 9100, single session, job assembly
│   ├── spool/
│   │   └── jobSpool.js          # disk-backed FIFO with retry/backoff
│   └── backends/
│       ├── index.js             # factory
│       ├── tcp.js
│       ├── usblp.js
│       ├── serial.js
│       └── file.js
├── tools/
│   ├── escpos-dump.js           # annotate a captured 9100 stream
│   └── fake-pos.js              # pretend to be Simphony: print + poll + time
├── systemd/
│   └── simphony-bridge.service
└── test/
    └── scanner.test.js
```

---

## 3. `package.json`

```json
{
  "name": "simphony-printer-bridge",
  "version": "0.1.0",
  "description": "Virtual Epson TM-T88 Ethernet printer for Oracle Simphony, relaying to a real network/USB printer",
  "main": "src/index.js",
  "engines": { "node": ">=18" },
  "scripts": {
    "start": "node src/index.js",
    "dump": "node tools/escpos-dump.js",
    "fakepos": "node tools/fake-pos.js",
    "test": "node --test test/"
  },
  "optionalDependencies": {
    "serialport": "^12.0.0"
  },
  "license": "UNLICENSED"
}
```

---

## 4. `config/default.json`

```json
{
  "listen": {
    "host": "0.0.0.0",
    "port": 9100,
    "singleSession": true,
    "idleTimeoutMs": 0,
    "jobIdleMs": 400,
    "flushOnClose": true,
    "maxJobBytes": 4194304
  },
  "identity": {
    "modelId": 32,
    "typeId": 2,
    "firmwareId": 3
  },
  "backend": {
    "type": "tcp",
    "tcp":    { "host": "192.168.1.50", "port": 9100, "pollMs": 3000, "replyTimeoutMs": 800 },
    "usblp":  { "device": "/dev/usb/lp0", "readStatus": true },
    "serial": { "device": "/dev/ttyACM0", "baudRate": 115200 },
    "file":   { "dir": "/var/lib/simphony-bridge/out" }
  },
  "spool": {
    "dir": "/var/lib/simphony-bridge/spool",
    "retryBaseMs": 1000,
    "retryMaxMs": 30000,
    "maxJobs": 200
  },
  "log": { "level": "info", "rawCapture": false, "rawCaptureDir": "/var/lib/simphony-bridge/raw" }
}
```

---

## 5. `src/escpos/constants.js`

```js
'use strict';

// Control bytes
const ESC = 0x1b, GS = 0x1d, FS = 0x1c, DLE = 0x10;
const EOT = 0x04, ENQ = 0x05, DC4 = 0x14;

const NEED_MORE = -1;   // command straddles the end of the buffer
const UNKNOWN   = -2;   // not a command we model: treat the byte as data

// --- fixed-length commands ---------------------------------------------
// Values are TOTAL length including the ESC/GS/FS prefix.

const ESC_FIXED = {
  0x0c: 2,  // ESC FF     print page (page mode)
  0x20: 3,  // ESC SP n   right-side character spacing
  0x21: 3,  // ESC ! n    print mode
  0x24: 4,  // ESC $      absolute position
  0x25: 3,  // ESC % n    select user-defined char set
  0x2d: 3,  // ESC - n    underline
  0x32: 2,  // ESC 2      default line spacing
  0x33: 3,  // ESC 3 n    line spacing
  0x3d: 3,  // ESC = n    peripheral device select
  0x3f: 3,  // ESC ? n    cancel user-defined char
  0x40: 2,  // ESC @      initialise
  0x45: 3,  // ESC E n    emphasised
  0x47: 3,  // ESC G n    double strike
  0x4a: 3,  // ESC J n    feed n dots
  0x4b: 3,  // ESC K n    reverse feed
  0x4c: 2,  // ESC L      page mode
  0x4d: 3,  // ESC M n    font
  0x52: 3,  // ESC R n    international char set
  0x53: 2,  // ESC S      standard mode
  0x54: 3,  // ESC T n    page-mode direction
  0x56: 3,  // ESC V n    90deg rotation
  0x57: 10, // ESC W      print area (8 params)
  0x5c: 4,  // ESC \      relative position
  0x61: 3,  // ESC a n    justification
  0x64: 3,  // ESC d n    feed n lines
  0x69: 2,  // ESC i      full cut (legacy)
  0x6d: 2,  // ESC m      partial cut (legacy)
  0x70: 5,  // ESC p m t1 t2   drawer pulse
  0x72: 3,  // ESC r n    print colour
  0x74: 3,  // ESC t n    code page
  0x75: 3,  // ESC u n    transmit peripheral status  -> RESPONSE
  0x76: 2,  // ESC v      transmit paper sensor       -> RESPONSE
  0x7b: 3   // ESC { n    upside-down
};

const GS_FIXED = {
  0x21: 3,  // GS ! n     character size
  0x24: 4,  // GS $       absolute vertical position
  0x2f: 3,  // GS / m     print downloaded bit image
  0x3a: 2,  // GS :       start/end macro
  0x42: 3,  // GS B n     white/black reverse
  0x45: 3,  // GS E n     head control
  0x48: 3,  // GS H n     HRI position
  0x49: 3,  // GS I n     transmit printer ID          -> RESPONSE
  0x4c: 4,  // GS L       left margin
  0x50: 4,  // GS P x y   motion units
  0x54: 3,  // GS T n     line position
  0x57: 4,  // GS W       print area width
  0x5c: 4,  // GS \       relative vertical position
  0x5e: 5,  // GS ^ r t m execute macro
  0x61: 3,  // GS a n     enable ASB                   -> INTERCEPT
  0x62: 3,  // GS b n     smoothing
  0x66: 3,  // GS f n     HRI font
  0x68: 3,  // GS h n     barcode height
  0x72: 3,  // GS r n     transmit status              -> RESPONSE
  0x77: 3   // GS w n     barcode width
};

const FS_FIXED = {
  0x21: 3,  // FS ! n
  0x26: 2,  // FS &   select kanji
  0x2d: 3,  // FS - n
  0x2e: 2,  // FS .   cancel kanji
  0x43: 3,  // FS C n kanji code system
  0x53: 4,  // FS S   kanji spacing
  0x57: 3,  // FS W n kanji double size
  0x70: 4   // FS p n m  print NV bit image
};

// Commands that must never be forwarded downstream: we answer them ourselves
// from the cached PrinterState so the upstream socket is never blocked.
const QUERY = { GS_I: 'GS_I', GS_r: 'GS_r', ESC_v: 'ESC_v', ESC_u: 'ESC_u', DLE_EOT: 'DLE_EOT' };

module.exports = { ESC, GS, FS, DLE, EOT, ENQ, DC4, NEED_MORE, UNKNOWN,
                   ESC_FIXED, GS_FIXED, FS_FIXED, QUERY };
```

---

## 6. `src/escpos/scanner.js`

The important part. A naive `indexOf(0x10 0x04)` scan **will** corrupt jobs,
because those bytes occur inside raster images, NV logos and downloaded
characters. This scanner is length-aware: it walks command by command and steps
over binary payloads without looking inside them.

```js
'use strict';

const C = require('./constants');
const { ESC, GS, FS, DLE, NEED_MORE, UNKNOWN } = C;

const u16 = (b, i) => b[i] | (b[i + 1] << 8);
const fit = (b, i, len) => (i + len <= b.length ? len : NEED_MORE);

function escLen(b, i) {
  if (i + 2 > b.length) return NEED_MORE;
  const c = b[i + 1];
  if (C.ESC_FIXED[c]) return fit(b, i, C.ESC_FIXED[c]);

  switch (c) {
    case 0x2a: {                       // ESC * m nL nH [d]
      if (i + 5 > b.length) return NEED_MORE;
      const m = b[i + 2], n = u16(b, i + 3);
      return fit(b, i, 5 + (m === 0 || m === 1 ? n : n * 3));
    }
    case 0x44: {                       // ESC D [n]* NUL
      let j = i + 2;
      while (j < b.length && b[j] !== 0x00) j++;
      return j < b.length ? j - i + 1 : NEED_MORE;
    }
    case 0x63:                         // ESC c 3|4|5 n
      return fit(b, i, 4);
    case 0x26: {                       // ESC & y c1 c2 [x d..]*
      if (i + 5 > b.length) return NEED_MORE;
      const y = b[i + 2], c1 = b[i + 3], c2 = b[i + 4];
      let j = i + 5;
      for (let ch = c1; ch <= c2; ch++) {
        if (j >= b.length) return NEED_MORE;
        j += 1 + b[j] * y;
      }
      return j <= b.length ? j - i : NEED_MORE;
    }
    default:
      return UNKNOWN;
  }
}

function gsLen(b, i) {
  if (i + 2 > b.length) return NEED_MORE;
  const c = b[i + 1];
  if (C.GS_FIXED[c]) return fit(b, i, C.GS_FIXED[c]);

  switch (c) {
    case 0x28:                         // GS ( fn pL pH [params]
      if (i + 5 > b.length) return NEED_MORE;
      return fit(b, i, 5 + u16(b, i + 3));
    case 0x38:                         // GS 8 L p1..p4 [params]
      if (i + 7 > b.length) return NEED_MORE;
      return fit(b, i, 7 + b.readUInt32LE(i + 3));
    case 0x2a:                         // GS * x y [d]
      if (i + 4 > b.length) return NEED_MORE;
      return fit(b, i, 4 + b[i + 2] * b[i + 3] * 8);
    case 0x56: {                       // GS V m  |  GS V m n
      if (i + 3 > b.length) return NEED_MORE;
      const m = b[i + 2];
      return fit(b, i, m === 0 || m === 1 || m === 48 || m === 49 ? 3 : 4);
    }
    case 0x6b: {                       // GS k  (two forms)
      if (i + 3 > b.length) return NEED_MORE;
      if (b[i + 2] <= 6) {             // NUL-terminated
        let j = i + 3;
        while (j < b.length && b[j] !== 0x00) j++;
        return j < b.length ? j - i + 1 : NEED_MORE;
      }
      if (i + 4 > b.length) return NEED_MORE;
      return fit(b, i, 4 + b[i + 3]);  // length-prefixed
    }
    case 0x76: {                       // GS v 0 m xL xH yL yH [d]
      if (i + 8 > b.length) return NEED_MORE;
      return fit(b, i, 8 + u16(b, i + 4) * u16(b, i + 6));
    }
    case 0x7a:                         // GS z 0 n
      return fit(b, i, 4);
    default:
      return UNKNOWN;
  }
}

function fsLen(b, i) {
  if (i + 2 > b.length) return NEED_MORE;
  const c = b[i + 1];
  if (C.FS_FIXED[c]) return fit(b, i, C.FS_FIXED[c]);

  if (c === 0x71) {                    // FS q n [xL xH yL yH d..]*n
    if (i + 3 > b.length) return NEED_MORE;
    const n = b[i + 2];
    let j = i + 3;
    for (let k = 0; k < n; k++) {
      if (j + 4 > b.length) return NEED_MORE;
      j += 4 + u16(b, j) * u16(b, j + 2) * 8;
    }
    return j <= b.length ? j - i : NEED_MORE;
  }
  return UNKNOWN;
}

function dleLen(b, i) {
  if (i + 2 > b.length) return NEED_MORE;
  const c = b[i + 1];
  if (c === C.EOT) {                   // DLE EOT n [a]
    if (i + 3 > b.length) return NEED_MORE;
    return fit(b, i, b[i + 2] === 8 ? 4 : 3);
  }
  if (c === C.ENQ) return fit(b, i, 3);
  if (c === C.DC4) {                   // DLE DC4 fn ...
    if (i + 3 > b.length) return NEED_MORE;
    const fn = b[i + 2];
    const len = fn === 1 ? 5 : fn === 2 ? 4 : fn === 3 ? 5 : fn === 8 ? 7 : 3;
    return fit(b, i, len);
  }
  return UNKNOWN;
}

/**
 * Streaming scanner.
 *
 * push(chunk) -> ordered array of items:
 *   { kind: 'data',     buf }              bytes to relay verbatim
 *   { kind: 'cut' }                        job boundary (cut bytes already in the
 *                                          preceding 'data' item)
 *   { kind: 'query',    q, n }             answer upstream, do NOT relay
 *   { kind: 'realtime', cmd, n, raw }      DLE ENQ / DLE DC4, handled locally
 *   { kind: 'asb',      n }                GS a n: record mask, do NOT relay
 */
class EscPosScanner {
  constructor({ maxPending = 4 * 1024 * 1024 } = {}) {
    this.buf = Buffer.alloc(0);
    this.maxPending = maxPending;
  }

  reset() { this.buf = Buffer.alloc(0); }

  push(chunk) {
    const b = this.buf.length ? Buffer.concat([this.buf, chunk]) : chunk;
    const items = [];
    let i = 0, flushed = 0;

    const emitData = (end) => {
      if (end > flushed) items.push({ kind: 'data', buf: b.subarray(flushed, end) });
      flushed = end;
    };

    while (i < b.length) {
      const op = b[i];
      let len;

      if (op === DLE) {
        len = dleLen(b, i);
        if (len === NEED_MORE) break;
        if (len !== UNKNOWN) {
          emitData(i);
          if (b[i + 1] === C.EOT) items.push({ kind: 'query', q: 'DLE_EOT', n: b[i + 2] });
          else items.push({ kind: 'realtime', cmd: b[i + 1], n: b[i + 2], raw: b.subarray(i, i + len) });
          i += len; flushed = i;                       // stripped, never relayed
          continue;
        }
        i += 1; continue;                              // lone 0x10 = data
      }

      len = op === ESC ? escLen(b, i) : op === GS ? gsLen(b, i) : op === FS ? fsLen(b, i) : UNKNOWN;
      if (len === NEED_MORE) break;
      if (len === UNKNOWN) { i += 1; continue; }        // plain text byte

      const c = b[i + 1];

      // Status queries: strip and answer locally.
      if ((op === GS && (c === 0x49 || c === 0x72)) || (op === ESC && (c === 0x75 || c === 0x76))) {
        emitData(i);
        const q = op === GS ? (c === 0x49 ? 'GS_I' : 'GS_r') : (c === 0x75 ? 'ESC_u' : 'ESC_v');
        items.push({ kind: 'query', q, n: b[i + 2] });
        i += len; flushed = i;
        continue;
      }

      // ASB enable: record, strip. We generate ASB upstream ourselves.
      if (op === GS && c === 0x61) {
        emitData(i);
        items.push({ kind: 'asb', n: b[i + 2] });
        i += len; flushed = i;
        continue;
      }

      // Cut = job boundary. The cut bytes stay in the relayed stream.
      if ((op === GS && c === 0x56) || (op === ESC && (c === 0x69 || c === 0x6d))) {
        i += len;
        emitData(i);
        items.push({ kind: 'cut' });
        continue;
      }

      i += len;
    }

    emitData(i);
    this.buf = b.subarray(i);
    if (this.buf.length > this.maxPending) {
      // A malformed length field would otherwise pin memory forever.
      const err = new Error(`pending buffer exceeded ${this.maxPending} bytes`);
      this.reset();
      throw err;
    }
    return items;
  }
}

module.exports = { EscPosScanner, escLen, gsLen, fsLen, dleLen };
```

---

## 7. `src/escpos/status.js`

```js
'use strict';

const { EventEmitter } = require('events');

// ESC/POS status bytes carry fixed bits: bit0=0, bit1=1, bit4=1, bit7=0.
const FIXED = 0b00010010; // 0x12

// VERIFY these bit maps against the ESC/POS Command Reference for the exact
// model you emulate, and against your own port-9100 capture. They match the
// TM-T88 family, which is what Simphony's IP Printer path expects.

function dleEot(n, s) {
  switch (n) {
    case 1: {                       // printer status
      let b = FIXED;
      if (s.drawerHigh) b |= 1 << 2;
      if (!s.online)    b |= 1 << 3;
      return b;
    }
    case 2: {                       // offline cause
      let b = FIXED;
      if (s.coverOpen)  b |= 1 << 2;
      if (s.feedButton) b |= 1 << 3;
      if (s.paperEnd)   b |= 1 << 5;
      if (s.error)      b |= 1 << 6;
      return b;
    }
    case 3: {                       // error cause
      let b = FIXED;
      if (s.cutterError)         b |= 1 << 3;
      if (s.unrecoverableError)  b |= 1 << 5;
      if (s.autoRecoverError)    b |= 1 << 6;
      return b;
    }
    case 4: {                       // paper roll sensors
      let b = FIXED;
      if (s.paperNearEnd) b |= (1 << 2) | (1 << 3);
      if (s.paperEnd)     b |= (1 << 5) | (1 << 6);
      return b;
    }
    default:
      return FIXED;
  }
}

function escV(s) {                  // paper sensor, 1 byte
  let b = 0;
  if (s.paperNearEnd) b |= 0b00000011;
  if (s.paperEnd)     b |= 0b00001100;
  return b;
}

function escU(s) { return s.drawerHigh ? 0x01 : 0x00; }

function gsR(n, s) {
  if (n === 1 || n === 49) return escV(s);
  if (n === 2 || n === 50) return escU(s);
  return 0x00;
}

class PrinterState extends EventEmitter {
  constructor() {
    super();
    this.online = false;            // pessimistic until the backend proves otherwise
    this.coverOpen = false;
    this.paperEnd = false;
    this.paperNearEnd = false;
    this.cutterError = false;
    this.unrecoverableError = false;
    this.autoRecoverError = false;
    this.feedButton = false;
    this.drawerHigh = false;
  }

  get error() {
    return this.cutterError || this.unrecoverableError || this.autoRecoverError;
  }

  update(patch) {
    let changed = false;
    for (const [k, v] of Object.entries(patch)) {
      if (this[k] !== v) { this[k] = v; changed = true; }
    }
    if (changed) this.emit('change', this.snapshot());
    return changed;
  }

  snapshot() {
    const { online, coverOpen, paperEnd, paperNearEnd, cutterError,
            unrecoverableError, autoRecoverError, drawerHigh } = this;
    return { online, coverOpen, paperEnd, paperNearEnd, cutterError,
             unrecoverableError, autoRecoverError, drawerHigh, error: this.error };
  }

  /**
   * Map a backend's DLE EOT reply byte back into our state.
   * n is the query index that produced it.
   */
  ingestReply(n, byte) {
    switch (n) {
      case 1: return this.update({ online: !(byte & (1 << 3)), drawerHigh: !!(byte & (1 << 2)) });
      case 2: return this.update({ coverOpen: !!(byte & (1 << 2)), paperEnd: !!(byte & (1 << 5)) });
      case 3: return this.update({ cutterError: !!(byte & (1 << 3)),
                                   unrecoverableError: !!(byte & (1 << 5)),
                                   autoRecoverError: !!(byte & (1 << 6)) });
      case 4: return this.update({ paperNearEnd: !!(byte & (1 << 2)),
                                   paperEnd: !!(byte & (1 << 5)) });
      default: return false;
    }
  }
}

module.exports = { PrinterState, dleEot, escV, escU, gsR, FIXED };
```

---

## 8. `src/escpos/responder.js`

```js
'use strict';

const { dleEot, escV, escU, gsR } = require('./status');

/**
 * Build the reply for an intercepted query. Returns null when the command
 * takes no reply.
 *
 * identity.modelId / typeId / firmwareId should be set from your capture of a
 * real TM-T88 so Simphony sees the ID it expects.
 */
function respond(item, state, identity) {
  switch (item.q) {
    case 'DLE_EOT': return Buffer.from([dleEot(item.n, state)]);
    case 'ESC_v':   return Buffer.from([escV(state)]);
    case 'ESC_u':   return Buffer.from([escU(state)]);
    case 'GS_r':    return Buffer.from([gsR(item.n, state)]);
    case 'GS_I':
      if (item.n === 1 || item.n === 49) return Buffer.from([identity.modelId]);
      if (item.n === 2 || item.n === 50) return Buffer.from([identity.typeId]);
      if (item.n === 3 || item.n === 51) return Buffer.from([identity.firmwareId]);
      return null;
    default:
      return null;
  }
}

/** 4-byte Automatic Status Back packet, sent unsolicited when state changes. */
function asbPacket(state) {
  let b1 = 0x10;                                   // fixed bit4
  if (state.drawerHigh) b1 |= 1 << 2;
  if (!state.online || state.coverOpen) b1 |= 1 << 3;

  let b2 = 0x00;
  if (state.cutterError)        b2 |= 1 << 3;
  if (state.unrecoverableError) b2 |= 1 << 5;
  if (state.autoRecoverError)   b2 |= 1 << 6;

  let b3 = 0x00;
  if (state.paperNearEnd) b3 |= 0b00000011;
  if (state.paperEnd)     b3 |= 0b00001100;

  return Buffer.from([b1, b2, b3, 0x00]);
}

module.exports = { respond, asbPacket };
```

> ASB bit assignments vary more between models than the `DLE EOT` ones do.
> Confirm `asbPacket` against your capture before trusting it; if Simphony never
> sends `GS a n`, leave ASB disabled and rely on polling alone.

---

## 9. `src/server/posListener.js`

```js
'use strict';

const net = require('net');
const { EventEmitter } = require('events');
const { EscPosScanner } = require('../escpos/scanner');
const { respond, asbPacket } = require('../escpos/responder');

class PosListener extends EventEmitter {
  constructor({ cfg, state, identity, log }) {
    super();
    this.cfg = cfg;
    this.state = state;
    this.identity = identity;
    this.log = log;
    this.sock = null;
    this.scanner = null;
    this.job = [];
    this.jobBytes = 0;
    this.jobTimer = null;
    this.asbMask = 0;

    this.state.on('change', (snap) => {
      if (this.sock && this.asbMask) this._write(asbPacket(snap));
    });
  }

  start() {
    this.server = net.createServer((s) => this._accept(s));
    this.server.on('error', (e) => this.emit('error', e));
    this.server.listen(this.cfg.port, this.cfg.host, () =>
      this.log.info(`listening on ${this.cfg.host}:${this.cfg.port}`));
  }

  stop() { this.server && this.server.close(); this.sock && this.sock.destroy(); }

  _accept(sock) {
    // A real TM printer serves one connection at a time. Mimic it, or Simphony
    // may open a second session on retry and interleave two jobs.
    if (this.cfg.singleSession && this.sock) {
      this.log.warn(`refusing second session from ${sock.remoteAddress}`);
      return sock.destroy();
    }

    this.sock = sock;
    this.scanner = new EscPosScanner({ maxPending: this.cfg.maxJobBytes });
    sock.setNoDelay(true);
    sock.setKeepAlive(true, 10000);
    if (this.cfg.idleTimeoutMs > 0) sock.setTimeout(this.cfg.idleTimeoutMs);

    this.log.info(`session open from ${sock.remoteAddress}`);
    this.emit('session', { remote: sock.remoteAddress, open: true });

    sock.on('data', (chunk) => this._onData(chunk));
    sock.on('timeout', () => { this.log.warn('idle timeout'); sock.destroy(); });
    sock.on('error', (e) => this.log.warn(`session error: ${e.message}`));
    sock.on('close', () => {
      if (this.cfg.flushOnClose) this._closeJob('socket-close');
      else this._discardJob();
      this.sock = null;
      this.asbMask = 0;
      this.log.info('session closed');
      this.emit('session', { open: false });
    });
  }

  _onData(chunk) {
    this.emit('raw', chunk);
    let items;
    try {
      items = this.scanner.push(chunk);
    } catch (e) {
      this.log.error(`scanner desync: ${e.message} — dropping session`);
      return this.sock.destroy();
    }

    for (const it of items) {
      switch (it.kind) {
        case 'query': {
          // Answered from cache, same tick. Never awaits the backend.
          const reply = respond(it, this.state, this.identity);
          if (reply) this._write(reply);
          break;
        }
        case 'realtime':
          // DLE ENQ (recover) / DLE DC4 (pulse, cancel). Surface and drop.
          this.emit('realtime', it);
          break;
        case 'asb':
          this.asbMask = it.n;
          this.log.debug(`ASB mask set to ${it.n}`);
          if (it.n) this._write(asbPacket(this.state.snapshot()));
          break;
        case 'data':
          this.job.push(it.buf);
          this.jobBytes += it.buf.length;
          if (this.jobBytes > this.cfg.maxJobBytes) {
            this.log.error('job exceeded maxJobBytes — discarding');
            this._discardJob();
          }
          this._armIdle();
          break;
        case 'cut':
          this._closeJob('cut');
          break;
      }
    }
  }

  _write(buf) {
    if (this.sock && !this.sock.destroyed) this.sock.write(buf);
  }

  _armIdle() {
    clearTimeout(this.jobTimer);
    if (this.cfg.jobIdleMs > 0) {
      this.jobTimer = setTimeout(() => this._closeJob('idle'), this.cfg.jobIdleMs);
    }
  }

  _closeJob(reason) {
    clearTimeout(this.jobTimer);
    if (!this.jobBytes) return;
    const buf = Buffer.concat(this.job, this.jobBytes);
    this.job = []; this.jobBytes = 0;
    this.log.info(`job closed (${reason}), ${buf.length} bytes`);
    this.emit('job', buf);
  }

  _discardJob() {
    clearTimeout(this.jobTimer);
    this.job = []; this.jobBytes = 0;
  }
}

module.exports = { PosListener };
```

---

## 10. `src/spool/jobSpool.js`

```js
'use strict';

const fs = require('fs');
const path = require('path');
const { EventEmitter } = require('events');

/**
 * Disk-backed FIFO. A slip that reached us is never lost to a power cut or a
 * dead downstream printer — it is fsynced before we consider it accepted.
 */
class JobSpool extends EventEmitter {
  constructor({ cfg, backend, log }) {
    super();
    this.cfg = cfg;
    this.backend = backend;
    this.log = log;
    this.seq = 0;
    this.busy = false;
    this.delay = cfg.retryBaseMs;
    fs.mkdirSync(cfg.dir, { recursive: true });
  }

  enqueue(buf) {
    const name = `${Date.now()}-${String(++this.seq).padStart(6, '0')}.bin`;
    const tmp = path.join(this.cfg.dir, `.${name}`);
    const dst = path.join(this.cfg.dir, name);
    const fd = fs.openSync(tmp, 'w');
    fs.writeSync(fd, buf);
    fs.fsyncSync(fd);
    fs.closeSync(fd);
    fs.renameSync(tmp, dst);              // atomic publish
    this.log.info(`spooled ${name} (${buf.length} bytes)`);
    this.emit('spooled', { name, bytes: buf.length });
    this.pump();
  }

  list() {
    return fs.readdirSync(this.cfg.dir).filter((f) => f.endsWith('.bin')).sort();
  }

  depth() { return this.list().length; }

  async pump() {
    if (this.busy) return;
    this.busy = true;
    try {
      for (;;) {
        const files = this.list();
        if (!files.length) { this.delay = this.cfg.retryBaseMs; break; }

        const file = path.join(this.cfg.dir, files[0]);
        const buf = fs.readFileSync(file);
        try {
          await this.backend.print(buf);
          fs.unlinkSync(file);
          this.delay = this.cfg.retryBaseMs;
          this.log.info(`printed ${files[0]}, ${files.length - 1} left`);
          this.emit('printed', { name: files[0] });
        } catch (e) {
          this.log.warn(`print failed (${e.message}), retry in ${this.delay}ms`);
          this.emit('failed', { name: files[0], error: e.message });
          setTimeout(() => this.pump(), this.delay);
          this.delay = Math.min(this.delay * 2, this.cfg.retryMaxMs);
          break;
        }
      }
    } finally {
      this.busy = false;
    }
  }
}

module.exports = { JobSpool };
```

---

## 11. Backends

### `src/backends/index.js`

```js
'use strict';

module.exports.create = function create(cfg, state, log) {
  switch (cfg.type) {
    case 'tcp':    return new (require('./tcp').TcpBackend)(cfg.tcp, state, log);
    case 'usblp':  return new (require('./usblp').UsbLpBackend)(cfg.usblp, state, log);
    case 'serial': return new (require('./serial').SerialBackend)(cfg.serial, state, log);
    case 'file':   return new (require('./file').FileBackend)(cfg.file, state, log);
    default: throw new Error(`unknown backend type: ${cfg.type}`);
  }
};
```

### `src/backends/tcp.js`

```js
'use strict';

const net = require('net');

/**
 * Relay to a real Ethernet printer on :9100 and poll it for status.
 *
 * We disable ASB downstream (GS a 0) on connect. Otherwise unsolicited 4-byte
 * ASB packets interleave with 1-byte DLE EOT replies on the same socket and
 * there is no reliable way to tell them apart.
 */
class TcpBackend {
  constructor(cfg, state, log) {
    this.cfg = cfg; this.state = state; this.log = log;
    this.sock = null;
    this.pending = null;                 // { resolve, reject, timer }
    this.connect();
    this.timer = setInterval(() => this.poll().catch(() => {}), cfg.pollMs);
  }

  connect() {
    if (this.sock) return;
    const s = net.connect({ host: this.cfg.host, port: this.cfg.port });
    s.setNoDelay(true);
    s.setKeepAlive(true, 10000);

    s.on('connect', () => {
      this.log.info(`backend connected to ${this.cfg.host}:${this.cfg.port}`);
      this.sock = s;
      s.write(Buffer.from([0x1d, 0x61, 0x00]));   // GS a 0 — ASB off
      this.poll().catch(() => {});
    });
    s.on('data', (d) => this._onData(d));
    s.on('error', (e) => this.log.warn(`backend socket: ${e.message}`));
    s.on('close', () => {
      this.sock = null;
      this.state.update({ online: false });
      setTimeout(() => this.connect(), 2000);
    });
  }

  _onData(d) {
    if (this.pending && d.length) {
      const p = this.pending; this.pending = null;
      clearTimeout(p.timer);
      p.resolve(d[0]);
    }
  }

  _ask(bytes) {
    return new Promise((resolve, reject) => {
      if (!this.sock) return reject(new Error('backend offline'));
      if (this.pending) return reject(new Error('query in flight'));
      const timer = setTimeout(() => {
        this.pending = null;
        reject(new Error('status timeout'));
      }, this.cfg.replyTimeoutMs);
      this.pending = { resolve, reject, timer };
      this.sock.write(Buffer.from(bytes));
    });
  }

  async poll() {
    if (!this.sock) return this.state.update({ online: false });
    for (const n of [1, 2, 3, 4]) {
      const byte = await this._ask([0x10, 0x04, n]);
      this.state.ingestReply(n, byte);
    }
  }

  print(buf) {
    return new Promise((resolve, reject) => {
      if (!this.sock) return reject(new Error('backend offline'));
      this.sock.write(buf, (err) => (err ? reject(err) : resolve()));
    });
  }

  close() { clearInterval(this.timer); this.sock && this.sock.destroy(); }
}

module.exports = { TcpBackend };
```

### `src/backends/usblp.js`

```js
'use strict';

const fs = require('fs');

/**
 * Kernel usblp path: works with any printer that enumerates as USB printer
 * class (bInterfaceClass 0x07). Bidirectional devices (protocol 2) also answer
 * status reads.
 *
 * If lsusb shows CDC-ACM instead — which the TM-T88VI and TM-P80II do by
 * default — use the serial backend, not this one.
 */
class UsbLpBackend {
  constructor(cfg, state, log) {
    this.cfg = cfg; this.state = state; this.log = log;
    this.open();
    this.timer = setInterval(() => this.poll(), 3000);
  }

  open() {
    try {
      this.fd = fs.openSync(this.cfg.device, this.cfg.readStatus ? 'r+' : 'w');
      this.state.update({ online: true });
      this.log.info(`opened ${this.cfg.device}`);
    } catch (e) {
      this.fd = null;
      this.state.update({ online: false });
      this.log.warn(`cannot open ${this.cfg.device}: ${e.message}`);
    }
  }

  poll() {
    if (this.fd === null) return this.open();
    if (!this.cfg.readStatus) return;
    try {
      fs.writeSync(this.fd, Buffer.from([0x10, 0x04, 1]));
      const rx = Buffer.alloc(1);
      const n = fs.readSync(this.fd, rx, 0, 1, null);
      if (n === 1) this.state.ingestReply(1, rx[0]);
    } catch (e) {
      this.log.debug(`usblp status read failed: ${e.message}`);
    }
  }

  print(buf) {
    return new Promise((resolve, reject) => {
      if (this.fd === null) return reject(new Error('usblp not open'));
      try { fs.writeSync(this.fd, buf); resolve(); }
      catch (e) {
        try { fs.closeSync(this.fd); } catch {}
        this.fd = null;
        this.state.update({ online: false });
        reject(e);
      }
    });
  }

  close() { clearInterval(this.timer); if (this.fd !== null) fs.closeSync(this.fd); }
}

module.exports = { UsbLpBackend };
```

### `src/backends/serial.js`

```js
'use strict';

/** CDC-ACM / RS-232 printers. Requires the optional `serialport` dependency. */
class SerialBackend {
  constructor(cfg, state, log) {
    this.cfg = cfg; this.state = state; this.log = log;
    const { SerialPort } = require('serialport');
    this.port = new SerialPort({ path: cfg.device, baudRate: cfg.baudRate, autoOpen: false });
    this.port.on('data', (d) => { if (d.length) this.state.ingestReply(1, d[0]); });
    this.port.on('close', () => this.state.update({ online: false }));
    this.open();
    this.timer = setInterval(() => this.poll(), 3000);
  }

  open() {
    this.port.open((err) => {
      if (err) { this.state.update({ online: false }); this.log.warn(err.message); return; }
      this.state.update({ online: true });
      this.log.info(`opened ${this.cfg.device}`);
    });
  }

  poll() {
    if (!this.port.isOpen) return this.open();
    this.port.write(Buffer.from([0x10, 0x04, 1]));
  }

  print(buf) {
    return new Promise((resolve, reject) => {
      if (!this.port.isOpen) return reject(new Error('serial not open'));
      this.port.write(buf, (e) => (e ? reject(e) : this.port.drain((e2) => (e2 ? reject(e2) : resolve()))));
    });
  }

  close() { clearInterval(this.timer); this.port.isOpen && this.port.close(); }
}

module.exports = { SerialBackend };
```

### `src/backends/file.js`

```js
'use strict';

const fs = require('fs');
const path = require('path');

/** Development sink: every job lands as a .bin you can feed to escpos-dump. */
class FileBackend {
  constructor(cfg, state, log) {
    this.cfg = cfg; this.log = log;
    fs.mkdirSync(cfg.dir, { recursive: true });
    state.update({ online: true });
  }

  async print(buf) {
    const f = path.join(this.cfg.dir, `${Date.now()}.bin`);
    fs.writeFileSync(f, buf);
    this.log.info(`wrote ${f}`);
  }

  close() {}
}

module.exports = { FileBackend };
```

---

## 12. `src/config.js`, `src/log.js`, `src/index.js`

```js
// src/config.js
'use strict';
const fs = require('fs');
const path = require('path');

function deepMerge(a, b) {
  const out = { ...a };
  for (const [k, v] of Object.entries(b || {})) {
    out[k] = v && typeof v === 'object' && !Array.isArray(v) ? deepMerge(a[k] || {}, v) : v;
  }
  return out;
}

module.exports.load = function load() {
  const base = JSON.parse(fs.readFileSync(path.join(__dirname, '../config/default.json'), 'utf8'));
  const override = process.env.BRIDGE_CONFIG && fs.existsSync(process.env.BRIDGE_CONFIG)
    ? JSON.parse(fs.readFileSync(process.env.BRIDGE_CONFIG, 'utf8'))
    : {};
  const cfg = deepMerge(base, override);
  if (process.env.BRIDGE_BACKEND) cfg.backend.type = process.env.BRIDGE_BACKEND;
  if (process.env.BRIDGE_TARGET)  cfg.backend.tcp.host = process.env.BRIDGE_TARGET;
  if (process.env.BRIDGE_LOG)     cfg.log.level = process.env.BRIDGE_LOG;
  return cfg;
};
```

```js
// src/log.js
'use strict';
const LEVELS = { debug: 10, info: 20, warn: 30, error: 40 };

module.exports.create = function create(level = 'info') {
  const min = LEVELS[level] ?? 20;
  const emit = (lvl, msg) => {
    if (LEVELS[lvl] < min) return;
    process.stdout.write(`${new Date().toISOString()} ${lvl.toUpperCase().padEnd(5)} ${msg}\n`);
  };
  return {
    debug: (m) => emit('debug', m),
    info:  (m) => emit('info', m),
    warn:  (m) => emit('warn', m),
    error: (m) => emit('error', m)
  };
};
```

```js
// src/index.js
'use strict';

const fs = require('fs');
const path = require('path');
const { load } = require('./config');
const { create: createLog } = require('./log');
const { PrinterState } = require('./escpos/status');
const { PosListener } = require('./server/posListener');
const { JobSpool } = require('./spool/jobSpool');
const backends = require('./backends');

const cfg = load();
const log = createLog(cfg.log.level);
const state = new PrinterState();
const backend = backends.create(cfg.backend, state, log);
const spool = new JobSpool({ cfg: cfg.spool, backend, log });

const listener = new PosListener({ cfg: cfg.listen, state, identity: cfg.identity, log });

listener.on('job', (buf) => spool.enqueue(buf));
listener.on('error', (e) => { log.error(`listener: ${e.message}`); process.exit(1); });

state.on('change', (s) =>
  log.info(`state: online=${s.online} cover=${s.coverOpen} paper=${s.paperEnd ? 'END' : 'ok'} err=${s.error}`));

// Optional: keep the raw upstream stream for offline analysis with escpos-dump.
if (cfg.log.rawCapture) {
  fs.mkdirSync(cfg.log.rawCaptureDir, { recursive: true });
  const f = fs.createWriteStream(path.join(cfg.log.rawCaptureDir, `${Date.now()}.raw`));
  listener.on('raw', (c) => f.write(c));
}

listener.start();
spool.pump();

for (const sig of ['SIGINT', 'SIGTERM']) {
  process.on(sig, () => {
    log.info(`${sig} — shutting down`);
    listener.stop();
    backend.close();
    process.exit(0);
  });
}
```

---

## 13. `tools/escpos-dump.js`

Decodes a raw capture into an annotated command listing. This is the tool you
use on the pcap payload from a working Simphony + TM-T88 pair.

```js
'use strict';

const fs = require('fs');
const { EscPosScanner } = require('../src/escpos/scanner');

const file = process.argv[2];
if (!file) { console.error('usage: escpos-dump <file.raw|file.bin>'); process.exit(1); }

const scanner = new EscPosScanner();
const items = scanner.push(fs.readFileSync(file));

const QNAMES = { DLE_EOT: 'DLE EOT (real-time status)', GS_I: 'GS I (printer ID)',
                 GS_r: 'GS r (transmit status)', ESC_v: 'ESC v (paper sensor)',
                 ESC_u: 'ESC u (peripheral status)' };

let jobs = 1, bytes = 0;
for (const it of items) {
  switch (it.kind) {
    case 'data':
      bytes += it.buf.length;
      console.log(`  data  ${String(it.buf.length).padStart(6)} bytes  ` +
                  `${JSON.stringify(it.buf.toString('latin1').replace(/[^\x20-\x7e]/g, '.').slice(0, 60))}`);
      break;
    case 'query':
      console.log(`  QUERY ${QNAMES[it.q] || it.q}  n=${it.n}`);
      break;
    case 'realtime':
      console.log(`  RT    cmd=0x${it.cmd.toString(16)} n=${it.n}  [${it.raw.toString('hex')}]`);
      break;
    case 'asb':
      console.log(`  ASB   GS a ${it.n}`);
      break;
    case 'cut':
      console.log(`--- end of job ${jobs++} (${bytes} bytes) ---`);
      bytes = 0;
      break;
  }
}
console.log(`\n${scanner.buf.length} trailing bytes unparsed`);
```

Extract the payload from a capture first:

```bash
sudo tcpdump -i eth0 -s0 -w simphony.pcap 'tcp port 9100'
tshark -r simphony.pcap -Y 'tcp.dstport==9100 && tcp.len>0' \
       -T fields -e data.data | tr -d '\n:' | xxd -r -p > tosimphony.raw
node tools/escpos-dump.js tosimphony.raw
```

---

## 14. `tools/fake-pos.js`

Stands in for Simphony while you develop: prints a slip and measures how fast
you answer status.

```js
'use strict';

const net = require('net');
const host = process.argv[2] || '127.0.0.1';
const port = Number(process.argv[3] || 9100);

const slip = Buffer.concat([
  Buffer.from([0x1b, 0x40]),                        // ESC @
  Buffer.from([0x1b, 0x61, 0x01]),                  // centre
  Buffer.from('TDN TEST SLIP\n\n', 'latin1'),
  Buffer.from([0x1b, 0x61, 0x00]),                  // left
  Buffer.from('1  Flat White            42.00\n', 'latin1'),
  Buffer.from('1  Croissant             35.00\n', 'latin1'),
  Buffer.from('                        ------\n', 'latin1'),
  Buffer.from('   TOTAL                 77.00\n\n\n', 'latin1'),
  Buffer.from([0x1d, 0x56, 0x42, 0x00])             // GS V 66 0 — feed and cut
]);

const s = net.connect({ host, port }, () => {
  console.log('connected');

  let n = 1, sent = 0;
  const ask = () => { sent = Date.now(); s.write(Buffer.from([0x10, 0x04, n])); };

  s.on('data', (d) => {
    console.log(`DLE EOT ${n} -> 0x${d[0].toString(16).padStart(2, '0')}  (${Date.now() - sent} ms)`);
    if (++n <= 4) return ask();
    s.write(slip, () => { console.log('slip sent'); setTimeout(() => s.end(), 500); });
  });

  ask();
});

s.on('error', (e) => console.error(e.message));
```

---

## 15. `test/scanner.test.js`

```js
'use strict';

const test = require('node:test');
const assert = require('node:assert');
const { EscPosScanner } = require('../src/escpos/scanner');

const kinds = (items) => items.map((i) => i.kind);

test('strips and reports a real-time status query', () => {
  const s = new EscPosScanner();
  const items = s.push(Buffer.from([0x41, 0x10, 0x04, 0x01, 0x42]));
  assert.deepStrictEqual(kinds(items), ['data', 'query', 'data']);
  assert.strictEqual(items[1].n, 1);
  assert.strictEqual(Buffer.concat([items[0].buf, items[2].buf]).toString(), 'AB');
});

test('does not mistake image payload for a real-time command', () => {
  // GS v 0 m=0 x=2 y=1 -> 2 payload bytes that happen to be DLE EOT
  const raster = Buffer.from([0x1d, 0x76, 0x30, 0x00, 0x02, 0x00, 0x01, 0x00, 0x10, 0x04]);
  const s = new EscPosScanner();
  const items = s.push(raster);
  assert.deepStrictEqual(kinds(items), ['data']);
  assert.strictEqual(items[0].buf.length, 10);
});

test('splits jobs on cut and keeps the cut bytes', () => {
  const s = new EscPosScanner();
  const items = s.push(Buffer.concat([
    Buffer.from('hi\n', 'latin1'),
    Buffer.from([0x1d, 0x56, 0x42, 0x00]),
    Buffer.from('next\n', 'latin1')
  ]));
  assert.deepStrictEqual(kinds(items), ['data', 'cut', 'data']);
  assert.strictEqual(items[0].buf.length, 3 + 4);
});

test('holds a command that straddles a chunk boundary', () => {
  const s = new EscPosScanner();
  assert.deepStrictEqual(kinds(s.push(Buffer.from([0x10, 0x04]))), []);
  assert.deepStrictEqual(kinds(s.push(Buffer.from([0x02]))), ['query']);
});
```

---

## 16. `systemd/simphony-bridge.service`

```ini
[Unit]
Description=Simphony virtual slip printer bridge
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=bridge
Group=lp
ExecStart=/usr/bin/node /opt/simphony-bridge/src/index.js
Environment=NODE_ENV=production
Environment=BRIDGE_CONFIG=/etc/simphony-bridge/config.json
WorkingDirectory=/opt/simphony-bridge
StateDirectory=simphony-bridge
Restart=always
RestartSec=2
# Needed only if you bind :9100 as a non-root user on a locked-down kernel
AmbientCapabilities=CAP_NET_BIND_SERVICE
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

`Group=lp` gives access to `/dev/usb/lp0`. For CDC-ACM add `SupplementaryGroups=dialout`.

---

## 17. Simphony-side configuration

In EMC: **Property → Setup → Printers**, insert a record, then set

| Field | Value |
|---|---|
| Workstation | the workstation acting as print controller |
| Printer Type | IP Printer |
| Address | the bridge's IP (static or DHCP reservation) |
| Port | 9100 |
| Thermal printer | ticked |
| Center Logo Using | 3 1/8" (80 mm) if logo printing is on |

Then assign it as an Order Device or receipt printer as usual. Nothing else in
Simphony needs to know the bridge is not a printer.

---

## 18. Bring-up order

1. **Capture first.** Tap a live Simphony workstation talking to a real TM-T88
   Ethernet printer. Record idle, print, paper-out, cover-open and recovery.
   Run `escpos-dump` on it. That capture, not this scaffold, is your spec.
2. Run the bridge on a laptop with `backend.type = "file"`. Point an EMC IP
   Printer at it. Confirm a slip arrives and the workstation reports no error.
3. Diff your captured jobs against jobs the bridge relayed — they should be
   byte-identical apart from stripped status queries.
4. Switch to `tcp` backend into a real printer. Pull the paper. Confirm
   Simphony surfaces the error and fails over to its backup printer.
5. Switch to `usblp`. Check `lsusb -v` first: printer class 0x07 goes to
   `usblp`, CDC-ACM goes to `serial`.
6. Power-cut test mid-job. The spool should replay on boot with no duplicate
   and no loss.
7. Only then move to the custom board.

## 19. Porting notes for the production board

The whole hot path is `net` plus a file descriptor, so this runs unchanged on
any Linux target. If you later move to bare metal (i.MX RT1062 / STM32H5), the
pieces that port directly are `constants.js`, `scanner.js` and `status.js` —
they are pure byte logic with no I/O and translate to C almost line for line.
The parts you would rewrite are the listener (lwIP raw or socket API), the spool
(a flash FIFO instead of a directory), and the USB host, which is the reason to
stay on Linux unless boot time forces the issue.
