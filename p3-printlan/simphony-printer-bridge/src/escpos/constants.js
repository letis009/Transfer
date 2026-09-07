'use strict';

// Control bytes
const ESC = 0x1b, GS = 0x1d, FS = 0x1c, DLE = 0x10;
const EOT = 0x04, ENQ = 0x05, DC4 = 0x14;

const NEED_MORE = -1;   // command straddles the end of the buffer
const UNKNOWN   = -2;   // not a command we model: treat the byte as data

// --- fixed-length commands ---------------------------------------------
// Values are TOTAL length including the ESC/GS/FS prefix.

const ESC_FIXED = {
  0x0c: 2,  // ESC FF     print page (page mode)
  0x20: 3,  // ESC SP n   right-side character spacing
  0x21: 3,  // ESC ! n    print mode
  0x24: 4,  // ESC $      absolute position
  0x25: 3,  // ESC % n    select user-defined char set
  0x2d: 3,  // ESC - n    underline
  0x32: 2,  // ESC 2      default line spacing
  0x33: 3,  // ESC 3 n    line spacing
  0x3d: 3,  // ESC = n    peripheral device select
  0x3f: 3,  // ESC ? n    cancel user-defined char
  0x40: 2,  // ESC @      initialise
  0x45: 3,  // ESC E n    emphasised
  0x47: 3,  // ESC G n    double strike
  0x4a: 3,  // ESC J n    feed n dots
  0x4b: 3,  // ESC K n    reverse feed
  0x4c: 2,  // ESC L      page mode
  0x4d: 3,  // ESC M n    font
  0x52: 3,  // ESC R n    international char set
  0x53: 2,  // ESC S      standard mode
  0x54: 3,  // ESC T n    page-mode direction
  0x56: 3,  // ESC V n    90deg rotation
  0x57: 10, // ESC W      print area (8 params)
  0x5c: 4,  // ESC \      relative position
  0x61: 3,  // ESC a n    justification
  0x64: 3,  // ESC d n    feed n lines
  0x69: 2,  // ESC i      full cut (legacy)
  0x6d: 2,  // ESC m      partial cut (legacy)
  0x70: 5,  // ESC p m t1 t2   drawer pulse
  0x72: 3,  // ESC r n    print colour
  0x74: 3,  // ESC t n    code page
  0x75: 3,  // ESC u n    transmit peripheral status  -> RESPONSE
  0x76: 2,  // ESC v      transmit paper sensor       -> RESPONSE
  0x7b: 3   // ESC { n    upside-down
};

const GS_FIXED = {
  0x21: 3,  // GS ! n     character size
  0x24: 4,  // GS $       absolute vertical position
  0x2f: 3,  // GS / m     print downloaded bit image
  0x3a: 2,  // GS :       start/end macro
  0x42: 3,  // GS B n     white/black reverse
  0x45: 3,  // GS E n     head control
  0x48: 3,  // GS H n     HRI position
  0x49: 3,  // GS I n     transmit printer ID          -> RESPONSE
  0x4c: 4,  // GS L       left margin
  0x50: 4,  // GS P x y   motion units
  0x54: 3,  // GS T n     line position
  0x57: 4,  // GS W       print area width
  0x5c: 4,  // GS \       relative vertical position
  0x5e: 5,  // GS ^ r t m execute macro
  0x61: 3,  // GS a n     enable ASB                   -> INTERCEPT
  0x62: 3,  // GS b n     smoothing
  0x66: 3,  // GS f n     HRI font
  0x68: 3,  // GS h n     barcode height
  0x72: 3,  // GS r n     transmit status              -> RESPONSE
  0x77: 3   // GS w n     barcode width
};

const FS_FIXED = {
  0x21: 3,  // FS ! n
  0x26: 2,  // FS &   select kanji
  0x2d: 3,  // FS - n
  0x2e: 2,  // FS .   cancel kanji
  0x43: 3,  // FS C n kanji code system
  0x53: 4,  // FS S   kanji spacing
  0x57: 3,  // FS W n kanji double size
  0x70: 4   // FS p n m  print NV bit image
};

// Commands that must never be forwarded downstream: we answer them ourselves
// from the cached PrinterState so the upstream socket is never blocked.
const QUERY = { GS_I: 'GS_I', GS_r: 'GS_r', ESC_v: 'ESC_v', ESC_u: 'ESC_u', DLE_EOT: 'DLE_EOT' };

module.exports = { ESC, GS, FS, DLE, EOT, ENQ, DC4, NEED_MORE, UNKNOWN,
                   ESC_FIXED, GS_FIXED, FS_FIXED, QUERY };
