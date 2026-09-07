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
