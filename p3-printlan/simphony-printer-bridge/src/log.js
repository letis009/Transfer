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
