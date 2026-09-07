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
