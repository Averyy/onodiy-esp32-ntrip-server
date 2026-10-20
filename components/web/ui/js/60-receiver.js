/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Receiver page (#/receiver): the receiver (state, model, configuration,
 * problems as messages, the messages it sends), its base position (where,
 * the checks, the map on request, Lock or Survey again), then the signals
 * (65-signals.js) with the sky map (66-live.js).
 * Receiver settings (#/receiver/settings): the base position, the receiver
 * profile (signals and the sky map, correction messages, and Advanced folded
 * until opened: station ID, link speed, antenna model, Apply again), receiver
 * setup (automatic or manual: how the receiver gets its settings and what
 * happens if corrections stop) and the comparison of two signal groups (not
 * with manual setup, which cannot start one). What the receiver read back
 * and what it publishes are on Logs (80-diag.js). Receiver maintenance (hands
 * off) is in Settings, next to the receiver update (70-updates.js).
 * Field rules mirror PUT /api/gnss/profile; the station's own validation
 * has the last word.
 */

const RTCM_NAMES = {
  1005: 'Station position', 1006: 'Station position and height', 1007: 'Antenna name', 1008: 'Antenna name and serial',
  1013: 'System parameters', 1033: 'Receiver and antenna names', 1019: 'GPS ephemeris', 1020: 'GLONASS ephemeris',
  1042: 'BeiDou ephemeris', 1044: 'QZSS ephemeris', 1045: 'Galileo F/NAV ephemeris', 1046: 'Galileo I/NAV ephemeris',
  1074: 'GPS observations (MSM4)', 1075: 'GPS observations (MSM5)', 1077: 'GPS observations (MSM7)',
  1084: 'GLONASS observations (MSM4)', 1087: 'GLONASS observations (MSM7)', 1094: 'Galileo observations (MSM4)',
  1097: 'Galileo observations (MSM7)', 1114: 'QZSS observations (MSM4)', 1117: 'QZSS observations (MSM7)',
  1124: 'BeiDou observations (MSM4)', 1127: 'BeiDou observations (MSM7)', 1137: 'NavIC observations (MSM7)',
  1230: 'GLONASS biases',
};
const EPHEMERIS_MSGS = [1019, 1020, 1042, 1045, 1046];
const ONOCOY_RTCM = [1005, 1033, 1077, 1087, 1097, 1117, 1127, 1137, 1230];
const BAUDS = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600];
/* The profile's default link speed (um98x_profile.h). */
const DEFAULT_BAUD = 460800;
const IGS_ANTENNAS = 'https://files.igs.org/pub/station/general/rcvr_ant.tab';
const ONOCOY_INSTALL = 'https://docs.onocoy.com/documentation/3.-become-a-miner/2.-install-your-station';

function rtcmName(n) {
  if (RTCM_NAMES[n]) return RTCM_NAMES[n];
  if (n >= 1101 && n <= 1107) return 'SBAS observations (not sent to servers)';
  return '';
}

/** A distance in words: mm under 1 cm, cm under 1 m, else m or km. */
function fmtDist(m) {
  if (m === null || m === undefined || isNaN(m)) return DASH;
  const a = Math.abs(m);
  if (a < 0.01) return Math.round(a * 1000) + NB + 'mm';
  if (a < 1) return (a * 100).toFixed(1) + NB + 'cm';
  if (a < 1000) return a.toFixed(a < 10 ? 2 : 1) + NB + 'm';
  return (a / 1000).toFixed(1) + NB + 'km';
}

/**
 * Lock / Reset location buttons with their warnings and their error box (out),
 * built once per view: a pending request (or its dialog) keeps its button and
 * box across the refresh. update(g) shows the buttons that fit the state.
 */
function positionActions(done) {
  const out = errBox();
  const lockBtn = btn('Lock this position', async () => {
    const ok = await dialog({ title: 'Lock the surveyed position?', ok: 'Lock position',
      body: h('p', null, 'The receiver becomes a fixed base at this position, the same after every restart. ' +
        'Corrections pause for a few seconds.') });
    if (!ok) return;
    const r = await act(lockBtn, () => api('POST', '/api/gnss/position/lock'), out);
    if (r !== undefined) {
      toast('Locking the position…', 'ok');
      done();
    }
  }, 'primary', { hidden: true });
  const resurveyBtn = btn('Reset location', async () => {
    const ok = await dialog({ title: 'Reset the location?', ok: 'Reset location', okKind: 'danger solid',
      body: [h('p', null, 'The station forgets the locked position; the receiver works it out again (a survey-in) and it locks.'),
        msg('warn', 'The position sent to servers changes. Onocoy may validate the station again.')],
      check: 'I understand that the position changes.' });
    if (!ok) return;
    const r = await act(resurveyBtn, () => api('DELETE', '/api/gnss/position/lock'), out);
    if (r !== undefined) {
      toast('Working out the location again…', 'ok');
      done();
    }
  }, 'danger small', { hidden: true });
  return {
    out: out,
    buttons: [resurveyBtn, lockBtn],
    update(g) {
      const pos = g.position || {};
      const arp = g.arp || {};
      lockBtn.hidden = !(!pos.locked && (arp.valid || (pos.source && pos.source !== 'none')));
      resurveyBtn.hidden = !pos.locked;
    },
  };
}

/* Constellation names of the signal report (GET /api/gnss rx_bands). */
const BAND_SYS = { gps: 'GPS', glonass: 'GLONASS', galileo: 'Galileo', beidou: 'BeiDou', qzss: 'QZSS' };

/* The signals the profile's group should give and the report lacks, or null. */
function bandsMissingMsg(g) {
  const b = g.rx_bands || {};
  if (b.state !== 'ok' || b.matches !== false) return null;
  const lack = (b.systems || []).filter(x => (x.missing || []).length)
    .map(x => (BAND_SYS[x.sys] || x.sys) + ' ' + x.missing.join(', '));
  return msg('warn', h('strong', null, 'The receiver lacks signals its signal group should give.'),
    lack.length ? h('div', { class: 'small' }, 'Missing: ' + lack.join('; ') + '.') : null);
}

/** A signal group the receiver refused, in plain words (GET /api/gnss signal_group_refused), or null. */
function sgRefusedMsg(g) {
  const r = g.signal_group_refused;
  if (!r) return null;
  return msg(r.back_to ? 'warn' : 'bad', h('strong', null, 'The receiver refused signal group ' + r.master + ' ' + r.slave + '.'),
    h('div', { class: 'small' }, (r.back_to ? 'It is back on ' + sgName(r.back_to.master, r.back_to.slave) + ' and configured again.' :
      'Its previous group is unknown, so the configuration failed.') + ' Choose a group from the list and save.'),
    r.reply && r.reply !== 'signal group rejected' ? h('div', { class: 'small muted' }, 'Receiver reply: ' + r.reply) : null);
}

/** Station alerts as messages (bold title, one line); only: the ids to show. */
function alertMsgs(g, only) {
  return (g.alerts || []).filter(only).map(a => {
    const t = L.salert[a] || [a, ''];
    const text = a === 'config_failed' ? configFailedText(g.config_auto_retry, g.config_retry_in_s) :
      a === 'fixed_far' ? fixedFarText(g.fixed_check || {}) : t[1];
    return msg('warn', h('strong', null, t[0]), text ? h('div', { class: 'small' }, text) : null);
  });
}

/* ---- Receiver card ------------------------------------------------------ */

/* The Receiver card's chip for the station's conditions about the receiver (stationState keys). */
const RX_CHIP = {
  journal: 'Update needs you', updating: 'Updating', uart_closed: 'Port closed', no_receiver: 'No receiver found',
  rx_stopped: 'Stopped sending data', no_satellites: 'No satellites', no_rtcm: 'No corrections',
};
/* The conditions the Receiver card explains in a line under its title (what happens, what to do). */
const RX_PROBLEM = ['uart_closed', 'rx_stopped', 'no_satellites', 'no_rtcm'];

/* The Receiver card's chip [text, tone]: the Status page's condition and colour (never "Streaming" while corrections stop,
 * nor beside the fixed-position warning). */
function receiverChip(s) {
  const st = stationState(s, true);
  const r = s.receiver || {};
  if (RX_CHIP[st.key]) return [RX_CHIP[st.key], st.tone];
  if (st.key === 'safe_mode') return ['Off (safe mode)', ''];
  if (r.rx_maintenance) return ['Maintenance', 'info'];
  if (r.signal_test && SGT_RUNNING.indexOf(r.signal_test.state) >= 0) return ['Comparing groups', 'info'];
  if (r.first_apply_held) return ['Not configured yet', 'warn'];
  if (st.delivering && (s.alerts || []).some(a => a.id === 'fixed_far')) return ['Position looks wrong', 'warn'];
  if (st.delivering) return ['Streaming', 'ok'];
  return [HERO_TITLE[r.state] || lbl(L.station, r.state), r.state === 'no_receiver' ? 'bad' : 'info'];
}

/* The profile chip's word for a profile the receiver holds (profileChip). */
const APPLIED = 'Applied';

/** The profile's state in the Configuration row: [text, tone], the chip's, where a profile the receiver took but no window
 * checked yet says when it is checked (delivering: corrections arrive; nothing is checked during maintenance). */
function configWords(g, delivering) {
  const c = g.first_apply_held ? ['Waiting for your OK', 'warn'] : profileChip(g);
  if (c[0] !== APPLIED || g.profile_applied) return c;
  const when = (g.maintenance || {}).on ? ', checked after maintenance' : delivering ? ', being checked' : ', checked once corrections flow';
  return [APPLIED + when, c[1]];
}

/** Who configures the receiver, the profile's state and the signal group it runs ("Applied · 3 6 (Galileo E6)"). */
function configLine(g, delivering) {
  const c = configWords(g, delivering);
  const sg = g.rx_signal_group ? sgName(g.rx_signal_group.master, g.rx_signal_group.slave) : '';
  return [h('span', { class: c[1] === 'bad' || c[1] === 'warn' ? 't-' + c[1] : null }, c[0]), sg ? ' · ' + sg : ''];
}

/* The receiver unchecked during maintenance (GET /api/gnss; rxUnchecked). */
function gnssUnchecked(g) {
  return rxUnchecked((g.maintenance || {}).on, g.identity_confirmed);
}

/* A survey the receiver may still run: inside the station's window, or unfinished after it (without data it makes no
 * progress). */
function surveyRuns(g) {
  const sv = g.survey || {};
  return !!(sv.active || sv.unfinished);
}

/** The base position in one line: its state and what to do (delivering: corrections arrive, so a survey runs). */
function positionLine(g, delivering) {
  const pos = g.position || {};
  const sv = g.survey || {};
  const prof = g.profile || {};
  const have = pos.source && pos.source !== 'none';
  if (prof.base_mode === 'fixed' && prof.fixed && !(pos.locked && have)) return 'Fixed (typed in). Sent at the next configuration.';
  if (pos.locked) return 'Locked. If the antenna moves, reset the location.';
  if (surveyRuns(g)) {
    if (!delivering) return 'Surveying, paused while no corrections arrive.';
    return sv.window_left_s ? 'Surveying, about ' + inMinutes(sv.window_left_s) + ' left. Then lock it.' : 'Surveying. Lock it when it ends.';
  }
  if (gnssUnchecked(g)) return null;   /* the card's line says it (receiverTop) */
  return have ? 'Not locked. Lock it so it stays the same after restarts.' : 'No position yet.';
}

/* Whether the Receiver card lists a station alert (g.alerts' ids): not no_receiver, not the signals one beside the bands
 * message (missing), and not one the problem line says (st.covers). */
function rxAlertShown(st, missing) {
  const problem = !!st && RX_PROBLEM.indexOf(st.key) >= 0;
  return a => a !== 'no_receiver' && !(a === 'signals_mismatch' && missing) && !(problem && st.covers.indexOf(a) >= 0);
}

/** The card under its title: the problem in a line, the alerts (rxAlertShown), then model, configuration and pos (the
 * position's line while its own card has nothing more to show; st: stationState). With the receiver unchecked during
 * maintenance (gnssUnchecked) one line says so, in place of the rows that would only repeat it. */
function receiverTop(g, st, pos) {
  const r = g.receiver || {};
  const missing = bandsMissingMsg(g);
  const unchecked = gnssUnchecked(g);
  return [
    st && RX_PROBLEM.indexOf(st.key) >= 0 ? msg(st.tone, st.lead, st.plan ? h('div', { class: 'muted small' }, st.plan) : null) : null,
    sgRefusedMsg(g),
    missing,
    alertMsgs(g, rxAlertShown(st, !!missing)),
    unchecked ? h('p', { class: 'muted' }, NOT_CHECKED + '.') : null,
    kv([
      r.valid === false ? (unchecked ? null : ['Model', h('span', { class: 'muted' }, 'Not identified yet')]) :
        ['Model', [r.model || DASH, ' · ', fwText(r.firmware, r.build)]],
      unchecked && profileChip(g)[0] === NOT_CHECKED_CHIP ? null : ['Configuration', configLine(g, !st || st.delivering)],
      pos ? ['Position', pos] : null,
    ]),
  ];
}

/** The receiver's own position against the fixed one (GET /api/gnss fixed_check) as a row, or null without a fixed position. */
function fixedCheckRow(g) {
  const c = g.fixed_check || {};
  if (!c.state || c.state === 'none') return null;
  const label = 'Receiver\'s own position';
  if (c.state === 'far') {
    return [label, h('span', { class: 't-warn' }, c.height_only ? 'Fixed height ' + metresText(c.vertical_m) +
      (c.vertical_m > 0 ? ' above it' : ' below it') : metresText(c.distance_m) + ' from the fixed position')];
  }
  if (c.state === 'ok') return [label, 'Within ' + (c.limit_m || 50) + NB + 'm of the fixed position'];
  const pending = fixPendingText(c.state, (g.maintenance || {}).on, gnssUnchecked(g));
  return [label, pending === DASH ? DASH : h('span', { class: 'muted' }, pending)];
}

/** A fixed-position check without a verdict in words (maint: receiver maintenance is on, when nothing is checked;
 * unchecked: the card already says the receiver is not checked). */
function fixPendingText(state, maint, unchecked) {
  if (state === 'refused') return 'Not reported by this firmware';
  if (state === 'no_solution') return 'Not reported yet';
  if (unchecked) return DASH;
  return maint ? NOT_CHECKED : 'Being checked';
}

/** What casters get (the receiver's 1005) against the locked position, as a row: the same or how far from it; not received
 * yet; null while not locked (a survey sends the position it is at, the rows above). */
function streamRow(g) {
  const arp = g.arp || {};
  const label = 'Sent to servers';
  if (!arp.valid) return [label, h('span', { class: 'muted' }, 'Not received yet')];
  if (typeof arp.offset_m !== 'number') return null;
  return [label, arp.offset_m > 0.005 ? h('span', { class: 't-warn' }, fmtDist(arp.offset_m) + ' from this position') : 'This position'];
}

/**
 * The Position card's rows under its line (positionLine): where, how
 * precise, since when and from what, the receiver's own position against a
 * fixed one and what casters get. Without a position yet, the one the
 * receiver sends (a survey's). Null when neither is known.
 */
function positionRows(g) {
  const pos = g.position || {};
  const arp = g.arp || {};
  const have = pos.source && pos.source !== 'none';
  const at = have ? pos : arp.valid ? arp : null;
  if (!at) return null;
  return kv([
    ['Latitude', fmt.deg(at.lat_deg)],
    ['Longitude', fmt.deg(at.lon_deg)],
    ['Height (ellipsoid)', fmt.m(at.height_m)],
    have && pos.sigma_m ? ['Accuracy (σ)', fmtDist(pos.sigma_m)] : null,
    have && pos.locked_unix ? ['Locked on', fmt.time(pos.locked_unix)] : null,
    /* Without a position of the station's own: the survey's so far, or what the receiver sends (a base position it kept). */
    ['Source', have ? lbl(L.posSource, pos.source) : surveyRuns(g) ? 'Survey-in (running)' : 'Sent by the receiver'],
    fixedCheckRow(g),
    have ? streamRow(g) : null,
  ]);
}

/* ---- Receiver setup ----------------------------------------------------- */

/* Receiver setup: Automatic (with or without the station's restart) or Manual, one choice over gnss/autocfg and
 * gnss/recovery: [autocfg, recovery]; each named as L.recovery names it (Logs too). */
const MANAGE = { auto: [true, 'full'], no_restart: [true, 'no_reboot'], manual: [false, 'alerts_only'] };

/** The choice that is these two settings, or null (another combination: Custom). */
function manageKey(autocfg, recovery) {
  return Object.keys(MANAGE).find(k => MANAGE[k][0] === !!autocfg && MANAGE[k][1] === (recovery || 'full')) || null;
}

/** Another combination: what it does, and the choice nearest to it [text, key]. */
function manageCustom(autocfg, recovery) {
  if (autocfg) return ['The receiver is set up automatically, but nothing is done if corrections stop.', 'auto'];
  return ['You set up the receiver, but if corrections stop it is still reconnected and restarted' +
    (recovery === 'no_reboot' ? '.' : ', and the station may restart.'), 'manual'];
}

/**
 * Receiver setup: Automatic, with "Restart the station if the receiver stops
 * answering" under it while it is chosen, or Manual. onSaved: the settings changed (the
 * page reads them again). Returns {nodes, update(g), dirty()}.
 */
function manageCtl(onSaved) {
  const out = errBox();
  let stored = null, autocfg = true, recovery = 'full', held = false, fixKey = null;
  const name = k => L.recovery[MANAGE[k][1]];
  const restart = checkbox('Restart the station if the receiver stops answering', true,
    'When restarting the receiver did not help. At most once an hour and 3 times a day.');
  const choice = radios('Receiver setup', 'manage', [
    ['auto', h('span', { class: 'opt' }, h('span', null, name('auto')), h('small', { class: 'hint' },
      'Sets up the receiver with the profile on this page and keeps it so. If corrections stop, it reconnects, sends the ' +
      'setup again, then restarts the receiver.'))],
    ['manual', h('span', { class: 'opt' }, h('span', null, name('manual')), h('small', { class: 'hint' },
      'You set up the receiver yourself (e.g. from a PC); its settings never change on their own. A profile you save here ' +
      'is sent once. Status alerts you if corrections stop.'))]], null);
  const box = choice.el.querySelector('.choices');
  box.classList.add('stacked');
  choice.el.querySelector('legend').classList.add('sr-only');
  /* The restart box right under Automatic (Manual moves after it). */
  box.append(restart.el, box.children[1]);
  const customText = h('span');
  const fixBtn = btn('', () => saveAs(fixKey, fixBtn), 'small');
  const custom = h('div', { class: 'msg warn', hidden: true }, h('strong', null, 'Custom: '), customText,
    h('div', { class: 'form-actions' }, fixBtn));
  /* The choice on the card: Automatic with or without the restart, Manual, or null (nothing chosen: Custom). */
  const key = () => (choice.value() === 'auto' ? (restart.input.checked ? 'auto' : 'no_restart') : choice.value());
  const save = btn('Save', () => saveAs(key(), save), 'primary');
  const dirty = () => key() !== null && key() !== stored;
  const sync = () => {
    save.hidden = !dirty();
    restart.el.hidden = choice.value() !== 'auto';
  };
  choice.onchange(sync);
  restart.input.addEventListener('change', sync);
  /* The card as the settings are (k: their choice, null for Custom). */
  const show = k => {
    choice.set(k === 'no_restart' ? 'auto' : k);
    restart.input.checked = k ? k !== 'no_restart' : recovery !== 'no_reboot';
  };
  async function saveAs(k, b) {
    if (!k) return;
    const body = {};
    if (MANAGE[k][0] !== autocfg) body.autocfg = MANAGE[k][0];
    if (MANAGE[k][1] !== recovery) body.recovery = MANAGE[k][1];
    /* Automatic setup ends a held first configuration: the receiver is configured at once. */
    if (body.autocfg && held && !(await dialog({ title: 'Configure the receiver now?', ok: 'Configure',
      body: h('p', null, 'Automatic setup sends the profile to the receiver now. Corrections pause for a few seconds.') }))) return;
    /* One setting a request, in the order that never leaves automatic recovery on for a receiver the station does not
     * configure (the station stores them one after the other): recovery first when automatic setup goes off, else
     * automatic setup first. A refusal halfway leaves a safe pair (Custom says which). */
    const parts = ['autocfg', 'recovery'].filter(m => m in body).map(m => ({ [m]: body[m] }));
    if (body.autocfg === false) parts.reverse();
    for (const part of parts) {
      if ((await act(b, () => api('PUT', '/api/gnss/profile', part), out)) === undefined) return;
    }
    autocfg = MANAGE[k][0];
    recovery = MANAGE[k][1];
    stored = k;
    show(k);
    custom.hidden = true;
    sync();
    toast('Saved: ' + name(k) + '.', 'ok');
    if (onSaved) onSaved();
  }
  return {
    nodes: [cardHead('Receiver setup'), h('p', { class: 'muted small' }, 'How the receiver gets its settings, and what happens if ' +
      'corrections stop.'), custom, choice.el, out, h('div', { class: 'form-actions' }, save)],
    update(g) {
      held = !!g.first_apply_held;
      autocfg = !!g.autocfg;
      recovery = g.recovery || 'full';
      const was = dirty();
      stored = manageKey(autocfg, recovery);
      if (!was) show(stored);
      const c = stored ? null : manageCustom(autocfg, recovery);
      custom.hidden = !c;
      if (c) {
        customText.textContent = c[0];
        fixKey = c[1];
        fixBtn.textContent = 'Use "' + name(c[1]) + '"';
      }
      sync();
    },
    dirty: dirty,
  };
}

/* ---- The held first configuration --------------------------------------- */

/* The user's "keep it configured" choice, kept until the first configuration has worked (this browser). */
const HELD_AUTO_KEY = 'heldAuto';

/** What the station would send, from GET /api/gnss/apply/preview, as a numbered list for Logs (the Receiver page and the
 * setup say it in one line: previewSummary): one short line a command ("CONFIG SIGNALGROUP 3 6 · writes, if different,
 * saved"), in columns where the card is wide enough. */
function previewList(d) {
  const steps = (d && d.steps) || [];
  if (!steps.length) return h('p', { class: 'muted' }, 'Nothing to list (no valid profile).');
  return h('ol', { class: 'cmds small' }, steps.map(s => h('li', null, h('code', null, s.text),
    h('span', { class: 'muted' }, ' · ' + [lbl(L.previewKind, s.kind), lbl(L.previewWhen, s.when), s.saved ? 'saved' : null]
      .filter(Boolean).join(', ')))));
}

/** The same in one line ("17 commands; up to 13 change the receiver."), or null. A write sent only when the receiver
 * differs or has the setting off (when: if_differs, if_off) may change nothing, so with any such the count is "up to". */
function previewSummary(d) {
  const steps = (d && d.steps) || [];
  if (!steps.length) return null;
  const writes = steps.filter(s => s.kind === 'write');
  const maybe = writes.some(s => s.when && s.when !== 'always');
  return steps.length + ' commands; ' + (maybe ? 'up to ' : '') + writes.length + ' change the receiver.';
}

/**
 * The first configuration worked (the receiver took it: profileTaken): turn
 * automatic setup (with the station's restart) on when the user ticked it with
 * the confirmation. g: GET /api/gnss. Resolves true when it did.
 */
async function finishHeld(g) {
  if (!store.get(HELD_AUTO_KEY, null) || !g || g.first_apply_held || !profileTaken(g) || g.autocfg) return false;
  store.set(HELD_AUTO_KEY, null);
  try {
    await api('PUT', '/api/gnss/profile', { autocfg: true, recovery: 'full' });
  } catch (e) {
    store.set(HELD_AUTO_KEY, true);   /* tried again on the next look */
    throw e;
  }
  toast('The receiver is configured. Automatic setup is on.', 'ok');
  return true;
}

/**
 * The receiver's first configuration, held until the user confirms it (a
 * build that holds it): how many commands it would send (Logs lists
 * them), the choice of automatic setup afterwards, "Configure the
 * receiver now" and "Not now". The Receiver page and the setup wizard show it alike, so its
 * words name no place on either. opts.onLater: "Not now"; opts.onApplied:
 * the apply was accepted. Returns {el, load()}.
 */
function heldPanel(opts) {
  const err = errBox();
  const status = h('div', { hidden: true });
  const sum = h('p', { class: 'muted small' }, 'Reading the commands…');
  const auto = checkbox('Afterwards, keep it configured and fix problems automatically', true,
    'Turns on automatic receiver setup once this has worked.');
  let preview = null;
  let watching = false;
  const load = async () => {
    try {
      preview = await api('GET', '/api/gnss/apply/preview');
      sum.className = 'muted small';
      sum.textContent = previewSummary(preview) || 'Nothing to send (no valid profile).';
    } catch (e) {
      if (e instanceof ApiError && e.status === 401) return;
      sum.className = 'small t-warn';
      sum.textContent = 'The commands could not be read: ' + errText(e);
    }
  };
  const show = (tone, text) => {
    mount(status, msg(tone, text));
    status.hidden = false;
  };
  /* Until the receiver has taken it (or the attempt failed): then the user's choice is carried out. Runs on
   * when the panel leaves the page (the wizard goes on to the live view meanwhile). After an earlier failed
   * attempt (failedBefore) its alert stays raised until an apply works, so this attempt's failure is only
   * its own once the station was seen configuring and has left that state. */
  const watch = async failedBefore => {
    if (watching) return;
    watching = true;
    let seen = false;
    const until = Date.now() + 15 * 60000;
    while (Date.now() < until) {
      await sleep(3000);
      let g;
      try {
        g = await api('GET', '/api/gnss');
      } catch (e) {
        if (e instanceof ApiError && e.status === 401) break;
        continue;
      }
      if (profileTaken(g) && !g.first_apply_held) {
        show('ok', 'The receiver is configured.' + (store.get(HELD_AUTO_KEY, null) ? ' Turning automatic setup on…' : ''));
        try {
          await finishHeld(g);
        } catch (e) {
          show('warn', 'The receiver is configured, but automatic setup was not turned on: ' + errText(e));
        }
        break;
      }
      if (g.state === 'configuring') seen = true;
      if (g.config_failed && (!failedBefore || (seen && g.state !== 'configuring'))) {
        /* Held: automatic setup is off, so nothing retries it but this button. */
        show('bad', 'The configuration failed and the station does not retry on its own. The Receiver page says why; ' +
          'then press Configure the receiver now.');
        break;
      }
      show('info', g.state === 'streaming' || g.state === 'configuring' ? 'Configuring the receiver…' :
        'Queued until the receiver streams (now: ' + lbl(L.station, g.state).toLowerCase() + ').');
    }
    watching = false;
  };
  const confirmBtn = btn('Configure the receiver now', async () => {
    if (!preview) await load();   /* the card's line says how many commands (not said again here) */
    const ok = await dialog({ title: 'Configure the receiver now?', ok: 'Configure',
      body: [h('p', null, 'The station configures the receiver and saves the settings in it. Corrections pause for a few seconds.'),
        h('p', { class: 'muted small' }, auto.input.checked ? 'Afterwards, automatic setup turns on.' :
          'Afterwards, nothing changes on the receiver on its own (manual setup).')] });
    if (!ok) return;
    let failedBefore = false;
    try {
      failedBefore = !!(await api('GET', '/api/gnss')).config_failed;
    } catch (e) {
      /* unknown: the watch takes a failure it sees as this attempt's */
    }
    const r = await act(confirmBtn, () => api('POST', '/api/gnss/apply'), err);
    if (r === undefined) return;
    store.set(HELD_AUTO_KEY, auto.input.checked ? true : null);
    toast('Configuring the receiver…', 'ok');
    show('info', 'Configuring the receiver…');
    watch(failedBefore);
    if (opts && opts.onApplied) opts.onApplied();
  }, 'primary');
  const laterBtn = btn('Not now', () => {
    if (opts && opts.onLater) opts.onLater();
  });
  return {
    el: h('div', { class: 'stack' },
      cardHead('Receiver not configured yet', badge('Waiting for you', 'info')),
      h('p', null, 'So far the station has only read the receiver. Confirm to configure it with the receiver profile.'),
      sum, auto.el, status, err,
      h('div', { class: 'form-actions' }, laterBtn, confirmBtn)),
    load: load,
  };
}

/* ---- Position and receiver profile ------------------------------------------------ */

/* Manual receiver setup (automatic setup off, not the held first configuration): a saved profile is sent once. */
function manageOwn(g) {
  return !g.autocfg && !g.first_apply_held;
}

/* A card's Save button: "Save and apply" with manual setup, as a changed profile reaches the receiver all the same. */
function saveLabel(g, what) {
  return manageOwn(g) ? 'Save and apply' : 'Save ' + what;
}

/** The receiver holds the stored profile: checked against its RTCM (profile_applied), or acknowledged at its last apply and
 * not checked yet (profile_acknowledged: no RTCM, a receiver without an antenna), unless the RTCM that flows differs. */
function profileTaken(g) {
  return !!g.profile_applied || (!!g.profile_acknowledged && !g.rtcm_differs);
}

/** The profile's state for its chip: [text, tone]; grey with manual setup (as the Receiver page says), and
 * while the receiver took it but no window checked it yet. Amber only when the receiver does not hold it. */
function profileChip(g) {
  if (g.state === 'configuring') return ['Applying', 'info'];
  if (g.config_failed) return ['Failed', 'bad'];
  if (manageOwn(g)) return ['Manual setup', ''];
  if (profileTaken(g) && !g.first_apply_held) return [APPLIED, g.profile_applied ? 'ok' : ''];
  if (gnssUnchecked(g)) return [NOT_CHECKED_CHIP, ''];
  return ['Not applied', 'warn'];
}

/** "Servers see now: ..." under the antenna model (GET /api/gnss rtcm1033), or ''. */
function castersSee(g) {
  const d = g.rtcm1033 || {};
  if (!d.valid) return '';
  return d.antenna ? ['Servers see now: ', h('span', { class: 'ant' }, d.antenna)] : 'Servers see now: no antenna name.';
}

/** The base position's chip in Receiver settings: [text, tone] (delivering false: no corrections arrive, so a survey is
 * paused, as the Receiver page says). */
function positionChip(g, delivering) {
  const pos = g.position || {};
  const prof = g.profile || {};
  if ((g.fixed_check || {}).state === 'far') return ['Looks wrong', 'warn'];
  if (pos.locked && pos.source && pos.source !== 'none') return ['Locked', 'ok'];
  if (prof.base_mode === 'fixed' && prof.fixed) return ['Fixed (typed in)', 'info'];
  if (surveyRuns(g)) return delivering === false ? ['Paused', ''] : ['Surveying', 'info'];
  if (gnssUnchecked(g)) return [NOT_CHECKED_CHIP, ''];
  return ['Not locked', ''];
}

/** Whether the position casters get (1005) matches the locked one, in a line ('' while not locked); first whether the fixed
 * position is where the receiver is, and what fixes it. */
function matchLine(g) {
  const arp = g.arp || {};
  const fc = g.fixed_check || {};
  if (fc.state === 'far') {
    return h('span', { class: 't-warn' }, fixedFarSentence(fc) + (fc.sea_level ? ' Enter the height above the ellipsoid.' :
      ' Check the numbers, or use Survey-in.'));
  }
  if (!(g.position || {}).locked) return '';
  if (!arp.valid || arp.offset_m === null || arp.offset_m === undefined) return h('span', { class: 'muted' }, '1005 not received yet.');
  return arp.offset_m > 0.005 ? h('span', { class: 't-warn' }, '1005 is ' + fmtDist(arp.offset_m) + ' from the locked position.') :
    h('span', { class: 't-ok' }, '1005 matches the locked position.');
}

/** The antenna and B1C/B2a steps the receiver refused at the last configuration (GET /api/gnss apply_steps), as messages
 * (Logs lists both steps either way). */
function stepsRefused(g) {
  const s = g.apply_steps || {};
  const no = (x, what) => (x && x.sent && !x.ok ? msg('warn', what + ': ' + (x.reply || 'no reason given') + '.') : null);
  return [no(s.antenna, 'Antenna model rejected'), no(s.b1cb2a, 'BeiDou B1C/B2a output rejected')];
}

/** Whether "Use the current position" has a position to copy: a 1005 that is not the locked position (a fixed base sends
 * the position it was given, so copying that would repair nothing; while surveying it is the survey's). */
function arpCopyable(g) {
  const arp = g.arp || {};
  const pos = g.position || {};
  return !!arp.valid && !(pos.locked && typeof arp.offset_m === 'number' && arp.offset_m <= 0.005);
}

/** A group of the profile form: its title (with chip at its right, when given), then kids. */
function pgroup(title, chip, ...kids) {
  return h('section', { class: 'pgroup stack' }, chip ? h('div', { class: 'card-head' }, h('h3', null, title), chip) : h('h3', null, title),
    kids);
}

/** A line with an Edit button at its right that shows box under it (and hides it again; onToggle(open) after each). */
function editRow(line, box, onToggle) {
  const b = btn('Edit', () => {
    box.hidden = !box.hidden;
    b.setAttribute('aria-expanded', String(!box.hidden));
    if (onToggle) onToggle(!box.hidden);
  }, 'small', { 'aria-expanded': 'false' });
  box.classList.add('edit-box');
  box.hidden = true;
  return [h('div', { class: 'edit-row' }, line, b), box];
}

/**
 * Receiver settings' Position and Receiver profile cards. Each saves its own
 * fields over the profile as stored now (latest()), so an edit in one never
 * travels with the other's Save; Position's Save shows once its fields
 * differ. fold: {advanced} whether Advanced is open, kept by the page across
 * rebuilds (closed on a new visit). Returns {position, profile, update(g),
 * dirty()}.
 */
function receiverForms(g, latest, done, onCompare, fold) {
  const p = Object.assign({ signal_group_master: 3, signal_group_slave: 6, base_mode: 'survey_in', survey_in_s: 600, fixed: null,
    station_id: 0, com1_baud: DEFAULT_BAUD, rtcm: [], sat_view_output: true }, g.profile || {});
  const folds = fold || { advanced: false };
  let profDirty = false;

  /* -- Position -- */
  const mode = radios('Base mode', 'basemode', [['survey_in', 'Survey-in'], ['fixed', 'Fixed position']], p.base_mode);
  mode.el.querySelector('legend').classList.add('sr-only');
  const survey = numInput(p.survey_in_s || 600, { inputmode: 'numeric', maxlength: 4 });
  const fx = p.fixed || {};
  const lat = numInput(fx.lat_deg, { inputmode: 'text', placeholder: '45.123456789' });
  const lon = numInput(fx.lon_deg, { inputmode: 'text', placeholder: '-75.123456789' });
  const hgt = numInput(fx.height_m, { inputmode: 'text', placeholder: '85.1234' });
  /* -- Profile -- */
  const sgm = numInput(p.signal_group_master, { inputmode: 'numeric', maxlength: 3 });
  const sgs = numInput(p.signal_group_slave, { inputmode: 'numeric', maxlength: 3 });
  /* Named choices; other pairs (a newer receiver firmware) as numbers under Advanced. */
  const sgKey = p.signal_group_master + ' ' + p.signal_group_slave;
  const named = SG_CHOICES.some(c => c[0] === sgKey);
  const sgChoice = radios('Signal group', 'sgroup', SG_CHOICES.map(c => [c[0], h('span', { class: 'opt' },
    h('span', null, c[0] + ' ' + c[1]), h('small', { class: 'hint' }, c[2]))]),
  named ? sgKey : null);
  sgChoice.el.querySelector('.choices').classList.add('stacked', 'grid2');
  sgChoice.el.querySelector('legend').classList.add('sr-only');
  const sgCustom = checkbox('Use other signal group numbers', !named, 'Off by default. Only for receiver firmware with groups not ' +
    'listed above; a pair the receiver refuses is undone.');
  const sid = numInput(p.station_id, { inputmode: 'numeric', maxlength: 4 });
  const baud = selectInput(BAUDS.map(b => [b, b + ' baud' + (b === DEFAULT_BAUD ? ' (default)' : '')]), p.com1_baud);
  const satView = checkbox('Sky map', p.sat_view_output, 'On (default): the receiver sends where each satellite is every 10s, so ' +
    'the Receiver page shows them on a sky map. Off: no sky map.');
  const antModel = textInput(p.antenna_model || '', { maxlength: 64, class: 'ant' });
  const antSerial = textInput(p.antenna_serial || '', { maxlength: 64 });
  const antSetup = numInput(p.antenna_setup_id || 0, { inputmode: 'numeric', maxlength: 3 });
  const seeLine = h('span');

  const F = {
    survey_in_s: field('Survey-in time (seconds)', survey),
    lat_deg: field('Latitude (°)', lat),
    lon_deg: field('Longitude (°)', lon),
    height_m: field('Height (m)', hgt),
    signal_group_master: field('Main antenna', sgm),
    signal_group_slave: field('Second antenna', sgs, '0 turns it off.'),
    station_id: field('Station ID', sid, 'In every message. The default comes from this device\'s MAC; change it only if a ' +
      'server asks for a number.'),
    com1_baud: field('Receiver link speed (COM' + receiverLink().port + ')', baud, '460800 (default) brings each epoch out sooner. Ephemeris messages ' +
      'need 460800 or more.'),
    rtcm: { el: h('div'), input: null, error: null },
    antenna_model: field('Antenna model (IGS name)', antModel, ['Only for an antenna in the IGS list: name and radome as listed. ' +
      'Empty keeps the receiver\'s value; ADVNULLANTENNA clears it. ', extLink('IGS antenna list', IGS_ANTENNAS)]),
    antenna_serial: field('Antenna serial number', antSerial),
    antenna_setup_id: field('Antenna setup ID', antSetup),
  };

  /* Position: only the chosen mode's fields. The 1005 to copy (the latest one), only when it is not the position already
   * set (arpCopyable). */
  const useArp = h('div', null, btn('Use the current position', () => {
    const arp = latest().arp || {};
    if (!arp.valid) return;
    lat.value = arp.lat_deg.toFixed(9);
    lon.value = arp.lon_deg.toFixed(9);
    hgt.value = arp.height_m.toFixed(4);
    posSync();
  }, 'small'));
  const fixedBox = h('div', { class: 'stack' }, h('div', { class: 'cols c3' }, F.lat_deg.el, F.lon_deg.el, F.height_m.el),
    h('p', { class: 'hint' }, 'ITRF coordinates of the antenna\'s exact spot, for example from a PPP service.'), useArp);
  const posErr = errBox();
  /* Each card saves its own part, its button shown once that part changed (the main one: the position's while shown). */
  const posSave = btn('Save position', () => save('position', posSave, posErr), 'primary', { hidden: true });
  const saveBtn = h('button', { type: 'submit', class: 'btn primary', hidden: true }, 'Save profile');
  const saveSync = () => {
    saveBtn.hidden = !profDirty;
    saveBtn.classList.toggle('primary', posSave.hidden);
  };
  const posChip = badge('', '');
  /* Follows the status too (corrections arriving or not), as the Receiver page's position line does. */
  const chipSync = gg => {
    const st = S.status ? stationState(S.status, true) : null;
    const c = positionChip(gg, !st || st.delivering);
    if (posChip.textContent !== c[0]) posChip.textContent = c[0];
    posChip.className = 'badge ' + c[1];
  };
  const match = h('p', { class: 'small' });
  const posSame = () => {
    if (mode.value() !== p.base_mode) return false;
    if (mode.value() !== 'fixed') return survey.value.trim() === String(p.survey_in_s || 600);
    return [[lat, fx.lat_deg], [lon, fx.lon_deg], [hgt, fx.height_m]].every(x => x[0].value.trim() === (x[1] === null || x[1] === undefined ? '' : String(x[1])));
  };
  const posSync = () => {
    const fixed = mode.value() === 'fixed';
    fixedBox.hidden = !fixed;
    F.survey_in_s.el.hidden = fixed;
    posSave.hidden = posSame();
    saveSync();
  };
  mode.onchange(posSync);
  const position = [cardHead('Position', posChip), h('div', { class: 'stack', oninput: posSync },
    mode.el,
    F.survey_in_s.el, fixedBox,
    h('p', { class: 'muted small' }, 'For Onocoy: survey-in, then lock the result on the Receiver page. A precise fixed position is optional. ',
      extLink('Onocoy install guide', ONOCOY_INSTALL)),
    match, posErr, h('div', { class: 'form-actions' }, posSave))];
  posSync();

  /* Signals: other numbers turn the named choices off. */
  const sgNumbers = h('div', { class: 'cols c2' }, F.signal_group_master.el, F.signal_group_slave.el);
  const sgSync = () => {
    sgNumbers.hidden = !sgCustom.input.checked;
    for (const r of sgChoice.el.querySelectorAll('input')) r.disabled = sgCustom.input.checked;
  };
  sgChoice.onchange(() => {
    const v = (sgChoice.value() || '').split(' ');
    if (v.length === 2) {
      sgm.value = v[0];
      sgs.value = v[1];
    }
  });
  sgCustom.input.addEventListener('change', () => {
    if (!sgCustom.input.checked) {
      const key = sgm.value.trim() + ' ' + sgs.value.trim();
      sgChoice.set(SG_CHOICES.some(c => c[0] === key) ? key : SG_RECOMMENDED);
      const v = (sgChoice.value() || SG_RECOMMENDED).split(' ');
      sgm.value = v[0];
      sgs.value = v[1];
    }
    sgSync();
  });
  sgSync();

  /* Correction messages: how many, whether they are the Onocoy set (the chip: recommended, or custom) and each by number
   * and name (how often unless every second), kept up to date while rows change; the rows behind Edit, the list hidden
   * while they show. */
  const rows = h('div', { class: 'msgrows' });
  const rtcmErr = h('small', { class: 'err', hidden: true });
  const summary = h('span');
  const setChip = badge('', '');
  const msgList = h('ul', { class: 'msglist small' });
  F.rtcm.error = t => {
    rtcmErr.textContent = t || '';
    rtcmErr.hidden = !t;
  };
  const sumUp = () => {
    const list = Array.from(rows.children, r => [Number(r.children[0].value.trim()), Number(r.children[1].value.trim())]);
    const n = list.length;
    const each = list.every(x => x[1] === 1);
    const onocoy = n === ONOCOY_RTCM.length && each && ONOCOY_RTCM.every(m => list.some(x => x[0] === m));
    const c = !n ? ['None', 'warn'] : onocoy ? ['Recommended', 'ok'] : ['Custom', ''];
    setChip.textContent = c[0];
    setChip.className = 'badge ' + c[1];
    summary.textContent = !n ? 'No messages: the receiver sends no corrections.' : (onocoy ? 'The Onocoy set: ' : '') +
      n + (n === 1 ? ' message' : ' messages') + (!each ? '.' : n === 1 ? ', every second.' : ', each every second.');
    patch(msgList, list.filter(x => x[0]).map(x => h('li', null, h('b', null, String(x[0])), ' ' + rtcmName(x[0]),
      x[1] !== 1 ? ' · every ' + x[1] + 's' : '')));
  };
  function addRow(msgNo, period) {
    const m = numInput(msgNo, { inputmode: 'numeric', maxlength: 4, 'aria-label': 'Message number' });
    const per = numInput(period || 1, { inputmode: 'numeric', maxlength: 4, 'aria-label': 'Every (seconds)' });
    const nm = h('span', { class: 'muted' }, rtcmName(Number(msgNo)));
    /* The inputs first: sumUp reads them by place. */
    const row = h('div', { class: 'msgrow' }, m, per, nm, h('button', { type: 'button', class: 'icon-btn', 'aria-label': 'Remove message',
      onclick: () => {
        row.remove();
        profDirty = true;
        sumUp();
        saveSync();
      } }, '✕'));
    m.addEventListener('input', () => {
      nm.textContent = rtcmName(Number(m.value));
    });
    rows.append(row);
    return m;
  }
  for (const r of p.rtcm || []) addRow(r.msg, r.period_s);
  sumUp();
  const msgBox = h('div', { class: 'stack' },
    h('div', { class: 'msgrow muted small' }, h('span', null, 'Message'), h('span', null, 'Every (s)'), h('span', null, ''), h('span', null, '')),
    rows, rtcmErr,
    h('div', { class: 'row' },
      btn('Add message', () => {
        addRow('', 1).focus();
        profDirty = true;
        sumUp();
        saveSync();
      }, 'small'),
      btn('Use the Onocoy set', () => {
        clear(rows);
        for (const m of ONOCOY_RTCM) addRow(m, 1);
        profDirty = true;
        sumUp();
        saveSync();
      }, 'small')));
  const antBox = h('div', { class: 'stack' }, F.antenna_model.el, h('div', { class: 'cols c2' }, F.antenna_serial.el, F.antenna_setup_id.el),
    h('p', { class: 'hint' }, 'Serial and setup ID are only sent with a model.'));

  /* Field checks; a field behind its Edit button or in the closed Advanced opens first. */
  const int = (s, lo, hi) => {
    const n = Number(String(s).trim());
    return String(s).trim() !== '' && Number.isInteger(n) && n >= lo && n <= hi ? n : null;
  };
  const num = s => {
    const n = Number(String(s).trim());
    return String(s).trim() !== '' && isFinite(n) ? n : null;
  };
  function checker(keys) {
    for (const k of keys) F[k].error(null);
    let bad = null;
    return {
      fail(k, t) {
        F[k].error(t);
        if (!bad) bad = F[k].input || rows;
      },
      ok() {
        if (!bad) return true;
        for (let n = bad; n; n = n.parentElement) {
          if (n.classList && n.classList.contains('edit-box')) n.hidden = false;
          if (n.tagName === 'DETAILS' && !n.open) {
            n.open = true;
            folds.advanced = true;
          }
        }
        if (bad.focus) bad.focus();
        return false;
      },
    };
  }
  function collectPosition() {
    const c = checker(['survey_in_s', 'lat_deg', 'lon_deg', 'height_m']);
    const out = { base_mode: mode.value(), survey_in_s: 0, fixed: null };
    if (out.base_mode === 'fixed') {
      const la = num(lat.value), lo = num(lon.value), he = num(hgt.value);
      if (la === null || la < -90 || la > 90) c.fail('lat_deg', 'Latitude from -90 to 90.');
      if (lo === null || lo < -180 || lo > 180) c.fail('lon_deg', 'Longitude from -180 to 180.');
      if (he === null || he < -1000 || he > 10000) c.fail('height_m', 'Height from -1000 to 10000 m.');
      out.fixed = { lat_deg: la, lon_deg: lo, height_m: he };
    } else {
      out.survey_in_s = int(survey.value, 1, 3600);
      if (out.survey_in_s === null) c.fail('survey_in_s', 'From 1 to 3600 seconds.');
    }
    return c.ok() ? out : null;
  }
  function collectProfile() {
    const c = checker(['signal_group_master', 'signal_group_slave', 'station_id', 'com1_baud', 'rtcm', 'antenna_model', 'antenna_serial',
      'antenna_setup_id']);
    const pick = sgCustom.input.checked ? null : (sgChoice.value() || '').split(' ');
    const out = {
      signal_group_master: pick && pick.length === 2 ? Number(pick[0]) : int(sgm.value, 0, 255),
      signal_group_slave: pick && pick.length === 2 ? Number(pick[1]) : int(sgs.value, 0, 255),
      station_id: int(sid.value, 0, 4095),
      com1_baud: Number(baud.value),
      rtcm: [],
      sat_view_output: satView.input.checked,
      antenna_model: '',
      antenna_serial: '',
      antenna_setup_id: int(antSetup.value, 0, 255),
    };
    /* Trailing spaces are dropped (the stored form); inner spaces stay. */
    const antenna = (input, k) => {
      const v = input.value.replace(/ +$/, '');
      if (v.length > 31 || /[^\x20-\x7e]/.test(v) || v.indexOf('"') >= 0) {
        c.fail(k, 'At most 31 plain characters (letters, digits, spaces and punctuation, no ").');
        return '';
      }
      return v;
    };
    out.antenna_model = antenna(antModel, 'antenna_model');
    out.antenna_serial = antenna(antSerial, 'antenna_serial');
    if (out.antenna_setup_id === null) c.fail('antenna_setup_id', 'A number from 0 to 255.');
    if (out.signal_group_master === null) c.fail('signal_group_master', 'A number from 0 to 255.');
    if (out.signal_group_slave === null) c.fail('signal_group_slave', 'A number from 0 to 255.');
    if (out.station_id === null) c.fail('station_id', 'A number from 0 to 4095.');
    const seen = {};
    for (const row of rows.children) {
      const inputs = row.querySelectorAll('input');
      const m = int(inputs[0].value, 1001, 4095);
      const per = int(inputs[1].value, 1, 65535);
      if (m === null || per === null) {
        c.fail('rtcm', 'Message numbers 1001 to 4095, each with a period of at least 1 second.');
        break;
      }
      if (m >= 1101 && m <= 1107) {
        c.fail('rtcm', 'Messages 1101 to 1107 never reach a server; remove them.');
        break;
      }
      if (seen[m]) {
        c.fail('rtcm', 'Message ' + m + ' is listed twice.');
        break;
      }
      seen[m] = true;
      out.rtcm.push({ msg: m, period_s: per });
    }
    if (out.rtcm.length > 16) c.fail('rtcm', 'At most 16 messages.');
    if (out.rtcm.some(r => EPHEMERIS_MSGS.indexOf(r.msg) >= 0) && out.com1_baud < 460800) {
      c.fail('com1_baud', 'Ephemeris messages (1019, 1020, 1042, 1045, 1046) need 460800 baud or more.');
    }
    return c.ok() ? out : null;
  }

  const APPLY = {
    queued: 'Saved. The station configures the receiver now.',
    unchanged: 'Saved. Nothing changes on the receiver.',
    safe_mode: 'Saved. Applied after safe mode ends (with automatic setup).',
    maintenance: 'Saved. Applied after receiver maintenance ends (with automatic setup).',
    unsupported_model: 'Saved, but the station does not configure this receiver model.',
    not_running: 'Saved. Applied once the station can talk to the receiver.',
  };
  /* One card's part on top of the profile as stored now. */
  async function save(part, b, out) {
    const fields = part === 'position' ? collectPosition() : collectProfile();
    if (!fields) return;
    const profile = Object.assign({}, latest().profile || p, fields);
    const r = await act(b, () => api('PUT', '/api/gnss/profile', { profile: profile }), out);
    if (r === undefined) return;
    if (part === 'position') {
      Object.assign(p, fields);
      Object.assign(fx, fields.fixed || {});
      posSync();
    } else {
      profDirty = false;
      saveSync();
    }
    toast(APPLY[r.apply] || 'Saved.', r.apply === 'unsupported_model' ? 'warn' : 'ok');
    done();
  }

  const err = errBox();
  const applyBtn = btn('Apply again', async () => {
    const ok = await dialog({ title: 'Apply the profile again?', ok: 'Apply',
      body: h('p', null, 'The station configures the receiver again. Corrections pause for a few seconds.') });
    if (!ok) return;
    const r = await act(applyBtn, () => api('POST', '/api/gnss/apply'), err);
    if (r !== undefined) toast('Applying the profile…', 'ok');
  });
  /* Apply again under Advanced; beside Save while the last configuration failed. */
  const applyRow = settingRow('Apply again', 'Sends the saved profile to the receiver again, e.g. after it was changed from a PC.',
    applyBtn);
  const actions = h('div', { class: 'form-actions' }, saveBtn);
  const antLine = p.antenna_model ? h('span', { class: 'ant' }, p.antenna_model) :
    h('span', null, 'Not set (recommended unless your antenna is in the IGS list)');
  /* The antenna step's refusal above Advanced (in view while it is closed), the B1C/B2a step's under the messages
   * (stepsRefused). */
  const refused = [h('div', { class: 'stack', hidden: true }), h('div', { class: 'stack', hidden: true })];
  /* Hidden while there is no comparison to go to (manual setup). */
  const cmpLink = h('button', { type: 'button', class: 'linkbtn', onclick: onCompare }, 'Not sure? Compare two groups');
  /* Advanced: closed until opened (the only fold on this page), as the page last left it across rebuilds. */
  const advanced = h('details', { class: 'pgroup more', ontoggle: () => {
    folds.advanced = advanced.open;
  } }, h('summary', null, 'Advanced'),
  h('div', { class: 'stack' }, h('div', { class: 'cols c2' }, F.station_id.el, F.com1_baud.el), sgCustom.el, sgNumbers,
    h('h3', { class: 'sub' }, 'Antenna model'), editRow(antLine, antBox),
    h('p', { class: 'muted small' }, 'Goes to servers in message 1033. ', seeLine), applyRow));
  advanced.open = !!folds.advanced;
  const profile = h('form', { class: 'stack', oninput: () => {
    profDirty = true;
    sumUp();
    saveSync();
  }, onsubmit: e => {
    e.preventDefault();
    save('profile', saveBtn, err);
  } },
  pgroup('Signals', null, sgChoice.el, cmpLink, satView.el),
  pgroup('Correction messages', setChip, editRow(summary, msgBox, open => {
    msgList.hidden = open;
  }), msgList, refused[1]),
  /* The antenna's refusal stays in view while Advanced is closed. */
  refused[0],
  advanced,
  err,
  actions);

  const update = gg => {
    const failed = !!gg.config_failed;
    applyRow.hidden = failed;
    const home = failed ? actions : applyRow;
    if (applyBtn.parentNode !== home) home.append(applyBtn);
    chipSync(gg);
    for (const [b, what] of [[posSave, 'position'], [saveBtn, 'profile']]) {
      const t = saveLabel(gg, what);
      if (b.textContent !== t) b.textContent = t;
    }
    patch(match, matchLine(gg));
    match.hidden = !match.textContent;
    useArp.hidden = !arpCopyable(gg);
    patch(seeLine, castersSee(gg));
    stepsRefused(gg).forEach((m, i) => {
      patch(refused[i], m);
      refused[i].hidden = !m;
    });
  };
  update(g);
  return { position: position, profile: profile, update: update, status: () => chipSync(latest()),
    dirty: () => profDirty || !posSame(), compare: shown => {
      cmpLink.hidden = !shown;
    } };
}

/* ---- Signal-group comparison --------------------------------------------- */

const SGT_MINUTES = [[15, '15 minutes'], [30, '30 minutes'], [60, '1 hour'], [120, '2 hours']];
const SGT_RUNNING = ['switching', 'settling', 'measuring', 'deciding', 'restoring'];

/* Hours left to choose, rounded up ("24 h"). */
const sgtHours = s => Math.max(1, Math.ceil((s || 0) / 3600)) + 'h';

/** What a running comparison does now, in one line (null when idle or deciding). */
function sgtLine(t) {
  const grp = sgPairLabel(t.running_group);
  const left = 'about ' + inMinutes(Math.ceil((t.left_s || 0) / 60) * 60);
  const of = (t.phase !== null && t.phase !== undefined ? 'Group ' + (t.phase + 1) + ' of 2, ' : '') + grp;
  if (t.state === 'switching') return 'Switching the receiver to ' + grp + '. Corrections pause briefly.';
  if (t.state === 'settling') return of + ': settling, ' + left + '.';
  if (t.state === 'measuring') return of + ': measuring, ' + left + ' left.';
  if (t.state === 'restoring') return 'Switching back to ' + grp + '.' + (t.outcome ? ' ' + (L.sgtOutcome[t.outcome] || '') : '');
  return null;
}

/**
 * The comparison for the shell's notices (S.status, receiver.signal_test):
 * null, or [tone, title, text, button], as noticeItems (20-shell.js) takes it.
 * A running test shows everywhere but Receiver settings (its card is there);
 * a finished one only when the station ended it without the user's choice.
 */
function sgTestNotice(st) {
  const t = st && st.receiver ? st.receiver.signal_test : null;
  if (!t) return null;
  const details = link('Details', '#/receiver/settings', 'btn small');
  if (t.state === 'deciding') {
    return ['warn', 'Choose a signal group', 'With no choice the old group comes back in about ' + sgtHours(t.decide_left_s) + '.',
      link('Compare and choose', '#/receiver/settings', 'btn small')];
  }
  if (SGT_RUNNING.indexOf(t.state) >= 0) {
    return ['info', 'Comparing signal groups', t.state === 'switching' ? 'Corrections pause briefly.' : t.state === 'restoring' ?
      'Switching back.' : capFirst((t.phase !== null && t.phase !== undefined ? 'Group ' + (t.phase + 1) + ' of 2, ' : '') +
      'about ' + inMinutes(Math.ceil((t.left_s || 0) / 60) * 60) + ' left.'), details];
  }
  if (t.state === 'idle' && t.outcome && ['timed_out', 'rejected', 'failed', 'interrupted'].indexOf(t.outcome) >= 0) {
    return ['warn', 'Signal-group comparison ended', L.sgtOutcome[t.outcome] || t.outcome, details];
  }
  return null;
}

function sgPairLabel(g) {
  return g ? sgName(g.master, g.slave) : DASH;
}

/* Both groups side by side: how long each was measured, its best constellation, then per constellation the satellites and
 * bands, with each band's mean C/N0 and its mean satellites in a line under them ("L5 44.1 (9.6)"; the band table above
 * names the signals). On a phone a cell wraps only between its parts ("10.8 satellites ·", "L1 44.6 (10.8) ·"), never
 * inside one, so no line starts with a dot. */
function sgtResults(t) {
  const res = t.results || [];
  if (res.length !== 2) return null;
  const sysOrder = ['gps', 'glonass', 'galileo', 'qzss', 'beidou', 'navic'];
  const sysOf = (r, sys) => (r.systems || []).find(x => x.sys === sys);
  const present = sysOrder.filter(sys => res.some(r => sysOf(r, sys)));
  const parts = list => list.filter(Boolean).map((x, i, all) => i < all.length - 1 ?
    [h('span', { class: 'nowrap' }, x, NB + '·'), ' '] : h('span', { class: 'nowrap' }, x));
  const head = ['', sgPairLabel(res[0].group), sgPairLabel(res[1].group)];
  const rows = [
    ['Measured', ...res.map(r => r.status === 'rejected' ? h('span', { class: 't-bad' }, lbl(L.sgtResult, r.status)) :
      parts([fmt.dur(r.measured_s), r.status === 'measuring' ? 'measuring' : null,
        r.epoch_gaps ? fmt.num(r.epoch_gaps) + (r.epoch_gaps === 1 ? ' epoch missing' : ' epochs missing') : null]))],
    ['Best', ...res.map(r => r.best_sys ? h('b', { class: r.best_bands >= 4 ? 't-ok' : 't-warn' },
      parts([lbl(L.sys, r.best_sys), r.best_bands + ' bands'])) : DASH)],
    ['Onocoy score', ...res.map(r => {
      if (!(r.systems || []).length) return DASH;
      const s = onocoyScore(r);
      return [h('b', null, s.score.toFixed(2)), ' ', h('small', null, parts(['constellations ' + s.cons.toFixed(2),
        'bands ' + s.band.toFixed(2), s.signals + ' signals']))];
    })],
  ];
  for (const sys of present) {
    rows.push([h('b', null, lbl(L.sys, sys)), ...res.map(r => {
      const x = sysOf(r, sys);
      if (!x) return DASH;
      const n = (x.bands || []).length;
      const per = BAND_ORDER.map(b => (x.per_band || []).find(pb => pb.band === b)).filter(Boolean);
      return [parts([x.satellites.toFixed(1) + ' satellites', n + (n === 1 ? ' band' : ' bands')]),
        per.length ? h('small', null, parts(per.map(pb => [BAND_HEAD[pb.band] || pb.band,
          pb.cnr_dbhz === null || pb.cnr_dbhz === undefined ? '' : [' ', h('span', { class: cnrClass(pb.cnr_dbhz) }, pb.cnr_dbhz.toFixed(1))],
          typeof pb.satellites === 'number' ? ' (' + pb.satellites.toFixed(1) + ')' : '']))) : null];
    })]);
  }
  return h('div', null, table(head, rows, { label: 'Signal-group comparison results', cls: 'cmp' }),
    h('p', { class: 'muted small foot' }, 'Satellites rise and set, so their counts differ between the two measurements; ' +
      'the bands and signals are what a group changes. Onocoy score: constellations \u00d7 bands, from ',
      extLink('Onocoy\'s quality scale', ONO_QUALITY_DOC), ' (its signal-quality factor is alike for both groups on one ' +
      'antenna). Per band: mean C/N0 in dB-Hz, mean satellites in brackets.'));
}

/*
 * Onocoy's Quality Scale (its docs, Mining rewards breakdown > Quality Scale): the constellation reward (each
 * constellation's factor added up; QZSS and NavIC count nothing) times the band reward (the most bands on one
 * constellation; a fifth is Galileo E5 AltBOC, which the UM982 lacks). Its third factor, signal quality, is Onocoy's own
 * measurement of the stream, alike for two groups on one antenna. signals: the MSM signals sent, every constellation.
 */
const ONO_SYS_FACTOR = { gps: 0.286, glonass: 0.142, galileo: 0.286, beidou: 0.286 };
const ONO_BAND_FACTOR = [0, 0.08, 0.32, 0.8, 0.95, 1];
const ONO_QUALITY_DOC = 'https://docs.onocoy.com/documentation/mining-rewards-breakdown/quality-scale';

function onocoyScore(r) {
  let cons = 0, most = 0, signals = 0;
  for (const x of r.systems || []) {
    const n = (x.bands || []).length;
    if (!n) continue;
    if (ONO_SYS_FACTOR[x.sys]) {
      cons += ONO_SYS_FACTOR[x.sys];
      most = Math.max(most, n);
    }
    const per = x.per_band || [];
    signals += per.length ? per.reduce((k, pb) => k + Math.max(1, (pb.signal_ids || []).length), 0) : n;
  }
  cons = Math.min(1, Math.round(cons * 1000) / 1000);
  const band = ONO_BAND_FACTOR[Math.min(5, most)];
  return { cons: cons, band: band, score: Math.round(cons * band * 100) / 100, signals: signals };
}

/* Which group to keep, by Onocoy's score, then the signals sent (Onocoy asks for as many as possible), then the epochs
 * missed: {pick (0, 1 or null), text}; null until both are measured. */
function sgtAdvice(t) {
  const res = t.results || [];
  if (res.length !== 2 || res.some(r => r.status !== 'done')) return null;
  const s = res.map(onocoyScore);
  const keep = i => 'Keep ' + sgPairLabel(res[i].group) + ': ';
  if (s[0].score !== s[1].score) {
    const i = s[0].score > s[1].score ? 0 : 1;
    return { pick: i, text: keep(i) + 'Onocoy scores it higher (' + s[i].score.toFixed(2) + ' against ' + s[1 - i].score.toFixed(2) + ').' };
  }
  if (s[0].signals !== s[1].signals) {
    const i = s[0].signals > s[1].signals ? 0 : 1;
    return { pick: i, text: keep(i) + 'Onocoy scores both ' + s[i].score.toFixed(2) + ', and this one sends more signals (' +
      s[i].signals + ' against ' + s[1 - i].signals + '), as Onocoy asks.' };
  }
  const g = res.map(r => r.epoch_gaps || 0);
  if (g[0] !== g[1]) {
    const i = g[0] < g[1] ? 0 : 1;
    return { pick: i, text: keep(i) + 'Onocoy scores both ' + s[i].score.toFixed(2) + ' with the same signals, and this one missed fewer epochs.' };
  }
  return { pick: null, text: 'Onocoy scores both ' + s[0].score.toFixed(2) + ' with the same signals: either is fine.' };
}

/** Why the station would refuse a start now (its own order), or '' (GET /api/gnss). The station still decides. With manual
 * setup the card shows no start at all (sgtShown). */
function sgtBlock(g) {
  if (!g) return '';
  if (g.first_apply_held) return 'Configure the receiver first.';
  if (!g.autocfg) return 'Needs automatic receiver setup.';
  if ((g.maintenance || {}).on) return 'Not during receiver maintenance.';
  if (g.state === 'no_receiver' || g.state === 'detecting') return 'Needs a connected receiver.';
  if (!(g.position || {}).locked) return 'Lock the base position first.';
  return '';
}

/** Whether Receiver settings shows the comparison's card (g: GET /api/gnss, t: GET /api/gnss/signal-test, st: the status):
 * not with manual setup, which cannot start one, unless one still runs or waits for a choice, or a notice elsewhere points
 * to how one ended (sgTestNotice). */
function sgtShown(g, t, st) {
  return !manageOwn(g) || (!!t && SGT_RUNNING.indexOf(t.state) >= 0) || !!sgTestNotice(st);
}

/**
 * "Compare two signal groups": the start form (two groups, time per group,
 * a confirmation), the progress, the results side by side, keep and stop.
 * Built once per view; update(t, g) shows GET /api/gnss/signal-test (with
 * manual setup no start form: only how it runs or ended, sgtShown).
 * nodes: the card's body; badge: its chip while it runs.
 */
function sgTestCtl(done) {
  const out = errBox();
  const status = h('div');
  const results = h('div');
  let resultsKey = '';
  const opts = SG_CHOICES.map(c => [c[0], sgName(...c[0].split(' '))]);
  const selA = selectInput(opts, '3 6');
  const selB = selectInput(opts, '7 0');
  const minutes = selectInput(SGT_MINUTES, 30);
  let defaultsSet = false;
  const pair = v => {
    const p = String(v).split(' ');
    return { master: Number(p[0]), slave: Number(p[1]) };
  };
  const startBtn = btn('Start comparison', async () => {
    if (selA.value === selB.value) {
      say(out, 'Choose two different signal groups.');
      return;
    }
    const mins = Number(minutes.value);
    const ok = await dialog({ title: 'Compare two signal groups?', ok: 'Start',
      body: [h('p', null, 'The station measures ' + sgName(...selA.value.split(' ')) + ' and ' + sgName(...selB.value.split(' ')) +
        ' for ' + lbl(Object.fromEntries(SGT_MINUTES), mins) + ' each, then waits up to 24 hours for your choice.'),
      msg('warn', 'Each switch restarts the receiver: corrections pause up to a minute, up to three times.'),
      h('p', { class: 'muted small' }, 'With no choice, the group in use now comes back. Other receiver changes wait until it ends.')] });
    if (!ok) return;
    const r = await act(startBtn, () => api('POST', '/api/gnss/signal-test',
      { groups: [pair(selA.value), pair(selB.value)], phase_s: mins * 60 }), out);
    if (r !== undefined) {
      toast('Comparison started.', 'ok');
      done();
    }
  });
  /* The two groups, the time and Start in one row on a wide screen. While a start would be refused (no receiver, held,
   * not locked) only why, beside Start that waits. */
  const why = h('span', { class: 'note muted small', id: 'sgt-why', hidden: true });
  const startRow = h('div', { class: 'act' }, startBtn);
  const fields = h('div', { class: 'cols sgt-form' }, field('Group A', selA).el, field('Group B', selB).el,
    field('Time per group', minutes).el, startRow);
  const blocked = h('div', { class: 'form-actions', hidden: true }, why);
  const form = h('div', { class: 'stack' }, fields, blocked);
  const intro = h('p', { class: 'muted small' }, 'Tries two signal groups one after the other on this antenna, so you can keep the better one.');
  const keepA = btn('Keep A', async () => keep(0, keepA), 'primary');
  const keepB = btn('Keep B', async () => keep(1, keepB), 'primary');
  async function keep(i, b) {
    const r = await act(b, () => api('POST', '/api/gnss/signal-test/keep', { group: i }), out);
    if (r !== undefined) {
      toast('Keeping the group.', 'ok');
      done();
    }
  }
  const stopBtn = btn('Stop', async () => {
    const ok = await dialog({ title: 'Stop the comparison?', ok: 'Stop', okKind: 'danger solid',
      body: h('p', null, 'The station goes back to the signal group from before the comparison.') });
    if (!ok) return;
    const r = await act(stopBtn, () => api('DELETE', '/api/gnss/signal-test'), out);
    if (r !== undefined) {
      toast('Comparison stopped.', 'ok');
      done();
    }
  }, 'danger');
  const badgeEl = badge('', '');
  return {
    badge: badgeEl,
    nodes: [intro, status, results, form, out, h('div', { class: 'form-actions' }, stopBtn, keepA, keepB)],
    update(t, g) {
      if (!t) return;
      const running = SGT_RUNNING.indexOf(t.state) >= 0;
      badgeEl.className = 'badge ' + (t.state === 'deciding' ? 'warn' : 'info');
      badgeEl.textContent = lbl(L.sgtState, t.state);
      badgeEl.hidden = !running;
      /* Idle: what it does and the start; running or deciding: its progress and results. */
      intro.hidden = form.hidden = running || (!!g && manageOwn(g));
      const block = running ? '' : sgtBlock(g);
      /* A start in flight keeps its own disabled state (busy() gives it back when the answer is in). */
      if (!startBtn.classList.contains('busy')) startBtn.disabled = !!block;
      why.textContent = block;
      why.hidden = blocked.hidden = !block;
      fields.hidden = !!block;
      const home = block ? blocked : startRow;
      if (startBtn.parentNode !== home) home.append(startBtn);
      if (block) startBtn.setAttribute('aria-describedby', why.id);
      else startBtn.removeAttribute('aria-describedby');
      if (!running && !defaultsSet && g && g.profile) {
        /* Default: the group running now, and 7 0 (or 3 6 when 7 0 runs). */
        const now = g.profile.signal_group_master + ' ' + g.profile.signal_group_slave;
        if (SG_CHOICES.some(c => c[0] === now)) selA.value = now;
        selB.value = now === '7 0' ? '3 6' : '7 0';
        defaultsSet = true;
      }
      const groups = t.groups || [];
      keepA.hidden = keepB.hidden = t.state !== 'deciding';
      if (groups.length === 2) {
        keepA.textContent = 'Keep ' + sgName(groups[0].master, groups[0].slave);
        keepB.textContent = 'Keep ' + sgName(groups[1].master, groups[1].slave);
      }
      stopBtn.hidden = !running || t.state === 'restoring';
      let text = null;
      const line = sgtLine(t);
      if (line) text = msg('info', line);
      else if (t.state === 'deciding') {
        const adv = sgtAdvice(t);
        text = msg('warn', h('strong', null, adv ? adv.text : 'Both groups measured: keep one.'), ' Until then the receiver ' +
          'stays on ' + sgPairLabel(t.running_group) + '. With no choice within ' + sgtHours(t.decide_left_s) + ', ' +
          sgPairLabel(t.before) + ' comes back.');
        /* The advised group's button is the primary one. */
        keepA.classList.toggle('primary', !adv || adv.pick !== 1);
        keepB.classList.toggle('primary', !adv || adv.pick !== 0);
      }
      else if (t.outcome) text = msg(t.outcome === 'kept' || t.outcome === 'cancelled' ? 'ok' : 'warn',
        h('strong', null, t.outcome === 'kept' && t.kept ? 'You kept ' + sgPairLabel(t.kept) + '.' : (L.sgtOutcome[t.outcome] || t.outcome)),
        t.reason && t.outcome !== 'kept' ? ' ' + t.reason : '', t.ended_unix ? h('span', { class: 'muted' }, ' (' + fmt.time(t.ended_unix) + ')') : null);
      mount(status, text);
      const rk = JSON.stringify(t.results || null);
      if (rk !== resultsKey) {
        resultsKey = rk;
        /* Patched: a keyboard focus on the table's box stays across the refresh. */
        patch(results, t.results ? sgtResults(t) : null);
      }
    },
  };
}

/** Receiver settings' own note while a comparison runs or waits for a choice: its title, or null. Only the way to the
 * comparison's card at the page's end (that card says how far it is). */
function sgtPageNote(t) {
  if (!t || SGT_RUNNING.indexOf(t.state) < 0) return null;
  return t.state === 'deciding' ? 'Choose a signal group' : 'Comparing signal groups';
}

Views.receiver = {
  title: 'Receiver',
  admin: true,
  heading: arg => (/^settings(\/|$)/.test(arg) ? ['Receiver settings', true] : null),
  create(el, arg) {
    return receiverPage(el, arg || '');
  },
};

/**
 * The Receiver page (#/receiver; #/receiver/live and #/receiver/live/start,
 * which opened the sky before it was a map, land here), or with arg "settings" the
 * Receiver settings page (#/receiver/settings/manage: at its Receiver
 * setup card); both read GET /api/gnss every 5 s.
 */
function receiverPage(el, arg) {
  const settings = /^settings(\/|$)/.test(arg);
  let g = null, t = null, forms = null, stop = null, held = null, heldLater = false, sigs = null, missing = null;
  /* gone: the page was left (an answer that comes later starts nothing) */
  let gone = false;
  /* -- The Receiver page: the receiver beside its position, the signals under both -- */
  const heldCard = h('section', { class: 'card wide', hidden: true });
  const chip = badge('', '');
  /* No receiver, or the station looking for it: the same panel as in the setup. */
  const nr = noReceiverPanel();
  const top = h('div', { class: 'stack fit-kv' });
  /* The messages it sends (65-signals.js fills them). */
  const msgs = h('div', { class: 'stack', hidden: true });
  const info = h('section', { class: 'card' }, cardHead('Receiver', chip), nr.el, top, msgs,
    h('div', { class: 'form-actions' }, link('Receiver settings', '#/receiver/settings', 'btn')));
  const posActions = positionActions(() => refresh());
  const posLine = h('p', { hidden: true });
  const posRows = h('div');
  const map = mapBox();
  const where = h('section', { class: 'card' }, h('h2', null, 'Position'), posLine, posRows, map.el, posActions.out,
    h('div', { class: 'form-actions sep' }, posActions.buttons));
  /* Nothing known about the position but its line (no coordinates, so no map, and nothing to do): the line is a row of
   * the Receiver card, which then spans the page (as without a receiver). */
  const posAlone = () => !posRows.firstChild && map.el.hidden && posActions.buttons.every(b => b.hidden);
  /* -- Receiver settings -- */
  const posCard = h('section', { class: 'card a-pos' });
  const profChip = badge('', '');
  const profile = h('section', { class: 'card a-prof' });
  /* A changed setup changes what the page offers (the comparison, the Save buttons' words): read at once. */
  const manage = manageCtl(() => refresh().catch(() => {}));
  const manageCard = h('section', { class: 'card a-man', id: 'manage', tabindex: '-1' }, manage.nodes);
  const sgt = sgTestCtl(() => refresh().catch(() => {}));
  const compare = h('section', { class: 'card wide', id: 'compare', tabindex: '-1' }, cardHead('Compare two signal groups', sgt.badge),
    sgt.nodes);
  const openCompare = () => {
    compare.scrollIntoView({ behavior: 'smooth', block: 'start' });
    compare.focus({ preventScroll: true });
  };
  const noteTitle = h('strong');
  const sgtNote = h('div', { class: 'notice info wide', hidden: true },
    h('div', { class: 'text' }, noteTitle), btn('Go to the comparison', openCompare, 'small'));

  /* The chip, the lines under the title and the position's line follow the status (every second), the rest GET /api/gnss. */
  const status = () => {
    if (settings || !g || missing) return;
    const s = S.status;
    const st = s ? stationState(s, true) : null;
    const c = s ? receiverChip(s) : [lbl(L.station, g.state), stationTone(g.state)];
    chip.className = 'badge ' + c[1];
    chip.textContent = c[0];
    const line = positionLine(g, !st || st.delivering);
    const alone = posAlone();
    patch(top, receiverTop(g, st, alone ? line : null));
    patch(posLine, alone ? null : line);
    posLine.hidden = alone || !line;
    where.hidden = alone;
    info.classList.toggle('wide', alone);
  };

  const fill = () => {
    if (settings) {
      const note = sgtPageNote(t);
      sgtNote.hidden = !note;
      if (note) {
        sgtNote.className = 'notice wide ' + (t.state === 'deciding' ? 'warn' : 'info');
        noteTitle.textContent = note;
      }
      const c = profileChip(g);
      profChip.className = 'badge ' + c[1];
      profChip.textContent = c[0];
      /* No card with nothing to do in it, and no way to one (the Signals group's link). */
      compare.hidden = !sgtShown(g, t, S.status);
      if (forms) {
        forms.update(g);
        forms.compare(!compare.hidden);
      }
      manage.update(g);
      sgt.update(t, g);
      return;
    }
    /* The held first configuration first, until it is made (or put off for this visit). */
    if (g.first_apply_held && !heldLater) {
      if (!held) {
        held = heldPanel({
          onLater: () => {
            heldLater = true;
            heldCard.hidden = true;
          },
          onApplied: () => refresh().catch(() => {}),
        });
        mount(heldCard, held.el);
        held.load();
      }
      heldCard.hidden = false;
    } else {
      heldCard.hidden = true;
      if (!g.first_apply_held) finishHeld(g).then(done => done && refresh(), () => {});
    }
    /* A closed port too: the panel says so (the page-wide notice leaves it to it). */
    missing = (g.state === 'no_receiver' || g.state === 'detecting' || !!((S.status || {}).receiver || {}).uart_closed) &&
      !(g.maintenance || {}).on;
    nr.el.hidden = !missing;
    chip.hidden = top.hidden = missing;
    sigs.messagesOff(missing);
    if (missing) nr.update(S.status || { receiver: { state: g.state } });
    posActions.update(g);
    patch(posRows, positionRows(g));
    map.update(g.position);
    status();
    /* No receiver: the panel says it, the position is a stale fact. */
    if (missing) {
      where.hidden = true;
      info.classList.add('wide');
    }
    sigs.gnss(g);
  };
  /* Advanced open or closed, kept while the page rebuilds its forms. */
  const fold = { advanced: false };
  const buildForms = () => {
    forms = receiverForms(g, () => g, refresh, openCompare, fold);
    forms.compare(!compare.hidden);
    mount(posCard, forms.position);
    mount(profile, cardHead('Receiver profile', profChip), forms.profile);
  };
  /* The comparison's state (Receiver settings only); a failed read only leaves its card as it was. */
  const readTest = () => (!settings ? null : api('GET', '/api/gnss/signal-test').then(v => {
    t = v;
  }, e => {
    if (e instanceof ApiError && e.status === 401) throw e;
  }));
  async function refresh() {
    g = await api('GET', '/api/gnss');
    await readTest();
    if (!gone) fill();
  }
  async function load() {
    g = await api('GET', '/api/gnss');
    await readTest();
    if (gone) return;
    const grid = h('div', { class: settings ? 'grid rxset' : 'grid' });
    if (settings) {
      /* The comparison's note above the grid (its areas: position, receiver setup, profile beside both). */
      mount(el, sgtNote, grid);
      add(grid, [posCard, profile, manageCard, compare]);
      buildForms();
    } else {
      mount(el, grid);
      add(grid, [heldCard, info, where]);
      sigs = signalsSection(grid, msgs);
    }
    fill();
    /* Receiver settings asked for at its Receiver setup card (a "Change" link elsewhere). */
    if (arg === 'settings/manage') {
      manageCard.scrollIntoView({ block: 'start' });
      manageCard.focus({ preventScroll: true });
    }
    /* An address that asked for the sky (live, live/start): the page itself, the sky map in its Signals card. */
    if (/^live/.test(arg)) {
      history.replaceState(null, '', '#/receiver');
      if (shell.current) shell.current.arg = '';
    }
  }
  mount(el, h('p', { class: 'loading' }, 'Loading the receiver…'));
  load().then(() => {
    if (!gone) stop = poller(5000, refresh, true);
  }, e => mount(el, msg('bad', errText(e)), btn('Try again', () => openView('receiver', arg), 'primary')));
  return {
    update(st) {
      if (missing) nr.update(st);
      status();
      if (sigs) sigs.update(st);
      if (forms && g) forms.status();
    },
    destroy() {
      gone = true;
      if (stop) stop();
      if (sigs) sigs.destroy();
    },
    dirty: () => !!(forms && forms.dirty()) || manage.dirty(),
    reload() {
      if (!g) return;
      refresh().then(() => {
        if (settings && !(forms && forms.dirty())) buildForms();
      }).catch(() => {});
    },
  };
}
