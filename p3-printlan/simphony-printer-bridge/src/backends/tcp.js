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
