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
