'use strict';
/* NOISEFLIP v5: the UI. Talks to noiseflip.exe on this machine only (see ui_server.h). Every change is saved at once. */

// ------------------------------------------------------------------ helpers
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
function h(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  for (const k in attrs || {}) {
    const v = attrs[k];
    if (k === 'class') e.className = v;
    else if (k === 'text') e.textContent = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (v === true) e.setAttribute(k, '');
    else if (v !== false && v != null) e.setAttribute(k, v);
  }
  for (const c of kids.flat(2)) { if (c == null || c === false) continue; e.append(c.nodeType ? c : document.createTextNode(c)); }
  return e;
}
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const NOTE = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const noteName = n => (n >= 0 && n <= 127) ? NOTE[n % 12] + (Math.floor(n / 12) - 2) : '?';
const pad2 = n => String(n).padStart(2, '0');
const isNum = v => v !== '' && v != null && !isNaN(Number(v));

const store = { get(k, d) { try { const v = localStorage.getItem(k); return v == null ? d : JSON.parse(v); } catch (e) { return d; } },
                set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch (e) {} } };

// ------------------------------------------------------------------ what exists
const TRIG = [
  { sec: 'jump', name: 'OLLIE/IN AIR', d: 'Ground to air.' },
  { sec: 'land', name: 'LAND', d: 'Air to ground. Velocity follows the drop height.' },
  { sec: 'bail', name: 'BAIL/ON FOOT', d: 'Into the bail state, then disarmed until the skater has ridden for a while.' },
  { sec: 'grind', name: 'GRIND', d: 'Physics state in the grind list: grinds and slides are one trigger.' },
  { sec: 'manual', name: 'MANUAL', d: 'On the ground with the stick in the manual range (manual and nose manual).' },
  { sec: 'grab', name: 'GRAB', d: 'LT or RT, on the ground or in the air, not in a manual or grind.' },
  { sec: 'push', name: 'PUSH', d: 'A or X on the ground, not together with a grab.' },
  { sec: 'footplant', name: 'FOOTPLANT', d: 'Grab plus A or X.' },
  { sec: 'nofootgrab', name: 'NOFOOTGRAB', d: 'Grab plus B.' },
  { sec: 'rot_x', name: 'ROT X', d: 'Board rotation about world X.', rot: true },
  { sec: 'rot_y', name: 'ROT Y', d: 'Board rotation about the vertical axis: a shuvit in the air, a revert on the ground.', rot: true },
  { sec: 'rot_z', name: 'ROT Z', d: 'Board rotation about world Z: a flip is mostly this.', rot: true },
  { sec: 'fingerflip', name: 'FINGERFLIP', d: 'Grab held plus the board rotating in the air.', rot: true, fing: true },
];
const CCS = [
  { sec: 'cc_intensity', name: 'INTENSITY', d: 'Energy meter: every note adds energy, it falls back over time.' },
  { sec: 'cc_rot_x', name: 'ROT X', d: 'Net board spin about X, kept as a fader.' },
  { sec: 'cc_rot_y', name: 'ROT Y', d: 'Net board spin about Y (shuvits, reverts), kept as a fader.' },
  { sec: 'cc_rot_z', name: 'ROT Z', d: 'Net board spin about Z (flips), kept as a fader.' },
  { sec: 'cc_manual', name: 'MANUAL', d: 'The manual stick position, scaled to 0-127.' },
  { sec: 'cc_speed', name: 'SPEED', d: 'The board speed, scaled to 0-127.' },
];
const BTN = ['A', 'B', 'X', 'Y', 'LB', 'RB', 'LS', 'RS', 'START', 'BACK', 'DPAD_UP', 'DPAD_DOWN', 'DPAD_LEFT', 'DPAD_RIGHT'];
const CTX = ['air', 'ground_or_air', 'any'];

const G = (k, l, t, o = {}) => Object.assign({ k, l, t }, o);
const GLOBAL = [
  { id: 'general', sec: 'general', title: 'General', open: true, fields: [
    G('channel', 'MIDI channel', 'int', { min: 1, max: 16, h: 'The channel every note and controller uses unless it has its own (Mapping tab).' }),
    G('velocity', 'Velocity', 'int', { min: 1, max: 127, h: 'Used by every trigger unless it sets its own.' }),
    G('speed_bonus', 'Speed bonus', 'int', { min: 0, max: 126, h: 'Extra velocity at top speed. 0 = off.' }),
    G('speed_min', 'Speed min (m/s)', 'num', { min: 0, max: 30, step: 0.1, h: 'At or below this the bonus is 0.' }),
    G('speed_max', 'Speed max (m/s)', 'num', { min: 0, max: 30, step: 0.1, h: 'At or above this the bonus is full.' }),
    G('hold_ms', 'Pulse length (ms)', 'int', { min: 1, max: 60000, smax: 1500, h: 'Length of every pulse note unless it sets its own.' }),
    G('note_off', 'Note-off style', 'sel', { opts: ['noteoff', 'velocity0'], h: 'noteoff = a real note-off. velocity0 = a note-on with velocity 0.' }),
    G('midi_port', 'MIDI port', 'port', { restart: true, h: 'The output port whose name contains this text.' }),
    G('poll_ms', 'Poll (ms)', 'int', { min: 0, max: 100, restart: true, h: '0 = as fast as possible (a full CPU core).' }),
    G('midi_check', 'Foreign MIDI check', 'bool', { restart: true, h: 'Listen to the port and warn about notes Noiseflip did not send.' }),
  ] },
  { id: 'input', sec: 'input', title: 'Gamepad input', fields: [
    G('trigger_press', 'Trigger press', 'int', { min: 1, max: 255, h: 'LT/RT value that counts as pressed.' }),
    G('trigger_release', 'Trigger release', 'int', { min: 0, max: 254, h: 'It counts as released below this.' }),
    G('grab_lt', 'Grab with LT', 'bool'), G('grab_rt', 'Grab with RT', 'bool'),
    G('push_block_with_grab', 'Grab blocks push', 'bool', { h: 'A press that is part of grab + button is not a push.' }),
  ] },
  { id: 'manual_stick', sec: 'manual_stick', title: 'Manual stick', fields: [
    G('direction', 'Direction', 'sel', { opts: ['both', 'up', 'down'], h: 'both = manual and nose manual are one trigger.' }),
    G('min', 'Range min', 'int', { min: 0, max: 32767 }), G('max', 'Range max', 'int', { min: 0, max: 32767 }),
    G('hysteresis', 'Hysteresis', 'int', { min: 0, max: 32767, smax: 8000, h: 'Once inside, it stays on until this far outside.' }),
    G('cross_axis_max', 'Cross axis max', 'int', { min: 0, max: 32767, h: '32767 = no limit.' }),
  ] },
  { id: 'land_velocity', sec: 'land_velocity', title: 'Landing velocity', fields: [
    G('enabled', 'Follow the drop', 'bool', { h: 'The land note velocity follows how far the skater fell.' }),
    G('min_velocity', 'Quietest', 'int', { min: 1, max: 127 }), G('max_velocity', 'Hardest', 'int', { min: 1, max: 127 }),
    G('min_height', 'Min drop (m)', 'num', { min: 0, max: 10, step: 0.05 }), G('max_height', 'Max drop (m)', 'num', { min: 0, max: 10, step: 0.05 }),
    G('curve', 'Curve', 'num', { min: 0.05, max: 4, step: 0.05, h: '1 = straight. Below 1 small drops are louder.' }),
  ] },
  { id: 'rotation', sec: 'rotation', title: 'Rotation (shared)', fields: [
    G('detect', 'Detect', 'sel', { opts: ['angle', 'rate'], h: 'angle = a note every step_deg degrees. rate = a note when the spin passes a threshold.' }),
    G('switch_button', 'Switch button', 'sel', { opts: ['none', ...BTN], h: 'Press it while playing to flip between angle and rate.' }),
    G('step_deg', 'Step (deg)', 'num', { min: 10, max: 720, step: 10 }),
    G('min_rate', 'Min rate (deg/s)', 'int', { min: 1, max: 1000, h: 'Slower than this is not a rotation.' }),
    G('ground_min_rate', 'Ground min rate', 'int', { min: 1, max: 1500, h: 'On the ground only quick turns count (a revert, not carving).' }),
    G('idle_reset_ms', 'Idle reset (ms)', 'int', { min: 0, max: 2000 }),
    G('hold_ms', 'Note length (ms)', 'int', { min: 1, max: 1000, smax: 500, h: 'Angle mode: each note; the next step cuts it.' }),
    G('note_step', 'Note step', 'int', { min: -48, max: 48, h: 'Semitones added per step inside one trick. 0 = same note.' }),
  ] },
];

// A description for every setting. A field's own `h` wins; otherwise this is looked up by "section.key", then by key.
const HELP = {
  enabled: 'Switch it off and it never sends anything.',
  velocity: 'How loud the note is. DEFAULT uses the general velocity from GLOBAL.',
  speed_bonus: 'Extra velocity added at top speed. DEFAULT uses the global bonus; 0 turns it off for this one.',
  hold_ms: 'How long a pulse note lasts. DEFAULT uses the global pulse length.',
  min_hold_ms: 'The shortest a gate note may last, even if its condition ends sooner.',
  context: 'Where the rotation counts: only in the air, on the ground or in the air, or anywhere.',
  release: 'Rate mode: the rotation counts as over when its speed falls below this.',
  tail_gate: 'Rate mode: how long the speed must stay below release before the note ends.',
  tail_pulse: 'Rate mode: how long the speed must stay low before this trigger can fire again.',
  step_deg: 'Angle mode: degrees the board turns per note. On a trigger, DEFAULT follows the shared rotation setting.',
  axes: 'Which board axes count for this trigger.',
  process: 'The name of the game program Noiseflip reads (Skate.exe).',
  ground: 'Physics state numbers that count as riding on the ground.',
  air: 'Physics state numbers that count as being in the air.',
  grind: 'Physics state numbers that count as a grind or slide.',
  bail: 'Physics state numbers that count as a bail (off the board).',
  grab_lt: 'The left trigger can start a grab.',
  grab_rt: 'The right trigger can start a grab.',
  push_button: 'Buttons that count as a push. Either foot works when both are on.',
  axis: 'Which stick axis drives the manual.',
  min: 'The lowest stick value that counts as a manual (raw XInput, 0 to 32767).',
  max: 'The highest stick value that counts as a manual (raw XInput, 0 to 32767).',
  min_velocity: 'The velocity of the quietest landing, for example off a curb.',
  max_velocity: 'The velocity of the hardest landing, a big drop.',
  min_height: 'A drop this small or smaller gives the quietest landing.',
  max_height: 'A drop this big or bigger gives the hardest landing.',
  idle_reset_ms: 'Angle mode: after this long without rotation the counted angle starts again from 0.',
  max_rate_hz: 'Never send more than this many messages per second.',
  min_value: 'The value sent when there is no energy.',
  max_value: 'The value sent at full energy.',
  curve: 'Bends the response. 1 is straight; below 1 it reacts early, above 1 only near the top.',
  grind_per_s: 'Energy added every second while a grind is held.',
  manual_per_s: 'Energy added every second while a manual is held.',
  grab_per_s: 'Energy added every second while a grab is held.',
  min_rate: 'Spin slower than this (deg/s) is ignored, so ollie pitch and drift do not count.',
  ground_min_rate: 'On the ground only turns faster than this (deg/s) count: a revert, not carving.',
  start: 'The value the fader starts at and drifts back to.',
  idle: 'The value sent when there is no manual. AUTO is 0 (64 in signed mode).',
};
function helpFor(sec, f) {
  if (sec === 'cc_intensity' && TRIG.some(t => t.sec === f.k)) return 'Energy added by every ' + f.l + ' note (0 to 1; negative takes energy away).';
  if (f.k === 'tail_ms') return (String(val(sec, 'mode')).toLowerCase() === 'gate') ? HELP.tail_gate : HELP.tail_pulse;
  return HELP[sec + '.' + f.k] ?? HELP[f.k] ?? '';
}

function trigFields(t) {
  const c = CFG[t.sec] || {};
  const gate = (c.mode || '').toLowerCase() === 'gate';
  const f = [
    G('mode', 'Mode', 'seg', { opts: ['pulse', 'gate'], re: true, h: gate ? 'Held while the condition lasts.' : 'A fixed-length note when it starts.' }),
    G('velocity', 'Velocity', 'oint', { min: 1, max: 127, dflt: 'default', seed: 80 }),
    G('speed_bonus', 'Speed bonus', 'oint', { min: 0, max: 126, dflt: 'default', seed: 20 }),
    G('delay_ms', 'Delay (ms)', 'int', { min: 0, max: 60000, smax: 1000, h: gate ? 'The condition must hold this long first.' : 'Wait this long after the event.' }),
  ];
  if (!gate) f.push(G('hold_ms', 'Pulse length (ms)', 'oint', { min: 1, max: 60000, smax: 1500, dflt: 'default', seed: 300 }));
  if (!t.rot) {
    if (gate) f.push(G('off_delay_ms', 'Off delay (ms)', 'int', { min: 0, max: 60000, smax: 1000, h: 'The condition must stay false this long before the note ends.' }));
  } else {
    if (gate) f.push(G('min_hold_ms', 'Min length (ms)', 'int', { min: 0, max: 60000, smax: 1000 }));
    f.push(G('context', 'Where it counts', 'sel', { opts: CTX }));
    f.push(G('detect', 'Detect', 'sel', { opts: ['default', 'rate', 'angle'], h: 'default follows the shared rotation setting.' }));
    f.push(G('threshold', 'Rate: threshold (deg/s)', 'int', { min: 1, max: 3000, h: 'Rate mode: a trick is about 900-1200, an ollie much less.' }));
    f.push(G('release', 'Rate: release (deg/s)', 'int', { min: 1, max: 3000 }));
    f.push(G('tail_ms', gate ? 'Rate: tail (ms)' : 'Rate: re-arm (ms)', 'int', { min: 0, max: 60000, smax: 1000 }));
    f.push(G('step_deg', 'Angle: step (deg)', 'onum', { min: 10, max: 720, step: 10, dflt: 'default', seed: 180 }));
    f.push(G('note_step', 'Angle: note step', 'oint', { min: -48, max: 48, dflt: 'default', seed: 2, h: 'Semitones per step inside one trick.' }));
    if (t.fing) f.push(G('axes', 'Axes', 'axes'));
  }
  if (gate && !t.rot) f.push(G('min_hold_ms', 'Min length (ms)', 'int', { min: 0, max: 60000, smax: 1000 }));
  return f;
}

const CC_COMMON = [
  G('smooth_ms', 'Smoothing (ms)', 'num', { min: 0, max: 2000, step: 5, h: '0 = none. Higher = softer, slower.' }),
  G('min_step', 'Min step', 'int', { min: 1, max: 127, h: 'Ignore moves smaller than this (a settled value is still sent).' }),
  G('max_rate_hz', 'Max rate (msg/s)', 'int', { min: 1, max: 1000, smax: 120 }),
];
function ccFields(c) {
  const f = [G('enabled', 'Enabled', 'bool'), ...CC_COMMON];
  if (c.sec === 'cc_intensity') f.push(
    G('decay_s', 'Fall time (s)', 'num', { min: 0, max: 60, step: 0.5, h: 'The energy falls to 37% in this time. 0 = never.' }),
    G('min_value', 'Min value', 'int', { min: 0, max: 127 }), G('max_value', 'Max value', 'int', { min: 0, max: 127 }),
    G('curve', 'Curve', 'num', { min: 0.05, max: 4, step: 0.05 }),
    G('land_scaled', 'Landing scaled', 'bool', { h: 'A landing adds energy in proportion to how hard it was.' }),
    G('grind_per_s', 'Grind per second', 'num', { min: -1, max: 1, step: 0.01 }),
    G('manual_per_s', 'Manual per second', 'num', { min: -1, max: 1, step: 0.01 }),
    G('grab_per_s', 'Grab per second', 'num', { min: -1, max: 1, step: 0.01 }),
    G('speed_per_s', 'Speed per second', 'num', { min: -1, max: 1, step: 0.01, h: 'At top speed. 0 = speed does not count.' }));
  else if (c.sec === 'cc_manual') f.push(
    G('only_while_manual', 'Only while manual', 'bool', { h: 'Only while the manual note sounds.' }),
    G('mode', 'Mode', 'sel', { opts: ['magnitude', 'signed'], h: 'signed: manual up 64-127, nose manual down 0-63.' }),
    G('idle', 'Idle value', 'oint', { min: 0, max: 127, dflt: 'auto', seed: 0 }));
  else if (c.sec === 'cc_speed') f.push(
    G('min_speed', 'Min speed (m/s)', 'num', { min: 0, max: 40, step: 0.1, h: 'At or below this the value is 0.' }),
    G('max_speed', 'Max speed (m/s)', 'num', { min: 0, max: 40, step: 0.1, h: 'At or above this the value is 127.' }),
    G('curve', 'Curve', 'num', { min: 0.05, max: 4, step: 0.05 }),
    G('invert', 'Invert', 'bool', { h: 'Faster = lower.' }));
  else f.push(
    G('range_deg', 'Range (deg)', 'num', { min: 10, max: 20000, step: 10, h: 'This much net spin = the full 0-127.' }),
    G('min_rate', 'Min rate (deg/s)', 'int', { min: 0, max: 1000 }), G('ground_min_rate', 'Ground min rate', 'int', { min: 0, max: 1500 }),
    G('context', 'Where it counts', 'sel', { opts: CTX }),
    G('invert', 'Invert', 'bool', { h: 'The other direction raises it.' }),
    G('start', 'Start value', 'int', { min: 0, max: 127 }),
    G('decay_per_s', 'Drift back (units/s)', 'num', { min: 0, max: 127, step: 1, h: '0 = it stays where it is.' }));
  return f;
}

// ------------------------------------------------------------------ state
let CFG = {}, DEF = {}, LIVE = null, SEQ = 0, TAB = store.get('nf_tab', 'global'), PORTS = [];
let OPEN = store.get('nf_open', {});
const LOG = [];
let lost = 0;

function parseIni(t) {
  const o = {}; let sec = null;
  for (let line of t.split(/\r?\n/)) {
    line = line.replace(/(^|\s)[;#].*$/, '').trim();
    if (!line) continue;
    if (line[0] === '[') { sec = line.slice(1, line.indexOf(']')).trim().toLowerCase(); o[sec] = o[sec] || {}; continue; }
    const i = line.indexOf('=');
    if (i < 0 || !sec) continue;
    o[sec][line.slice(0, i).trim().toLowerCase()] = line.slice(i + 1).trim();
  }
  return o;
}
const same = (a, b) => (isNum(a) && isNum(b)) ? Number(a) === Number(b) : String(a ?? '').toLowerCase().replace(/\s+/g, '') === String(b ?? '').toLowerCase().replace(/\s+/g, '');
const val = (sec, k) => (CFG[sec] || {})[k];

// ------------------------------------------------------------------ talking to the program
async function api(path, body) {
  const opt = body == null ? {} : { method: 'POST', headers: { 'X-Noiseflip': '1' }, body };
  const r = await fetch(path, opt);
  const ct = r.headers.get('content-type') || '';
  return ct.includes('json') ? r.json() : r.text();
}
let saveTimer = 0;
function status(text, kind) {
  const e = $('#save'); if (!e) return;
  e.textContent = text; e.dataset.kind = kind || '';
}
function toast(msg) {
  const t = $('#toast'); t.textContent = msg; t.classList.add('on');
  clearTimeout(toast.t); toast.t = setTimeout(() => t.classList.remove('on'), 5200);
}
async function save(items) {
  status('SAVING...', 'busy');
  try {
    const r = await api('/api/set', items.map(([s, k, v]) => `${s}|${k}|${v}`).join('\n'));
    if (r && r.ok) {
      for (const [s, k, v] of items) { (CFG[s] = CFG[s] || {})[k] = String(v); }
      status('SAVED ' + new Date().toTimeString().slice(0, 8), 'ok');
      return true;
    }
    toast('NOT SAVED: ' + (r && r.error ? r.error : 'unknown error'));
    status('NOT SAVED', 'bad');
  } catch (e) { toast('NOT SAVED: the program does not answer'); status('NOT SAVED', 'bad'); }
  await loadConfig();
  render();
  return false;
}
async function loadConfig() { CFG = parseIni(await api('/api/config')); }

// ------------------------------------------------------------------ controls
function normalise(f, v) {
  if (f.t === 'int' || f.t === 'num' || f.t === 'oint' || f.t === 'onum') {
    if (!isNum(v)) return null;
    let n = Number(v);
    if (f.t === 'int' || f.t === 'oint') n = Math.round(n);
    return clamp(n, f.min ?? -1e9, f.max ?? 1e9);
  }
  return v;
}
function numberPair(f, cur, commit, locked) {
  const step = f.step || 1;
  const smax = f.smax ?? f.max;
  const box = h('input', { type: 'number', class: 'num', min: f.min, max: f.max, step: f.t.endsWith('num') ? 'any' : 1, value: cur, disabled: locked });
  const sl = h('input', { type: 'range', class: 'rng', min: f.min, max: smax, step, value: clamp(Number(cur) || 0, f.min, smax), disabled: locked });
  const paint = () => sl.style.setProperty('--p', ((sl.value - sl.min) / ((sl.max - sl.min) || 1) * 100) + '%');
  paint();
  sl.addEventListener('input', () => { box.value = sl.value; paint(); });
  sl.addEventListener('change', () => commit(Number(sl.value)));
  box.addEventListener('change', () => {
    const n = normalise(f, box.value);
    if (n == null) { box.value = cur; return; }
    box.value = n; sl.value = clamp(n, f.min, smax); paint(); commit(n);
  });
  return h('div', { class: 'pair' }, sl, box);
}
function seg(opts, cur, commit, cls) {
  const wrap = h('div', { class: 'seg ' + (cls || '') });
  for (const o of opts) {
    wrap.append(h('button', { type: 'button', class: same(o, cur) ? 'on' : '', text: String(o).toUpperCase(), onclick: () => {
      if (same(o, cur)) return;
      $$('button', wrap).forEach(b => b.classList.toggle('on', b.textContent === String(o).toUpperCase()));
      cur = o; commit(o);
    } }));
  }
  return wrap;
}
function control(sec, f, cur, commit) {
  switch (f.t) {
    case 'int': case 'num': return numberPair(f, cur, commit);
    case 'oint': case 'onum': {
      const isDef = same(cur, f.dflt);
      const wrap = h('div', { class: 'opt' });
      const toggle = h('button', { type: 'button', class: 'defbtn' + (isDef ? ' on' : ''), text: String(f.dflt).toUpperCase(),
        title: isDef ? 'Click to set your own value' : 'Click to use the default',
        onclick: () => commit(isDef ? clamp(f.seed ?? f.min, f.min, f.max) : f.dflt, true) });
      wrap.append(toggle, numberPair(f, isDef ? '' : cur, commit, isDef));
      return wrap;
    }
    case 'bool': {
      const on = String(cur).toLowerCase() === 'true';
      const b = h('button', { type: 'button', class: 'tog' + (on ? ' on' : ''), role: 'switch', 'aria-checked': on, onclick: () => {
        const n = !b.classList.contains('on'); b.classList.toggle('on', n); b.setAttribute('aria-checked', n); commit(n);
      } }, h('i'), h('span', { text: on ? 'ON' : 'OFF' }));
      return b;
    }
    case 'sel': case 'seg':
      return f.opts.length <= 5 ? seg(f.opts, cur, commit)
        : h('select', { class: 'sel', onchange: e => commit(e.target.value) }, f.opts.map(o => h('option', { value: o, selected: same(o, cur) }, String(o).toUpperCase())));
    case 'btns': {
      const set = new Set(String(cur).split(/[\s,+|]+/).filter(Boolean).map(s => s.toUpperCase()));
      const wrap = h('div', { class: 'chips' });
      for (const b of BTN) wrap.append(h('button', { type: 'button', class: 'chip' + (set.has(b) ? ' on' : ''), text: b.replace('DPAD_', 'D-'), onclick: e => {
        if (set.has(b) && set.size === 1) return;
        set.has(b) ? set.delete(b) : set.add(b);
        e.currentTarget.classList.toggle('on', set.has(b));
        commit([...set].join(', '));
      } }));
      return wrap;
    }
    case 'axes': {
      const set = new Set(String(cur).split(/[\s,+]+/).filter(Boolean).map(s => s.toLowerCase()));
      const wrap = h('div', { class: 'chips' });
      for (const a of ['x', 'y', 'z']) wrap.append(h('button', { type: 'button', class: 'chip' + (set.has(a) ? ' on' : ''), text: a.toUpperCase(), onclick: e => {
        if (set.has(a) && set.size === 1) return;
        set.has(a) ? set.delete(a) : set.add(a);
        e.currentTarget.classList.toggle('on', set.has(a));
        commit(['x', 'y', 'z'].filter(x => set.has(x)).join(', '));
      } }));
      return wrap;
    }
    case 'port': {
      const inp = h('input', { type: 'text', class: 'txt', value: cur, list: 'ports', placeholder: 'any port', spellcheck: 'false', onchange: e => commit(e.target.value.trim()) });
      return inp;
    }
    default:
      return h('input', { type: 'text', class: 'txt', value: cur, placeholder: 'none', spellcheck: 'false', onchange: e => commit(e.target.value.trim()) });
  }
}

function field(sec, f) {
  const cur = val(sec, f.k);
  if (cur === undefined) return null;
  const row = h('div', { class: 'f f-' + f.t });
  const modified = DEF[sec] && DEF[sec][f.k] !== undefined && !same(cur, DEF[sec][f.k]);
  row.classList.toggle('mod', !!modified);
  const commit = async (v, rerender) => {
    let out = v;
    if (f.t === 'int' || f.t === 'num' || f.t === 'oint' || f.t === 'onum') out = (typeof v === 'number') ? String(v) : v;
    const ok = await save([[sec, f.k, out]]);
    if (ok && (f.re || rerender)) render();
    else if (ok) row.classList.toggle('mod', !!(DEF[sec] && DEF[sec][f.k] !== undefined && !same(out, DEF[sec][f.k])));
  };
  const lab = h('label', { class: 'fl' }, h('span', { text: f.l }), f.restart ? h('em', { class: 'tag', text: 'RESTART' }) : null);
  const rs = h('button', { type: 'button', class: 'rs', title: 'Back to the default: ' + (DEF[sec] ? DEF[sec][f.k] : ''), text: '\u21BA', onclick: async () => {
    if (await save([[sec, f.k, DEF[sec][f.k]]])) render();
  } });
  const help = f.h ?? helpFor(sec, f);
  row.append(h('div', { class: 'fh' }, lab, rs), control(sec, f, cur, commit), help ? h('p', { class: 'help', text: help }) : null);
  return row;
}

function fold(id, num, title, o) {
  const isOpen = OPEN[id] ?? !!o.open;
  const root = h('section', { class: 'fold' + (isOpen ? ' open' : '') + (o.off ? ' off' : ''), 'data-fold': id });
  const body = h('div', { class: 'fold-body' });
  const toggle = () => {
    const now = root.classList.toggle('open');
    OPEN[id] = now; store.set('nf_open', OPEN);
    if (now && !body.firstChild) body.append(o.build());
  };
  const head = h('div', { class: 'fold-head', tabindex: 0, role: 'button', onclick: toggle, onkeydown: e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); toggle(); } } },
    h('span', { class: 'idx', text: num }), o.led != null ? h('i', { class: 'led', 'data-led': o.led }) : null,
    h('h3', { text: title }), h('span', { class: 'badges' }, o.badges || []), h('span', { class: 'sp' }), o.right || null, h('span', { class: 'car', text: '+' }));
  root.append(head, body);
  if (isOpen) body.append(o.build());
  return root;
}
function resetBar(sec, fields) {
  return h('div', { class: 'foldfoot' }, h('button', { type: 'button', class: 'btn ghost', text: 'RESET THIS SECTION TO DEFAULTS', onclick: async () => {
    const items = fields.filter(f => DEF[sec] && DEF[sec][f.k] !== undefined && val(sec, f.k) !== undefined).map(f => [sec, f.k, DEF[sec][f.k]]);
    if (await save(items)) render();
  } }));
}
const grid = (sec, fields) => h('div', { class: 'fgrid' }, fields.map(f => field(sec, f)));

// ------------------------------------------------------------------ tabs
function headBlock(n, title, sub) {
  return h('div', { class: 'pagehead' }, h('b', { class: 'ghost-num', text: n }), h('div', {}, h('h2', { text: title }), h('p', { text: sub })));
}
function tabGlobal() {
  const w = h('div', { class: 'stack' }, headBlock('01', 'GLOBAL', 'Shared settings: MIDI, gamepad, manual stick, landing velocity, rotation.'));
  GLOBAL.forEach((g, i) => w.append(fold('g:' + g.id, pad2(i + 1), g.title, { open: g.open, build: () => h('div', {}, grid(g.sec, g.fields), resetBar(g.sec, g.fields)) })));
  return w;
}
function tabTriggers() {
  const w = h('div', { class: 'stack' }, headBlock('02', 'TRIGGERS', 'Thirteen triggers. Each one is a pulse (a fixed-length note) or a gate (held while it lasts). Notes and channels live in MAPPING.'));
  TRIG.forEach((t, i) => {
    const c = CFG[t.sec] || {};
    const gate = (c.mode || '').toLowerCase() === 'gate';
    const on = String(c.enabled).toLowerCase() === 'true';
    const quick = h('button', { type: 'button', class: 'tog sm' + (on ? ' on' : ''), title: 'Enable / disable', onclick: async e => {
      e.stopPropagation();
      const n = !quick.classList.contains('on');
      quick.classList.toggle('on', n); quick.firstChild.nextSibling.textContent = n ? 'ON' : 'OFF';
      $(`[data-fold="t:${t.sec}"]`).classList.toggle('off', !n);
      if (await save([[t.sec, 'enabled', n]])) { const cb = $(`[data-fold="t:${t.sec}"] .f-bool .tog`); if (cb) { cb.classList.toggle('on', n); cb.lastChild.textContent = n ? 'ON' : 'OFF'; } }
    } }, h('i'), h('span', { text: on ? 'ON' : 'OFF' }));
    const fields = trigFields(t);
    w.append(fold('t:' + t.sec, pad2(i + 1), t.name, {
      off: !on, led: 't' + i,
      badges: [h('em', { class: 'badge ' + (gate ? 'gate' : 'pulse'), text: gate ? 'GATE' : 'PULSE' }), h('em', { class: 'badge note', text: isNum(c.note) ? 'N' + c.note + ' ' + noteName(Number(c.note)) : 'NO NOTE' })],
      right: quick,
      build: () => h('div', {}, h('p', { class: 'desc', text: t.d }), grid(t.sec, [G('enabled', 'Enabled', 'bool'), ...fields]), resetBar(t.sec, fields)),
    }));
  });
  return w;
}
function tabCC() {
  const w = h('div', { class: 'stack' }, headBlock('03', 'CONTROLLERS', 'Six MIDI controllers, 0-127. Controller numbers and channels live in MAPPING.'));
  CCS.forEach((c, i) => {
    const on = String((CFG[c.sec] || {}).enabled).toLowerCase() === 'true';
    const fields = ccFields(c);
    w.append(fold('c:' + c.sec, pad2(i + 1), c.name, {
      off: !on,
      badges: [h('em', { class: 'badge note', text: isNum((CFG[c.sec] || {}).cc) ? 'CC' + CFG[c.sec].cc : 'NO CC' })],
      right: h('span', { class: 'mini', title: 'Live value' }, h('i', { 'data-ccbar': i }), h('b', { 'data-ccval': i, text: '0' })),
      build: () => {
        const body = h('div', {}, h('p', { class: 'desc', text: c.d }), grid(c.sec, fields));
        if (c.sec === 'cc_intensity') {
          body.append(h('h4', { class: 'sub', text: 'Energy added by each note' }, h('span', { text: '0-1 per note; negative takes energy away' })),
            grid(c.sec, TRIG.map(t => G(t.sec, t.name, 'num', { min: -1, max: 1, step: 0.01 }))));
        }
        const all = c.sec === 'cc_intensity' ? [...fields, ...TRIG.map(t => G(t.sec, t.name, 'num'))] : fields;
        body.append(resetBar(c.sec, all));
        return body;
      },
    }));
  });
  return w;
}

function conflicts() {
  const bad = {};
  const seen = {};
  const chOf = sec => { const c = val(sec, 'channel'); return isNum(c) ? Number(c) : Number(val('general', 'channel') || 1); };
  TRIG.forEach(t => {
    if (String(val(t.sec, 'enabled')) !== 'true') return;
    const key = 'n' + chOf(t.sec) + ':' + val(t.sec, 'note');
    (seen[key] = seen[key] || []).push('t:' + t.sec);
  });
  CCS.forEach(c => {
    if (String(val(c.sec, 'enabled')) !== 'true') return;
    const key = 'c' + chOf(c.sec) + ':' + val(c.sec, 'cc');
    (seen[key] = seen[key] || []).push('c:' + c.sec);
  });
  for (const k in seen) if (seen[k].length > 1) seen[k].forEach(id => bad[id] = true);
  return bad;
}
async function test(kind, i) {
  const t = LIVE && LIVE.snap.test;
  const running = t && t.kind === kind && t.id === i;
  await api('/api/test', running ? 'stop' : (kind === 1 ? 'note|' : 'cc|') + i);
}
function mapRow(kind, i, c, bad) {
  const sec = c.sec, isNote = kind === 1;
  const numKey = isNote ? 'note' : 'cc';
  const en = String(val(sec, 'enabled')) === 'true';
  const f = G(numKey, isNote ? 'Note' : 'Controller', 'int', { min: 0, max: 127 });
  const nameOut = h('span', { class: 'nn', text: isNote ? noteName(Number(val(sec, numKey))) : 'CC ' + val(sec, numKey) });
  const numBox = h('input', { type: 'number', class: 'num big', min: 0, max: 127, step: 1, value: val(sec, numKey), onchange: async e => {
    const n = normalise(f, e.target.value);
    if (n == null) { e.target.value = val(sec, numKey); return; }
    e.target.value = n;
    if (await save([[sec, numKey, n]])) { nameOut.textContent = isNote ? noteName(n) : 'CC ' + n; render(); }
  } });
  const chCur = val(sec, 'channel');
  const chSel = h('select', { class: 'sel', title: 'MIDI channel', onchange: async e => { if (await save([[sec, 'channel', e.target.value]])) render(); } },
    h('option', { value: 'default', selected: !isNum(chCur) }, 'DEFAULT (' + (val('general', 'channel') || 1) + ')'),
    Array.from({ length: 16 }, (_, k) => h('option', { value: k + 1, selected: Number(chCur) === k + 1 }, 'CH ' + (k + 1))));
  const btn = h('button', { type: 'button', class: 'btn test', 'data-test': kind + ':' + i, onclick: () => test(kind, i) }, h('i'), h('span', { text: 'TEST' }));
  return h('div', { class: 'mrow' + (en ? '' : ' off') + (bad[(isNote ? 't:' : 'c:') + sec] ? ' clash' : '') },
    h('span', { class: 'idx', text: pad2(i + 1) }), h('i', { class: 'led', 'data-led': isNote ? 't' + i : null }), h('b', { class: 'mn', text: c.name }),
    h('span', { class: 'mk', text: isNote ? (val(sec, 'mode') === 'gate' ? 'GATE' : 'PULSE') : 'CC' }),
    numBox, nameOut, chSel, btn);
}
function tabMapping() {
  const bad = conflicts();
  const w = h('div', { class: 'stack' }, headBlock('04', 'MAPPING', 'Assign every trigger to a note and every controller to a number. TEST sends that signal again and again (a note on and off, a controller sweeping 0-127), so Ableton can learn it. Press TEST again to stop.'));
  w.append(h('div', { class: 'howto' }, h('b', { text: 'ABLETON' }), h('span', { text: 'Ctrl+M, click the target, press TEST. Ctrl+M again to leave. Make sure the loopMIDI input has Track and Remote on.' })));
  w.append(h('div', { class: 'maphead' }, h('h3', { text: 'NOTES' }), h('span', { text: 'trigger / mode / note / name / channel' })));
  const t = h('div', { class: 'mtable' }); TRIG.forEach((c, i) => t.append(mapRow(1, i, c, bad))); w.append(t);
  w.append(h('div', { class: 'maphead' }, h('h3', { text: 'CONTROLLERS' }), h('span', { text: 'controller / number / channel' })));
  const u = h('div', { class: 'mtable' }); CCS.forEach((c, i) => u.append(mapRow(2, i, c, bad))); w.append(u);
  if (Object.keys(bad).length) w.append(h('p', { class: 'warnline', text: 'Two enabled signals share the same number on the same channel (marked). They would fight in Ableton.' }));
  return w;
}

// the LIVE tab is built once and then only updated
let L = null;
function tabLive() {
  const w = h('div', { class: 'stack' }, headBlock('05', 'LIVE', 'What Noiseflip reads and sends right now.'));
  L = { spin: [], trig: [], cc: [] };
  const mk = (cls, ...k) => h('div', { class: 'panel ' + cls }, ...k);
  const kv = (label, key) => { const v = h('b', { class: 'v', text: 'NO DATA' }); L[key] = v; return h('div', { class: 'kv' }, h('span', { text: label }), v); };
  const game = mk('p-game', h('h3', { text: 'GAME' }), (L.status = h('p', { class: 'stat', text: 'Connecting to Noiseflip...' })),
    h('div', { class: 'kvs' }, kv('PHYSICS STATE', 'state'), kv('KIND', 'kind'), kv('SPEED m/s', 'speed'), kv('HEIGHT m', 'y'), kv('ROTATION MODE', 'mode'), kv('ENERGY', 'energy')));
  const spin = mk('p-spin', h('h3', { text: 'BOARD SPIN deg/s' }));
  ['X', 'Y', 'Z'].forEach((a, i) => {
    const bar = h('i'), num = h('b', { text: '0' });
    L.spin[i] = { bar, num };
    spin.append(h('div', { class: 'sbar' }, h('span', { text: a }), h('div', { class: 'zero' }, bar), num));
  });
  const padp = mk('p-pad', h('h3', { text: 'GAMEPAD' }));
  L.lt = h('i'); L.rt = h('i');
  padp.append(h('div', { class: 'trg' }, h('span', { text: 'LT' }), h('div', {}, L.lt), h('span', { text: 'RT' }), h('div', {}, L.rt)));
  L.ls = h('i'); L.rs = h('i');
  padp.append(h('div', { class: 'sticks' }, h('div', { class: 'stick' }, L.ls, h('em', { text: 'L' })), h('div', { class: 'stick' }, L.rs, h('em', { text: 'R' }))));
  L.btns = {};
  const bw = h('div', { class: 'chips ro' });
  [['A', 0x1000], ['B', 0x2000], ['X', 0x4000], ['Y', 0x8000], ['LB', 0x100], ['RB', 0x200], ['LS', 0x40], ['RS', 0x80], ['START', 0x10], ['BACK', 0x20], ['D-UP', 1], ['D-DN', 2], ['D-L', 4], ['D-R', 8]]
    .forEach(([n, m]) => { const c = h('span', { class: 'chip', text: n }); L.btns[m] = c; bw.append(c); });
  padp.append(bw);
  const trig = mk('p-trig', h('h3', { text: 'TRIGGERS' }));
  const tg = h('div', { class: 'tgrid' });
  TRIG.forEach((t, i) => {
    const nn = h('b', { text: 'NOT FIRED' }), cnt = h('em', { text: '0' });
    const tile = h('div', { class: 'tile' }, h('span', { text: t.name }), nn, cnt);
    L.trig[i] = { tile, nn, cnt }; tg.append(tile);
  });
  trig.append(tg);
  const cc = mk('p-cc', h('h3', { text: 'CONTROLLERS' }));
  CCS.forEach((c, i) => {
    const bar = h('i'), num = h('b', { text: '0' });
    L.cc[i] = { bar, num };
    cc.append(h('div', { class: 'cbar' }, h('span', { text: c.name }), h('div', {}, bar), num));
  });
  L.log = h('div', { class: 'log' });
  const logp = mk('p-log', h('h3', { text: 'CONSOLE' }), L.log);
  w.append(h('div', { class: 'livegrid' }, game, spin, padp, trig, cc, logp));
  LOG.forEach(addLog);
  if (LIVE) updateLive();
  return w;
}
function addLog(l) {
  if (!L || !L.log) return;
  const near = L.log.scrollHeight - L.log.scrollTop - L.log.clientHeight < 40;
  L.log.append(h('div', {}, h('span', { text: l.t.toFixed(1).padStart(8) }), l.m));
  while (L.log.childNodes.length > 200) L.log.firstChild.remove();
  if (near) L.log.scrollTop = L.log.scrollHeight;
}
function ranges(text) {
  return String(text || '').split(',').map(s => s.trim()).filter(Boolean).map(s => { const [a, b] = s.split('-').map(Number); return [a, isNaN(b) ? a : b]; });
}
const inR = (r, n) => r.some(([a, b]) => n >= a && n <= b);
function updateLive() {
  const s = LIVE.snap;
  if (!L || TAB !== 'live') return;
  L.status.textContent = s.status;
  const st = s.state, g = CFG.states || {};
  L.state.textContent = s.hasState ? st : (s.attached ? 'NO SKATER' : 'NO GAME');
  L.kind.textContent = !s.hasState ? (s.attached ? 'NOT IN A LEVEL' : 'WAITING') : inR(ranges(g.bail), st) ? 'BAIL' : inR(ranges(g.grind), st) ? 'GRIND' : inR(ranges(g.air), st) ? 'AIR' : inR(ranges(g.ground), st) ? 'GROUND' : 'OTHER';
  L.speed.textContent = s.motion ? s.speed.toFixed(2) : 'NO DATA';
  L.y.textContent = s.motion ? s.y.toFixed(2) : 'NO DATA';
  L.mode.textContent = s.mode.toUpperCase();
  L.energy.textContent = s.energy.toFixed(2);
  s.spin.forEach((v, i) => {
    const p = clamp(v / 1500, -1, 1) * 50;
    const bar = L.spin[i].bar;
    bar.style.left = (p < 0 ? 50 + p : 50) + '%'; bar.style.width = Math.abs(p) + '%';
    L.spin[i].num.textContent = Math.round(v);
    bar.classList.toggle('hot', Math.abs(v) >= Number(val('rot_' + 'xyz'[i], 'threshold') || 300));
  });
  const p = s.pad;
  L.lt.style.width = (p.lt / 255 * 100) + '%'; L.rt.style.width = (p.rt / 255 * 100) + '%';
  const dot = (e, x, y) => { e.style.left = (50 + clamp(x / 32768, -1, 1) * 46) + '%'; e.style.top = (50 - clamp(y / 32768, -1, 1) * 46) + '%'; };
  dot(L.ls, p.lx, p.ly); dot(L.rs, p.rx, p.ry);
  for (const m in L.btns) L.btns[m].classList.toggle('on', (p.b & Number(m)) !== 0);
  s.trig.forEach((t, i) => {
    L.trig[i].tile.classList.toggle('on', t.on);
    L.trig[i].nn.textContent = t.n ? 'N' + t.note + ' V' + t.vel : 'NOT FIRED';
    L.trig[i].cnt.textContent = t.n;
  });
  s.cc.forEach((c, i) => { L.cc[i].bar.style.width = (c.v / 127 * 100) + '%'; L.cc[i].num.textContent = Math.round(c.v); });
}

// ------------------------------------------------------------------ shell
// ------------------------------------------------------------------ presets
let PRESETS = { list: [], undo: false, current: '' };
const NAME_OK = /^[A-Za-z0-9()-]([A-Za-z0-9 _.()-]{0,46}[A-Za-z0-9_()-])?$/;
function twoStep(btn, armed, action) {
  let t = 0; const orig = btn.textContent;
  btn.addEventListener('click', () => {
    if (!t) { btn.textContent = armed; btn.classList.add('arm'); t = setTimeout(() => { t = 0; btn.textContent = orig; btn.classList.remove('arm'); }, 3000); }
    else { clearTimeout(t); t = 0; btn.textContent = orig; btn.classList.remove('arm'); action(); }
  });
}
async function presetCall(path, body, okText) {
  const r = await api(path, body);
  if (r && r.ok) { status(okText + ' ' + new Date().toTimeString().slice(0, 8), 'ok'); return true; }
  toast((r && r.error) || 'the preset action failed'); status('NOT DONE', 'bad'); return false;
}
function tabPresets() {
  const w = h('div', { class: 'stack' }, headBlock('06', 'PRESETS', 'Save everything you have set up under a name, and load it again any time. Presets keep every setting except this PC\'s own setup: MIDI port, game process, polling, gamepad slot, note-off style and port check.'));
  const nameIn = h('input', { type: 'text', class: 'txt', placeholder: 'NAME, e.g. Street session', maxlength: 48, spellcheck: 'false' });
  const descIn = h('input', { type: 'text', class: 'txt', placeholder: 'WHAT IS IT FOR? (optional) e.g. soft notes for the drum rack', maxlength: 200 });
  const saveBtn = h('button', { type: 'button', class: 'btn primary', text: 'SAVE CURRENT SETTINGS' });
  const exists = n => PRESETS.list.some(p => p.name.toLowerCase() === n.toLowerCase());
  const doSave = async (name, desc) => { if (await presetCall('/api/preset/save', name + '\n' + desc, 'SAVED')) render(); };
  let armTimer = 0;
  saveBtn.addEventListener('click', async () => {
    const name = nameIn.value.trim();
    if (!NAME_OK.test(name)) { toast('Give it a name: letters, digits, spaces and _ - . ( ), up to 48 characters.'); nameIn.focus(); return; }
    if (exists(name) && !armTimer) {
      saveBtn.textContent = 'REPLACE "' + name.toUpperCase() + '"? CLICK AGAIN'; saveBtn.classList.add('arm');
      armTimer = setTimeout(() => { armTimer = 0; saveBtn.textContent = 'SAVE CURRENT SETTINGS'; saveBtn.classList.remove('arm'); }, 3000);
      return;
    }
    clearTimeout(armTimer); armTimer = 0;
    doSave(name, descIn.value);
  });
  const tools = h('div', { class: 'ptools' },
    h('button', { type: 'button', class: 'btn ghost', text: 'OPEN PRESETS FOLDER', onclick: () => api('/api/preset/folder', 'open') }));
  const undoBtn = h('button', { type: 'button', class: 'btn ghost', text: 'UNDO LAST LOAD', title: 'Go back to the settings that ran before the last preset was loaded',
    onclick: async () => { if (await presetCall('/api/preset/undo', 'undo', 'RESTORED')) { await loadConfig(); render(); } } });
  undoBtn.hidden = true; tools.prepend(undoBtn);
  w.append(h('div', { class: 'panel psave' }, h('h3', { text: 'SAVE WHAT IS RUNNING NOW' }),
    h('div', { class: 'pform' }, nameIn, descIn, saveBtn), tools));
  const filter = h('input', { type: 'text', class: 'txt', placeholder: 'FILTER PRESETS...', spellcheck: 'false' });
  const listEl = h('div', { class: 'plist' });
  const count = h('span', { class: 'pcount' });
  w.append(h('div', { class: 'maphead' }, h('h3', { text: 'YOUR PRESETS' }), count), filter, listEl);
  const paintList = () => {
    const q = filter.value.trim().toLowerCase();
    const items = PRESETS.list.filter(p => !q || p.name.toLowerCase().includes(q) || (p.desc || '').toLowerCase().includes(q));
    count.textContent = PRESETS.list.length + ' saved';
    listEl.replaceChildren();
    if (!PRESETS.list.length) listEl.append(h('p', { class: 'pempty', text: 'No presets yet. Set things up the way you like, give it a name above and press SAVE.' }));
    else if (!items.length) listEl.append(h('p', { class: 'pempty', text: 'No preset matches "' + filter.value + '".' }));
    items.forEach(p => {
      const load = h('button', { type: 'button', class: 'btn', text: 'LOAD', onclick: async () => {
        if (await presetCall('/api/preset/load', p.name, 'LOADED')) { await loadConfig(); render(); } } });
      const upd = h('button', { type: 'button', class: 'btn ghost', text: 'UPDATE', title: 'Replace this preset with what is running now' });
      twoStep(upd, 'REPLACE?', () => doSave(p.name, p.desc || ''));
      const del = h('button', { type: 'button', class: 'btn ghost danger', text: 'DELETE' });
      twoStep(del, 'SURE?', async () => { if (await presetCall('/api/preset/delete', p.name, 'DELETED')) render(); });
      listEl.append(h('div', { class: 'pcard', 'data-pcard': p.name },
        h('div', { class: 'pmain' }, h('b', { class: 'pname', text: p.name }), h('em', { class: 'pbadge' }),
          h('p', { class: 'pdesc', text: p.desc || 'No description.' }), h('span', { class: 'pdate', text: p.saved ? 'SAVED ' + p.saved : '' })),
        h('div', { class: 'pbtns' }, load, upd, del)));
    });
    paintLive && LIVE && paintPresetBadges();
  };
  filter.addEventListener('input', paintList);
  paintList();
  api('/api/presets').then(d => { PRESETS = d; undoBtn.hidden = !d.undo; paintList(); }).catch(() => {});
  return w;
}
function paintPresetBadges() {
  const p = LIVE.snap.preset || { name: '', dirty: false };
  $$('[data-pcard]').forEach(c => {
    const act = c.dataset.pcard === p.name;
    c.classList.toggle('active', act);
    c.querySelector('.pbadge').textContent = act ? (p.dirty ? 'ACTIVE / CHANGED SINCE' : 'ACTIVE') : '';
  });
}

const TABS = [['global', '01', 'GLOBAL', tabGlobal], ['triggers', '02', 'TRIGGERS', tabTriggers], ['cc', '03', 'CC', tabCC], ['mapping', '04', 'MAPPING', tabMapping], ['live', '05', 'LIVE', tabLive], ['presets', '06', 'PRESETS', tabPresets]];
function render() {
  const y = window.scrollY;
  const t = TABS.find(x => x[0] === TAB) || TABS[0];
  L = null;
  const main = $('#main');
  main.replaceChildren(t[3]());
  $$('.tabs button').forEach(b => b.classList.toggle('on', b.dataset.tab === t[0]));
  window.scrollTo(0, y);
  if (LIVE) paintLive();
}
function setTab(id) { TAB = id; store.set('nf_tab', id); window.scrollTo(0, 0); render(); }
function chip(id, on, text) { const e = $('#' + id); e.classList.toggle('ok', !!on); e.lastChild.textContent = text; }
function paintLive() {
  const s = LIVE.snap;
  chip('c-game', s.attached, s.attached ? 'GAME ATTACHED' : 'GAME WAITING');
  chip('c-midi', s.midi.open, s.midi.open ? 'MIDI ' + s.midi.port : 'MIDI NOT OPEN');
  chip('c-pad', s.pad.c, s.pad.c ? 'PAD OK' : 'NO PAD');
  const pr = s.preset || { name: '', dirty: false };
  chip('c-preset', pr.name && !pr.dirty, pr.name ? 'PRESET ' + pr.name + (pr.dirty ? ' *' : '') : 'NO PRESET');
  $('#c-preset').classList.toggle('dirty', !!(pr.name && pr.dirty));
  if (TAB === 'presets') paintPresetBadges();
  if (s.saved && !$('#save').dataset.kind) status(s.saved.toUpperCase(), 'ok');
  $$('[data-led]').forEach(e => { const k = e.dataset.led; if (k[0] === 't') e.classList.toggle('on', !!(s.trig[+k.slice(1)] || {}).on); });
  $$('[data-ccbar]').forEach(e => { const c = s.cc[+e.dataset.ccbar]; e.style.width = (c.v / 127 * 100) + '%'; });
  $$('[data-ccval]').forEach(e => { e.textContent = Math.round(s.cc[+e.dataset.ccval].v); });
  $$('[data-test]').forEach(b => {
    const [k, i] = b.dataset.test.split(':').map(Number);
    const on = s.test.kind === k && s.test.id === i;
    b.classList.toggle('on', on); b.lastChild.textContent = on ? 'STOP' : 'TEST';
  });
  updateLive();
}
async function poll() {
  try {
    const d = await api('/api/live?since=' + SEQ);
    lost = 0; $('#lost').classList.remove('on');
    if (d.seq < SEQ) { SEQ = 0; LOG.length = 0; }
    LIVE = d;
    for (const l of d.log) { LOG.push(l); if (LOG.length > 300) LOG.shift(); addLog(l); }
    SEQ = d.seq;
    paintLive();
  } catch (e) {
    if (++lost > 5) $('#lost').classList.add('on');
  }
}
const SCALES = [70, 80, 90, 100];
function applyScale() { document.documentElement.style.zoom = store.get('nf_scale', 80) / 100; }
function buildScale() {
  const cur = store.get('nf_scale', 80), box = $('#scale');
  box.replaceChildren(...SCALES.map(v => h('button', { type: 'button', class: v === cur ? 'on' : '', text: v + '%', onclick: () => {
    store.set('nf_scale', v); applyScale(); buildScale();
  } })));
}
async function boot() {
  applyScale(); buildScale();
  $('#c-preset').addEventListener('click', () => setTab('presets'));
  $$('.tabs button').forEach(b => b.addEventListener('click', () => setTab(b.dataset.tab)));
  $('#quit').addEventListener('click', async () => {
    if (!confirm('Quit Noiseflip? Notes stop and the program closes.')) return;
    try { await api('/api/quit', 'quit'); } catch (e) {}
    $('#lost').textContent = 'NOISEFLIP IS CLOSED. YOU CAN CLOSE THIS WINDOW.'; $('#lost').classList.add('on');
  });
  try {
    const [c, d, p] = await Promise.all([api('/api/config'), api('/api/defaults'), api('/api/ports')]);
    CFG = parseIni(c); DEF = parseIni(d); PORTS = String(p).split('\n').filter(Boolean);
    $('#ports').replaceChildren(...PORTS.map(x => h('option', { value: x })));
  } catch (e) { $('#lost').classList.add('on'); }
  render();
  await poll();
  setInterval(poll, 90);
  // remember the window's size and position (the program stores them and restores them at the next start)
  let wLast = '', wPend = '';
  setInterval(() => {
    const x = window.screenX, y = window.screenY, w = window.outerWidth, hh = window.outerHeight;
    if (x <= -30000 || w < 400 || hh < 300) return;       // minimised or not a real window
    const v = [x, y, w, hh].join(' ');
    if (v === wLast) return;
    if (v !== wPend) { wPend = v; return; }               // send only once it has stopped changing
    wLast = v; api('/api/window', v).catch(() => {});
  }, 1000);
}
if (typeof module === 'undefined') window.addEventListener('DOMContentLoaded', boot); else module.exports = { boot, parseIni };
