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
