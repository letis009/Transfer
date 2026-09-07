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
