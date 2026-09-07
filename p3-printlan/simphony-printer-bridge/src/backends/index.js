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
