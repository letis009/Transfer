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
