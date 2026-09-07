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
