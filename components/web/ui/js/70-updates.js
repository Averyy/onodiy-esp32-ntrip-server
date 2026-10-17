/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The Updates card of Settings (#/settings/updates; #/settings/session opens
 * the receiver update session's card under it). Every update starts with a
 * click here: the station only checks once a day (or on "Check now") and
 * shows the notice and the dot on the Settings tab. One row each:
 *   App: the running version and what is offered (install, install again
 *        after a rollback in the first minute, or anyway).
 *   Receiver: its firmware and the offered package (download and verify,
 *        then install; in builds without receiver flashing the package is
 *        checked against every rule instead and released, and nothing is
 *        sent to the receiver).
 *   Receiver maintenance: hands off while a PC tool updates the receiver.
 * The last check and "Check now" sit in the title row.
 * Folded: install an app image (.bin) or a receiver package (.pkg) from a
 * file, and go back to the previous app version.
 * Session: a receiver update session waiting for a decision (retry,
 *        restart the receiver, clear), and how the last one ended.
 */

const JOURNAL_STATE = {
  prepared: 'Prepared', baud_changing: 'Changing the link speed', reset_sent: 'Receiver restarted into its loader',
  menu2_sent: 'Loader asked to receive', transferring: 'Transferring', eot_sent: 'Transfer finished', verifying: 'Checking the result',
  failed: 'Failed', needs_user: 'Waiting for you',
};

const FLASH_ACTIONS = [
  ['retry', 'retry', 'Flash again', 'Flash the same package again?',
    'The same package is sent to the receiver again. Keep the station powered and do not plug anything into the receiver\'s USB port.'],
  ['restart_receiver', 'restart-receiver', 'Restart the receiver', 'Restart the receiver?',
    'The receiver leaves its loader and the station checks its firmware version. A receiver whose firmware is broken may only come back after a power cycle.'],
  ['clear', 'clear', 'Clear the session', 'Clear the update session?',
    'The session is forgotten and its package released. The receiver may not run valid firmware afterwards.'],
];

const NOT_ENABLED = 'Receiver firmware updates are not enabled in this build yet.';
const TRANSFER_FAILED = 'A transfer failed; the update goes on (keep the station powered)';
const NOT_YET_DOWNLOADED = 'Receiver package downloaded and verified. ' + NOT_ENABLED +
  ' The station now checks the package against every rule; nothing is sent to the receiver.';
const CHECK_STARTED = NOT_ENABLED + ' The station checks the package against every rule, then releases it; nothing is sent to the receiver.';

/* Outcome of a receiver session or package check: success and checked are good,
 * a refused package or an unexpected build bad, everything else needs a look. */
function outcomeTone(o) {
  if (o === 'success' || o === 'checked') return 'ok';
  if (o === 'rejected' || o === 'unexpected_build') return 'bad';
  return 'warn';
}

/** Sentence for how a session or package check ended (a final progress message or `last`). */
function outcomeText(m) {
  const fw = m.firmware ? m.firmware : 'unknown firmware';
  const why = m.reason ? ' (' + m.reason + ')' : '';
  switch (m.outcome) {
    case 'success':
      return 'Updated: the receiver runs ' + fw + '.' + (m.reapply_ok === false ? ' The station profile did not come back; check the Receiver page.' : '');
    case 'checked':
      return 'The package passes every rule. ' + NOT_ENABLED + ' It was released; nothing was sent to the receiver.';
    case 'rejected':
      return 'Package refused' + (m.failed_rule ? ' by rule ' + m.failed_rule + (m.cross_layout ? ' (other partition layout)' : '') : '') +
        (m.reason ? ': ' + m.reason : '') + '. Nothing was sent to the receiver.';
    case 'old_firmware':
      return 'The receiver still runs its old firmware (' + fw + ')' + why + '.';
    case 'unexpected_build':
      return 'The receiver runs a build that was not expected: ' + fw + why + '.';
    case 'not_done':
      return 'Not done: the receiver is unchanged' + why + '.';
    case 'not_started':
      return 'Not started; nothing was sent to the receiver' + why + '.';
    case 'needs_user':
      return 'Stopped and waiting for your decision' + why + '. See the receiver update session below.';
    default:
      return lbl(L.flashOutcome, m.outcome);
  }
}

/* Uploads the station did not take because they wait for the user's answer: nothing failed. */
const NOT_INSTALLED = ['same_image', 'rolled_back_ack_required'];

function resultText(m) {
  if (m.outcome && m.outcome !== 'none') return outcomeText(m);
  let t = lbl(L.result, m.result);
  if (m.result === 'package_rejected' && m.failed_rule) t += ' (rule ' + m.failed_rule + (m.cross_layout ? ', other partition layout' : '') + ')';
  if (m.result !== 'ok' && m.result !== 'not_available_yet' && NOT_INSTALLED.indexOf(m.result) < 0 && m.message) t += ' (' + m.message + ')';
  return t;
}

function resultTone(m) {
  if (m.outcome && m.outcome !== 'none') return outcomeTone(m.outcome);
  if (NOT_INSTALLED.indexOf(m.result) >= 0) return '';
  return m.result === 'ok' ? 'ok' : m.result === 'not_available_yet' ? 'warn' : 'bad';
}

/** Blocks, transfers and entry attempts of a receiver session's progress. */
function flashCounts(m) {
  const parts = [];
  if (m.frames_total) parts.push('block ' + fmt.num(m.frames_acked || 0) + ' of ' + fmt.num(m.frames_total));
  if (m.transfers > 1) parts.push('transfer ' + m.transfers);
  if (m.entry_attempts > 1) parts.push('loader attempt ' + m.entry_attempts);
  if (m.retries) parts.push(fmt.num(m.retries) + ' resent');
  return parts.join(', ');
}

/** Progress line for one job: label, bar, then the result. */
function progressWidget() {
  const label = h('div', { class: 'small' });
  const fill = h('span');
  const b = h('div', { class: 'bar' }, fill);
  const el = h('div', { hidden: true }, label, b);
  return {
    el: el,
    set(m) {
      el.hidden = false;
      let frac = null;
      if (m.pct !== undefined && m.pct !== null) frac = m.pct / 100;
      else if (m.bytes_total) frac = m.bytes_sent / m.bytes_total;
      else if (m.total) frac = m.bytes / m.total;
      const tone = m.done ? resultTone(m) : '';
      if (m.done) frac = tone === 'ok' || m.result === 'not_available_yet' ? 1 : frac;
      b.className = 'bar ' + tone + (frac === null && !m.done ? ' indet' : '');
      fill.style.width = frac === null ? '' : (Math.max(0, Math.min(1, frac)) * 100).toFixed(1) + '%';
      const counts = m.job === 'gnss_flash' && !m.done ? flashCounts(m) : '';
      /* "failed" before the final message: a transfer failed and the session goes on (back to the loader,
       * the same package again within the caps, or a stop for your decision). */
      const phase = m.job === 'gnss_flash' && !m.done && m.phase === 'failed' ? TRANSFER_FAILED : lbl(L.phase, m.phase);
      label.textContent = lbl(L.job, m.job) + ': ' + (m.done ? resultText(m) : phase +
        (frac !== null ? ' ' + Math.round(frac * 100) + ' %' : '') + (counts ? ' (' + counts + ')' : ''));
    },
    hide() {
      el.hidden = true;
    },
  };
}

/**
 * Steps of a receiver update. state: confirm, download, failed (at the
 * second step), install or check (the third step runs), refused (the third
 * step failed), installed or checked (all done). upload: the manual-upload
 * wording. checkOnly: builds without receiver flashing, whose third step
 * checks the package instead of installing it.
 */
function receiverSteps(state, upload, checkOnly) {
  const at = { confirm: 0, download: 1, failed: 1, install: 2, check: 2, refused: 2, installed: 3, checked: 3 }[state] || 0;
  const names = upload ? ['Upload and check', 'Confirm'] : ['Confirm', 'Download and verify'];
  names.push(checkOnly ? 'Check every rule (installing is not enabled in this build)' : 'Install on the receiver');
  return h('ol', { class: 'steps' }, names.map((text, i) => {
    let cls = i < at ? 'done' : i === at ? 'now' : '';
    if (i === at && (state === 'failed' || state === 'refused')) cls = 'na';
    return h('li', { class: cls }, (cls === 'done' ? '✓ ' : '') + text);
  }));
}

/* ---- The last update check, in words ---- */

/** {tone, text} for a failed check (GET /api/update), or null when it went well (or none ran). The firmware's error code is in the log. */
function checkFailure(u) {
  const r = u.last_check_result;
  if (!r || r === 'ok') return null;
  const repo = u.release_repo ? 'github.com/' + u.release_repo : 'the release repository';
  const retry = u.next_check_in_s === null || u.next_check_in_s === undefined ? '' :
    u.next_check_in_s <= 0 ? ' The station tries again as soon as it is online.' : ' The station tries again in ' + inMinutes(u.next_check_in_s) + '.';
  const http = u.last_http_status ? ' (HTTP ' + u.last_http_status + ')' : '';
  let text;
  switch (r) {
    case 'no_release':
      text = 'No release found at ' + repo + http + '. The repository may be private or have no releases yet.';
      break;
    case 'no_response':
      text = 'GitHub did not answer.' + (retry || ' The station tries again later.');
      break;
    case 'http_error':
      text = 'GitHub answered with an error' + http + '. The station tries again with the next daily check.';
      break;
    case 'untrusted_redirect':
      text = 'GitHub sent the request somewhere this firmware does not trust, so it stopped there. The station tries again with the next daily check.';
      break;
    case 'bad_manifest':
      text = 'The latest release at ' + repo + ' has no release manifest this build accepts.';
      break;
    default:
      text = 'The update check failed (' + r + ').';
  }
  return { tone: 'warn', text: text };
}

/** "2 h ago": the age of an update check in whole minutes, hours or days. */
function checkedAgo(unix) {
  const s = Date.now() / 1000 - unix;
  return s < 90 ? 'just now' : s < 5400 ? Math.round(s / 60) + NB + 'min ago' : s < 172800 ? Math.round(s / 3600) + NB + 'h ago' :
    Math.round(s / 86400) + NB + 'days ago';
}

/* ---- App image checks before an upload (the first bytes of the file) ---- */

const IMAGE_MAGIC = 0xE9;
const DESC_OFS = 32;               /* image header (24) + first segment header (8) */
const DESC_MAGIC = 0xABCD5432;
const DESC_VERSION_OFS = 16, DESC_PROJECT_OFS = 48, DESC_ELF_SHA_OFS = 144;

function cstr(bytes, ofs, len) {
  let end = ofs;
  while (end < ofs + len && bytes[end] !== 0) end++;
  return String.fromCharCode.apply(null, Array.from(bytes.subarray(ofs, end)));
}

/** {version, project, elfSha} from an app image's first 256 bytes; null if it is not an app image. */
async function readAppImage(file) {
  let b;
  try {
    b = new Uint8Array(await file.slice(0, 256).arrayBuffer());
  } catch (e) {
    return null;
  }
  if (b.length < DESC_OFS + DESC_ELF_SHA_OFS + 32 || b[0] !== IMAGE_MAGIC) return null;
  const d = DESC_OFS;
  const magic = (b[d] | (b[d + 1] << 8) | (b[d + 2] << 16) | (b[d + 3] << 24)) >>> 0;
  if (magic !== DESC_MAGIC) return null;
  return {
    version: cstr(b, d + DESC_VERSION_OFS, 32),
    project: cstr(b, d + DESC_PROJECT_OFS, 32),
    elfSha: Array.from(b.subarray(d + DESC_ELF_SHA_OFS, d + DESC_ELF_SHA_OFS + 32), x => x.toString(16).padStart(2, '0')).join(''),
  };
}

/** Expected downtime against today's remaining Onocoy budget. */
function downtimeRows(down, left) {
  const mid = fmt.utcMidnight();
  const over = down !== null && down !== undefined && left !== null && left !== undefined && down > left;
  return [
    kv([
      ['Expected downtime', down === null || down === undefined ? DASH : 'about ' + fmt.dur(down)],
      ['Onocoy offline budget left today', left === null || left === undefined ? 'unknown' : fmt.dur(Math.max(0, left))],
    ]),
    over ? msg('warn', 'That is more than today\'s remaining Onocoy budget. The budget starts again at 00:00 UTC (' + mid.local +
      ' here, in ' + fmt.dur(mid.in_s) + ').') : null,
  ];
}

function receiverWarnings(flashAvailable) {
  return [
    h('ul', null,
      h('li', null, 'Do not power off the station or plug anything into the receiver\'s USB port.'),
      h('li', null, 'The station profile is applied to the receiver again afterwards.')),
    flashAvailable ? null : msg('warn', NOT_ENABLED + ' The station verifies the package and checks it against every rule, then releases it. ' +
      'Nothing is sent to the receiver.'),
  ];
}

/*
 * A file chooser that looks and works like the other buttons (the
 * browser's own input stays hidden): onFile(file) once one is chosen.
 * {input, b}: both go on the page.
 */
function fileButton(label, accept, onFile) {
  const input = h('input', { type: 'file', accept: accept, hidden: true, onchange: () => {
    const f = input.files && input.files[0];
    input.value = '';   /* the same file again fires again */
    if (f) onFile(f);
  } });
  return { input: input, b: btn(label, () => input.click()) };
}

/* Receiver maintenance (Settings, next to the receiver update): the duration, what it does and what a configuration saves, then start. */
async function startMaintenance(b, out) {
  const dur = selectInput(MAINT_CHOICES, 1800);
  const ok = await dialog({ title: 'Start receiver maintenance?', ok: 'Start',
    body: [h('p', null, MAINT_WHAT), field('Duration', dur).el,
      h('p', { class: 'muted' }, 'It ends by itself, with End now, or at a power cut. Then the station checks the receiver as at start-up.'),
      msg('warn', MAINT_SAVE_NOTE + ' Undo any you do not want to keep before you end.')] });
  if (!ok) return;
  const r = await act(b, () => api('POST', '/api/gnss/maintenance', { duration_s: Number(dur.value) }), out);
  if (r !== undefined) toast('Receiver maintenance on.', 'ok');
}

const updatesPart = {
  create(arg) {
    let u = null, staged = null, flash = null;
    let rflow = null;          /* receiver flow: {state, text, tone} */
    let stagedDeadline = 0, stagedTimer = 0;
    let appMain = false;       /* the app row shows the card's main button */
    let mainShown = false;     /* the app or receiver row does */
    let maintKey = null;
    const offs = [];
    const P = {
      check: progressWidget(), app_update: progressWidget(), app_upload: progressWidget(),
      gnss_update: progressWidget(), gnss_flash: progressWidget(), gnss_upload: progressWidget(), gnss_check: progressWidget(),
    };
    const headBox = h('div'), appBox = h('div'), gnssBox = h('div'), maintBox = h('div'), rbBox = h('div');
    const sessionCard = h('section', { class: 'card wide tight-kv', hidden: true });
    const stagedBox = h('div');
    const appErr = errBox(), gnssErr = errBox(), sessionErr = errBox(), maintErr = errBox();
    const maintLeft = h('span');

    /* ---- The card's title row: the last check and "Check now" (a tool, like Refresh) ---- */
    function renderCheck() {
      const fail = checkFailure(u);
      const checkBtn = u.checks_enabled ? btn('Check now', () => act(checkBtn, async () => {
        await api('POST', '/api/update/check');
        P.check.set({ job: 'check', phase: 'checking', done: false });
      }), 'small') : null;
      /* The last attempt, successful or not; the last success when no attempt ran this start. */
      const at = u.last_attempt_unix || u.last_check_unix;
      const head = cardHead('Updates', u.state && u.state !== 'idle' && u.state !== 'failed' ? badge(lbl(L.updState, u.state), 'info') : null,
        checkBtn ? h('div', { class: 'tools' }, h('span', { class: 'muted small' }, at ? 'Checked ' + checkedAgo(at) +
          (fail && u.last_attempt_unix ? ', failed' : '') : 'Not checked yet'), checkBtn) : null);
      head.classList.add('fit');
      mount(headBox, head,
        u.checks_enabled ? null : h('p', { class: 'muted small' }, ERR.checks_disabled),
        fail && u.state !== 'checking' ? msg(fail.tone, fail.text) : null,
        P.check.el);
    }

    /* ---- App ---- */
    async function installApp(version, ack, button) {
      const ok = await dialog({
        title: 'Install app ' + version + '?',
        ok: 'Install',
        body: [h('p', null, 'The station downloads ' + version + ' from the release over HTTPS, checks its size and SHA-256 against the release\'s manifest, installs it and restarts. ' +
          'The new version then runs on trial: if it fails, the station goes back to this version by itself.'),
        h('p', { class: 'muted' }, 'The casters reconnect after the restart, which usually takes less than a minute of today\'s Onocoy budget.')],
        check: ack ? 'I know this version was rolled back on this station before.' : null,
      });
      if (!ok) return;
      appErr.hidden = true;
      try {
        await busy(button, () => api('POST', '/api/update/app', { version: version, rolled_back_ack: !!ack }));
        P.app_update.set({ job: 'app_update', phase: 'downloading', done: false });
      } catch (e) {
        if (e instanceof ApiError && e.code === 'rolled_back_ack_required' && !ack) {
          installApp(version, true, button);
          return;
        }
        if (e instanceof ApiError && e.code === 'manifest_changed') load();
        showErr(e, appErr);
      }
    }

    /* "App v0.3.0: up to date" or what is offered, with its one button; going back sits in the files' fold. */
    function renderApp() {
      const a = u.app || {};
      const trial = a.trial || {};
      const lr = u.last_result;
      let text, b = null;
      if (a.available && a.latest && !(a.reverted_in_stage1 && a.reverted === a.latest)) {
        text = a.latest + ' is available.';
        b = btn('Update', () => installApp(a.latest, false, b), 'primary');
      } else if (a.reverted_in_stage1 && a.reverted) {
        text = a.reverted + ' was rolled back in its first minute. If that happens again, it is no longer offered.';
        b = btn('Install again', () => installApp(a.reverted, false, b), 'primary');
      } else if (a.latest_rolled_back && a.latest) {
        text = a.latest + ' was rolled back here before, so it is not offered.';
        b = btn('Install anyway', () => installApp(a.latest, true, b));
      } else {
        text = a.dev_build ? 'Development build: no release offers.' : a.latest ? 'Up to date.' : 'No release information yet.';
      }
      if (trial.stage && trial.stage !== 'none') text += ' On trial' + (trial.left_s ? ', ' + fmt.dur(trial.left_s) + ' left' : '') + '.';
      appMain = !!b && b.classList.contains('primary');
      mount(appBox, settingRow('App ' + (a.running || ''), text, b),
        lr && ['app_update', 'app_upload', 'rollback'].indexOf(lr.job) >= 0 && lr.result !== 'ok' ?
          h('p', { class: 'muted small' }, 'Last attempt: ' + resultText(lr)) : null,
        P.app_update.el, appErr);
      const rb = a.rollback_available ? btn('Go back…', async () => {
        const ok = await dialog({ title: 'Go back to the previous version?', ok: 'Go back', okKind: 'danger solid',
          body: h('p', null, 'The station restarts into the version installed before ' + a.running + '. ' + a.running +
            ' is not offered again; you can still install it from a file.') });
        if (!ok) return;
        const r = await act(rb, () => api('POST', '/api/update/rollback', { confirm: true }), appErr);
        if (r !== undefined) showRestarting('Going back to the previous version');
      }, 'danger') : null;
      mount(rbBox, rb ? settingRow('Go back to the previous version', 'The one installed before ' + a.running + '.', rb) : null,
        (a.rolled_back || []).length ? h('p', { class: 'muted small' }, 'Rolled back here, not offered again: ' + a.rolled_back.join(', ') + '.') : null);
      rbBox.hidden = !rbBox.firstChild;
    }

    /* ---- Receiver (offered package) ---- */
    async function updateReceiver(button) {
      const g = u.gnss || {};
      const ok = await dialog({
        title: 'Update the receiver firmware?',
        ok: g.flash_available ? 'Update receiver' : 'Download and verify',
        body: [h('p', null, 'Install ' + g.latest + ' (build ' + g.latest_build + ') on the ' + (g.model || 'receiver') + ' (now ' + (g.running || 'unknown') + ').'),
          downtimeRows(g.expected_downtime_s, g.budget_left_s), receiverWarnings(g.flash_available)],
      });
      if (!ok) return;
      gnssErr.hidden = true;
      try {
        await busy(button, () => api('POST', '/api/update/gnss', { build: g.latest_build, cross_layout_ack: false }));
        rflow = { state: 'download' };
        P.gnss_update.set({ job: 'gnss_update', phase: 'downloading', done: false });
        renderGnss();
      } catch (e) {
        if (e instanceof ApiError && e.code === 'not_offered') load();
        showErr(e, gnssErr);
      }
    }

    /* "Receiver R4.10Build18975: up to date" or the offered build with its button; the downtime against the Onocoy budget is in the dialog. */
    function renderGnss() {
      const g = u.gnss || {};
      const blocked = g.blocked && g.blocked !== 'none';
      const upd = g.available ? btn(g.flash_available ? 'Update receiver…' : 'Download and verify…', () => updateReceiver(upd), appMain ? '' : 'primary',
        { disabled: blocked || (rflow && rflow.state === 'download') }) : null;
      const lr = u.last_result;
      const last = flash && flash.last;
      mainShown = appMain || !!upd;
      mount(gnssBox,
        settingRow('Receiver ' + (g.running ? fwText(g.running, g.running_build) : ''),
          g.available ? fwText(g.latest, g.latest_build) + ' is available.' + (g.flash_available ? '' : ' This build can only download and check it.') :
            !g.running ? (((S.status || {}).receiver || {}).rx_maintenance ? NOT_CHECKED + '.' : 'Its firmware shows once the station finds it.') :
            g.latest ? 'Up to date.' : 'No release information yet.', upd),
        blocked && g.available ? msg('warn', 'Not possible right now: ' + lbl(L.gnssBlock, g.blocked).toLowerCase() +
          (g.blocked_left_s ? ' (about ' + fmt.dur(g.blocked_left_s) + ' left).' : '.')) : null,
        rflow ? receiverSteps(rflow.state, rflow.upload, !g.flash_available) : null,
        P.gnss_update.el,
        P.gnss_check.el,
        P.gnss_flash.el,
        rflow && rflow.text ? msg(rflow.tone || 'info', rflow.text) : null,
        !rflow && last ? msg(outcomeTone(last.outcome), outcomeText(last)) : null,
        !rflow && !last && lr && (lr.job === 'gnss_update' || lr.job === 'gnss_upload') && lr.result !== 'ok' ?
          h('p', { class: 'muted small' }, 'Last attempt: ' + resultText(lr)) : null,
        gnssErr);
    }

    /*
     * Receiver maintenance under the receiver row (from the status, every
     * second): rebuilt only when it starts or ends, the time left written in
     * place, so a button keeps the keyboard focus. Not in safe mode (the
     * receiver is off).
     */
    function renderMaint(st) {
      const r = (st && st.receiver) || {};
      const on = !!r.rx_maintenance;
      maintBox.hidden = !!(st && st.safe_mode && st.safe_mode.active);
      maintLeft.textContent = on && r.maintenance_left_s ? ' Ends in about ' + upMin(r.maintenance_left_s) + '.' : '';
      const key = on ? 'on' + mainShown : 'off';
      if (key === maintKey) return;
      const was = maintKey;
      maintKey = key;
      /* The receiver row says when its firmware is not checked during maintenance. */
      if (was !== null && u && (was === 'off') !== (key === 'off')) renderGnss();
      if (on) {
        const more = btn('Add 30 min', async () => {
          const x = await act(more, () => api('POST', '/api/gnss/maintenance', { duration_s: 1800 }), maintErr);
          if (x !== undefined) toast('Receiver maintenance: ' + fmt.dur(x.left_s) + ' left.', 'ok');
        });
        mount(maintBox, settingRow('Receiver maintenance is on', ['The station sends the receiver no commands.', maintLeft],
          h('div', { class: 'form-actions' }, more, btn('End now', endMaintenance, mainShown ? '' : 'primary'))), maintErr);
      } else {
        const start = btn('Start', () => startMaintenance(start, maintErr));
        mount(maintBox, settingRow('Updating the receiver from a PC?', 'Start receiver maintenance first.', start), maintErr);
      }
    }

    /* ---- Receiver update session ---- */
    function renderSession() {
      const f = flash;
      if (!f || (f.journal === 'none' && !f.receiver_in_loader)) {
        sessionCard.hidden = true;
        return;
      }
      sessionCard.hidden = false;
      const rec = f.record;
      const pr = f.progress;
      const acts = f.actions || {};
      const notNow = [];
      const buttons = FLASH_ACTIONS.map(a => {
        const info = acts[a[0]] || { allowed: false };
        const b = btn(a[2], async () => {
          const ok = await dialog({ title: a[3], ok: a[2], okKind: a[0] === 'clear' ? 'danger solid' : 'primary', body: h('p', null, a[4]) });
          if (!ok) return;
          const r = await act(b, () => api('POST', '/api/gnss/flash/' + a[1], { confirm: true }), sessionErr);
          if (r !== undefined) {
            toast(a[0] === 'clear' ? 'Session cleared.' : 'Started.', 'ok');
            load();
          }
        }, a[0] === 'clear' ? 'danger' : '', { disabled: !info.allowed, title: info.allowed ? null : lbl(L.why, info.why) });
        /* Not in this build: the card's own line says it once for both. */
        if (!info.allowed && info.why && info.why !== 'not_available_yet') notNow.push(h('li', null, a[2] + ': not now, ' + lbl(L.why, info.why) + '.'));
        return b;
      });
      /* The chip and one sentence say the state; the evidence is folded (it stays open across the refresh). No firmware error code
       * ("ESP_ERR_TIMEOUT"): Failure says it in words. */
      const was = sessionCard.querySelector('details.sess');
      const fail = [pr, rec].map(x => x && x.failure).find(x => x && x !== 'none');
      const detail = [
        rec ? kv([
          ['Step', JOURNAL_STATE[rec.state] || rec.state],
          rec.phase && rec.phase !== rec.state ? ['Reached', JOURNAL_STATE[rec.phase] || rec.phase] : null,
          ['Package', fmt.bytes(rec.size) + ', SHA-256 ' + String(rec.sha256 || '').slice(0, 16) + '…'],
          ['Before the update', [rec.pre_model, rec.pre_fw].filter(Boolean).join(' ')],
          ['Transfers / entry attempts', rec.transfers + ' / ' + rec.entry_attempts],
          ['Last acknowledged block', fmt.num(rec.last_acked_block)],
          ['Changed', fmt.time(rec.changed_unix)],
        ]) : null,
        pr ? kv([
          pr.journal_state ? ['Recorded step', JOURNAL_STATE[pr.journal_state] || pr.journal_state] : null,
          pr.frames_total ? ['Blocks acknowledged', fmt.num(pr.frames_acked) + ' of ' + fmt.num(pr.frames_total) +
            ' (' + fmt.num(pr.frames_sent) + ' sent)'] : null,
          ['Transfers / loader attempts', fmt.num(pr.transfers) + ' / ' + fmt.num(pr.entry_attempts)],
          ['Blocks resent', fmt.num(pr.retries)],
        ]) : null,
        f.receiver_text ? h('pre', { class: 'log' }, f.receiver_text) : null];
      mount(sessionCard,
        h('div', { class: 'card-head', id: 'session' }, h('h2', null, 'Receiver update session'), badge(lbl(L.journal, f.journal), f.journal === 'running' ? 'info' : 'warn')),
        f.journal !== 'none' ? h('p', null, (L.journal[f.journal] || ['', ''])[1]) : null,
        f.receiver_in_loader ? msg('warn', L.salert.receiver_in_loader[1]) : null,
        rec || pr ? kv([
          rec ? ['Expected firmware', rec.expected_fw] : null,
          pr ? ['Phase', lbl(L.flashPhase, pr.phase)] : null,
          pr ? ['Sent', fmt.bytes(pr.bytes_sent) + ' of ' + fmt.bytes(pr.bytes_total)] : null,
          fail ? ['Failure', lbl(L.flashFailure, fail)] : null,
        ]) : null,
        pr && pr.bytes_total ? bar(pr.bytes_sent / pr.bytes_total) : null,
        rec || pr || f.receiver_text ? h('details', { class: 'more sess', open: !!(was && was.open) }, h('summary', null, 'Details'), detail) : null,
        f.last && f.last.ended_unix && f.journal === 'none' ? msg(outcomeTone(f.last.outcome), outcomeText(f.last)) : null,
        f.flash_available ? null : msg('', NOT_ENABLED + ' Retrying and restarting the receiver from here need a build with receiver flashing; clearing works.'),
        sessionErr,
        h('div', { class: 'form-actions' }, buttons.slice().reverse()),
        notNow.length ? h('ul', { class: 'muted small' }, notNow) : null);
    }

    /* ---- Files ---- */
    /* An image that does not name this project: install only after the warning (?any_project=1). */
    function anotherProject(project) {
      return dialog({ title: 'Firmware for another project?', ok: 'Install anyway', okKind: 'danger solid',
        body: h('p', null, (project ? 'This image says it is firmware for "' + project + '", not for this station.' :
          project === '' ? 'This image names no project.' : 'This image does not say it is firmware for this station.') +
          ' Firmware for something else can leave the station unreachable until it is flashed over USB.'),
        check: 'I know what this image is.' });
    }
    /* The same image as the one running, or a version rolled back here before: install only after asking. */
    const sameOk = v => dialog({ title: 'Install the same version again?', ok: 'Install again',
      body: h('p', null, 'This is the version already running' + (v ? ' (' + v + ')' : '') + '. Install it again? ' +
        'The station restarts into it; nothing else changes.') });
    const rolledBackOk = v => dialog({ title: 'Install a version that was rolled back?', ok: 'Install anyway',
      body: h('p', null, (v ? 'Version ' + v : 'This version') + ' was rolled back on this station before: its trial failed, or someone ' +
        'went back from it. If it fails again, the station goes back to the version running now.'),
      check: 'I know this version was rolled back on this station before.' });
    const appPick = fileButton('Choose a .bin file…', '.bin,application/octet-stream', f => uploadApp(f, {}));
    const appUploadErr = errBox();
    /* Too big for the station: said before anything is sent (and when a refusal ends in a reset). */
    const tooBig = (file, max, what) => 'The file is ' + fmt.bytes(file.size) + '; ' + what + ' holds at most ' + fmt.bytes(max) + '.';
    /* o.anyProject, o.rolledBackAck, o.reinstall: what the user already confirmed. */
    async function uploadApp(file, o) {
      appUploadErr.hidden = true;
      const a = (u && u.app) || {};
      const max = u && u.app_max_bytes;
      /* Read the image's own description before sending a byte. */
      if (max && file.size > max) {
        say(appUploadErr, tooBig(file, max, 'the station\'s app slot'));
        return;
      }
      const img = await readAppImage(file);
      if (!img) {
        say(appUploadErr, 'This file is not an app image for the station (it does not start like one). Choose the .bin file of a release or of your own build.');
        return;
      }
      if (img.project !== 'onocoydiy-esp-um982' && !o.anyProject) {
        /* The same warning the station's own refusal (not_this_project) leads to: a fork under another
         * project name is installable after it. */
        if (await anotherProject(img.project)) uploadApp(file, Object.assign({}, o, { anyProject: true }));
        return;
      }
      const same = u && u.running_elf_sha256 && img.elfSha === u.running_elf_sha256;
      if (same && !o.reinstall) {
        if (await sameOk(img.version || a.running)) uploadApp(file, Object.assign({}, o, { reinstall: true }));
        return;
      }
      if (!same && (a.rolled_back || []).indexOf(img.version) >= 0 && !o.rolledBackAck) {
        if (await rolledBackOk(img.version)) uploadApp(file, Object.assign({}, o, { rolledBackAck: true }));
        return;
      }
      if (!o.anyProject && !o.rolledBackAck && !o.reinstall) {
        const ok = await dialog({ title: 'Install ' + (img.version || file.name) + '?', ok: 'Install',
          body: [h('p', null, file.name + ', ' + fmt.bytes(file.size) + '. The station writes it to its spare app slot and restarts into it. ' +
            'The new version runs on trial and goes back to this one if it fails.')] });
        if (!ok) return;
      }
      const q = [o.anyProject ? 'any_project=1' : null, o.rolledBackAck ? 'rolled_back_ack=1' : null].filter(x => x).join('&');
      let r;
      try {
        r = await busy(appPick.b, () => upload('/api/upload/app' + (q ? '?' + q : ''), file, (sent, total) =>
          P.app_upload.set({ job: 'app_upload', phase: 'receiving', bytes: sent, total: total, done: false }),
        o.reinstall ? { 'X-Reinstall': '1' } : null));
      } catch (e) {
        P.app_upload.hide();
        if (e instanceof ApiError && (e.code === 'network' || e.code === 'too_large') && max && file.size > max) {
          say(appUploadErr, tooBig(file, max, 'the station\'s app slot'));
          return;
        }
        if (e instanceof ApiError && e.code === 'same_image' && !o.reinstall) {
          if (await sameOk(null)) uploadApp(file, Object.assign({}, o, { reinstall: true }));
          return;
        }
        if (e instanceof ApiError && e.code === 'not_this_project' && !o.anyProject) {
          if (await anotherProject(null)) uploadApp(file, Object.assign({}, o, { anyProject: true }));
          return;
        }
        if (e instanceof ApiError && e.code === 'rolled_back_ack_required' && !o.rolledBackAck) {
          if (await rolledBackOk(null)) uploadApp(file, Object.assign({}, o, { rolledBackAck: true }));
          return;
        }
        showErr(e, appUploadErr);
        return;
      }
      showRestarting('Installing ' + (r && r.version ? r.version : 'the new app'),
        'The image was written' + (r && r.sha256 ? ' (SHA-256 ' + r.sha256.slice(0, 16) + '…)' : '') +
        '. The station restarts into it now; this page reconnects by itself.');
    }

    const pkgPick = fileButton('Choose a .pkg file…', '.pkg,application/octet-stream', f => uploadPkg(f));
    const pkgErr = errBox();
    async function uploadPkg(file) {
      pkgErr.hidden = true;
      const max = u && u.pkg_max_bytes;
      if (max && file.size > max) {
        say(pkgErr, tooBig(file, max, 'the station\'s package area'));
        return;
      }
      try {
        staged = await busy(pkgPick.b, () => upload('/api/upload/gnss', file, (sent, total) =>
          P.gnss_upload.set({ job: 'gnss_upload', phase: sent < total ? 'receiving' : 'inspecting', bytes: sent, total: total, done: false })));
        P.gnss_upload.hide();
        stagedDeadline = Date.now() + (staged.staged.expires_in_s || 900) * 1000;
        renderStaged();
      } catch (e) {
        P.gnss_upload.hide();
        if (e instanceof ApiError && (e.code === 'network' || e.code === 'too_large') && max && file.size > max) {
          say(pkgErr, tooBig(file, max, 'the station\'s package area'));
          return;
        }
        showErr(e, pkgErr);
      }
    }

    function renderStaged(final) {
      clearInterval(stagedTimer);
      if (final) {
        mount(stagedBox, final);
        return;
      }
      if (!staged) {
        clear(stagedBox);
        return;
      }
      const s = staged.staged || {};
      const r = staged.receiver || {};
      const latest = u && u.gnss && u.gnss.latest ? u.gnss.latest : '';
      const exp = textInput(latest, { maxlength: 47, placeholder: 'R4.10Build19152' });
      const fExp = field('Firmware the receiver should report afterwards', exp, 'As published with the package, e.g. R4.10Build19152.');
      const shaOk = checkbox('The SHA-256 above matches the checksum published with this package.', false);
      const expires = h('span');
      const tick = () => {
        const left = Math.max(0, Math.round((stagedDeadline - Date.now()) / 1000));
        expires.textContent = fmt.dur(left);
        if (!left) {
          /* The station released it; show that instead of a dead form. */
          staged = null;
          renderStaged(msg('', 'The package was not confirmed within 15 minutes and was deleted.'));
        }
      };
      const err = errBox();
      const go = btn(staged.flash_available ? 'Start the receiver update…' : 'Continue…', async () => {
        fExp.error(null);
        const v = exp.value.trim();
        const m = /Build(\d+)/.exec(v);
        if (!m || v.length > 47 || hasControl(v)) {
          fExp.error('A firmware version with a Build number, at most 47 characters.');
          exp.focus();
          return;
        }
        if (r.build && Number(m[1]) < r.build) {
          fExp.error('That build is older than the receiver\'s current firmware (build ' + r.build + ').');
          return;
        }
        if (!shaOk.input.checked) {
          say(err, 'Compare the SHA-256 with the published one first.');
          return;
        }
        const ok = await dialog({
          title: 'Install this package on the receiver?',
          ok: staged.flash_available ? 'Update receiver' : 'Continue',
          body: [h('p', null, 'Expected afterwards: ' + v + '.'), downtimeRows(staged.expected_downtime_s, staged.budget_left_s),
            receiverWarnings(staged.flash_available)],
        });
        if (!ok) return;
        err.hidden = true;
        try {
          const r = await busy(go, () => api('POST', '/api/upload/gnss/confirm', { sha256: s.sha256, expected_fw: v, cross_layout_ack: false }));
          staged = null;
          if (r && r.check_only) {
            /* No receiver flashing in this build: the station checks the package and releases it. */
            rflow = { state: 'check', upload: true, tone: 'warn', text: CHECK_STARTED };
            P.gnss_check.set({ job: 'gnss_check', phase: 'checking', done: false });
            renderStaged(msg('info', 'The package check started. Its result shows above.'));
          } else {
            rflow = { state: 'install', upload: true };
            renderStaged(msg('info', 'Receiver update started. Its progress shows above.'));
          }
          renderGnss();
        } catch (e) {
          showErr(e, err);
        }
      }, 'primary');
      const cancel = btn('Delete the package', () => act(cancel, async () => {
        await api('DELETE', '/api/upload/gnss');
        staged = null;
        renderStaged(msg('', 'The package was deleted.'));
      }, err), 'danger');
      mount(stagedBox,
        h('h3', { class: 'section-title' }, 'Uploaded package'),
        receiverSteps('download', true),
        kv([
          ['Size', fmt.bytes(s.size) + ' (' + fmt.num(s.size) + ' bytes)'],
          ['SHA-256', h('code', null, s.sha256)],
          ['Package for', s.model || 'unknown model'],
          ['Partition layout', s.layout === -1 || s.layout === undefined ? 'unknown' : '#' + s.layout + (s.layout_build ? ' (build ' + s.layout_build + ')' : '')],
          s.stamps ? ['Block dates', s.stamps.join(', ')] : null,
          ['Receiver now', [r.model, r.firmware].filter(Boolean).join(' ') || DASH],
          ['Deleted unless confirmed in', expires],
        ]),
        r.model && s.model && r.model !== s.model ? msg('bad', 'This package is for a ' + s.model + ', but the receiver is a ' + r.model + '. The station will refuse it.') : null,
        downtimeRows(staged.expected_downtime_s, staged.budget_left_s),
        fExp.el,
        shaOk.el,
        staged.flash_available ? null : msg('warn', NOT_ENABLED + ' Continuing checks the package against every rule and releases it; nothing is sent to the receiver.'),
        err,
        h('div', { class: 'form-actions' }, cancel, go));
      tick();
      if (staged) stagedTimer = setInterval(tick, 1000);
    }

    const filesFold = h('details', { class: 'more' }, h('summary', null, 'Install from a file or go back'),
      h('div', { class: 'rows' },
        h('div', null, settingRow('App image (.bin)', 'For pre-releases, older versions and your own builds.', appPick.b), appPick.input,
          P.app_upload.el, appUploadErr),
        h('div', null, settingRow('Receiver package (.pkg)', 'Checked first. Nothing reaches the receiver until you confirm.', pkgPick.b),
          pkgPick.input, P.gnss_upload.el, pkgErr, stagedBox),
        rbBox));
    const updCard = h('section', { class: 'card' }, headBox, h('div', { class: 'rows' }, appBox, gnssBox, maintBox, filesFold));

    /* ---- Loading and live progress ---- */
    /* The 10 s poll re-renders only when something changed and nobody is
     * working in the card (keyboard focus, a text selection): every render
     * builds its buttons anew. Loads after an action or an event always
     * render. */
    let shownKey = '';
    async function load(periodic) {
      const res = await Promise.all([
        api('GET', '/api/update'),
        api('GET', '/api/gnss/flash').catch(() => null),
        staged ? Promise.resolve(staged) : api('GET', '/api/upload/gnss').catch(() => null),
      ]);
      u = res[0];
      flash = res[1];
      if (!staged && res[2] && res[2].staged) {
        staged = res[2];
        stagedDeadline = Date.now() + (staged.staged.expires_in_s || 900) * 1000;
        renderStaged();
        filesFold.open = true;   /* an uploaded package waits for its confirmation there */
      }
      if (u.state === 'downloading_gnss' && !rflow) {
        rflow = { state: 'download' };
        P.gnss_update.set({ job: 'gnss_update', phase: 'downloading', pct: u.progress_pct, done: false });
      }
      if (u.state === 'downloading_app') P.app_update.set({ job: 'app_update', phase: 'downloading', pct: u.progress_pct, done: false });
      if (flash && flash.check_running && !rflow) {
        rflow = { state: 'check', tone: 'warn', text: CHECK_STARTED };
        P.gnss_check.set({ job: 'gnss_check', phase: 'checking', done: false });
      }
      if (flash && flash.journal === 'running' && flash.progress && !rflow) {
        rflow = { state: 'install' };
        P.gnss_flash.set(Object.assign({ job: 'gnss_flash' }, flash.progress, { done: false }));
      }
      const key = JSON.stringify([u, flash, rflow]);
      if (periodic && (key === shownKey || interacting(updCard) || interacting(sessionCard))) return;
      shownKey = key;
      renderCheck();
      renderApp();
      renderGnss();
      renderSession();
      maintKey = null;
      renderMaint(S.status);
    }

    offs.push(bus.on('progress', m => {
      const w = P[m.job];
      if (m.job === 'check' && m.done) {
        if (w) w.hide();   /* the result shows once, as the row's own message */
      } else if (w) {
        w.set(m);
      }
      if (m.job === 'gnss_update') {
        if (!m.done) {
          rflow = { state: 'download' };
        } else if (m.result === 'not_available_yet') {
          /* No receiver flashing in this build: the package check follows. */
          rflow = { state: 'check', tone: 'warn', text: NOT_YET_DOWNLOADED };
        } else if (m.result === 'ok') {
          rflow = { state: 'install' };
        } else {
          rflow = { state: 'failed', tone: 'bad', text: 'The receiver package could not be prepared: ' + resultText(m) + '. Nothing on the receiver changed.' };
        }
        if (u) renderGnss();
      }
      if ((m.job === 'gnss_flash' || m.job === 'gnss_check') && u) {
        const upload = !!(rflow && rflow.upload);
        if (!m.done) {
          if (!rflow || (rflow.state !== 'install' && rflow.state !== 'check')) {
            rflow = { state: m.job === 'gnss_check' ? 'check' : 'install', upload: upload };
          }
        } else {
          const good = resultTone(m) === 'ok';
          rflow = {
            state: good ? (m.job === 'gnss_check' ? 'checked' : 'installed') : 'refused',
            upload: upload,
            tone: resultTone(m),
            text: m.outcome && m.outcome !== 'none' ? resultText(m) :
              (m.job === 'gnss_check' ? 'Package check: ' : 'Receiver update: ') + resultText(m),
          };
        }
        renderGnss();
      }
      if (m.done) load().catch(() => {});
    }));
    offs.push(bus.on('event', m => {
      if (m.event === 'update_state') load().catch(() => {});
    }));

    mount(headBox, cardHead('Updates'));
    mount(appBox, h('p', { class: 'loading' }, 'Loading…'));
    renderMaint(S.status);
    const ready = load().catch(e => mount(updCard, msg('bad', errText(e)), btn('Try again', () => openView('settings', arg), 'primary')));
    const stop = poller(10000, () => load(true), true);

    return {
      ready: ready,
      cards: { updates: updCard, session: sessionCard },
      update: renderMaint,
      destroy() {
        stop();
        for (const off of offs) off();
        clearInterval(stagedTimer);
      },
      reload() {
        load().catch(() => {});
      },
    };
  },
};
