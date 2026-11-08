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
 * The last check and "Check now" sit in the title row. Under them, Install
 * manually: an app image (.bin) or a receiver package (.pkg) from a file,
 * the receiver from a PC (receiver maintenance: hands off meanwhile), and
 * going back to the previous app version.
 * Session: a receiver update session waiting for a decision (retry,
 *        restart the receiver, clear), what it recorded, and how the last
 *        one ended.
 */

const JOURNAL_STATE = {
  prepared: 'Prepared', baud_changing: 'Changing the link speed', reset_sent: 'Receiver restarted into its loader',
  menu2_sent: 'Loader asked to receive', transferring: 'Transferring', eot_sent: 'Transfer finished', verifying: 'Checking the result',
  failed: 'Failed', needs_user: 'Waiting for you',
};

/* The interrupted update's package is gone (kept in memory only on this board): how the retry gets it back. */
function packageNeededText(p) {
  return p.in_release ? 'The restart cleared the package from memory. Retry downloads it again first.'
    : 'The restart cleared the package from memory. Upload the same .pkg under Install manually to continue.';
}

const FLASH_ACTIONS = [
  ['retry', 'retry', 'Flash again', 'Flash the same package again?',
    'The same package is sent to the receiver again. Keep the station powered and do not plug anything into the receiver\'s USB port.'],
  ['restart_receiver', 'restart-receiver', 'Restart the receiver', 'Restart the receiver?',
    'The receiver leaves its loader and the station checks its firmware version. A receiver whose firmware is broken may only come back after a power cycle.'],
  ['clear', 'clear', 'Clear the session', 'Clear the update session?',
    'The session is forgotten and its package released. The receiver may not run valid firmware afterwards.'],
];

const TRANSFER_FAILED = 'A transfer failed; the update goes on (keep the station powered)';
const notYetDownloaded = () => 'Receiver package downloaded and verified. ' + notEnabled() +
  ' The station now checks the package against every rule; nothing is sent to the receiver.';
const checkStarted = () => notEnabled() + ' The station checks the package against every rule, then releases it; nothing is sent to the receiver.';

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
      /* reapply_ok null: the profile was applied again, with no corrections yet to check it against (no antenna). */
      return 'Updated: the receiver runs ' + fw + '.' + (m.reapply_ok === false ? ' The station profile did not come back; check the Receiver page.' :
        m.reapply_ok === null ? ' Its station settings are applied again and get checked once corrections flow; none arrive yet, so check that the antenna is connected.' : '');
    case 'checked':
      return 'The package passes every rule. ' + notEnabled() + ' It was released; nothing was sent to the receiver.';
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

/* The last receiver session's outcome is shown until dismissed in this browser (by that session: a new one shows again). */
let flashSeenKey = null; /* fallback when the browser storage is unavailable */
const lastKey = l => [l.outcome, l.firmware, l.build, l.ended_unix || 0].join('|');
const lastDismissed = l => store.get('flashSeen', flashSeenKey) === lastKey(l);

/** The last session's outcome with its ×, or null once dismissed; onDismiss redraws. */
function lastOutcomeMsg(last, onDismiss) {
  if (!last || lastDismissed(last)) return null;
  return h('div', { class: 'msg dismissable ' + outcomeTone(last.outcome) }, h('span', null, outcomeText(last)),
    h('button', { type: 'button', class: 'btn small dismiss', 'aria-label': 'Dismiss', onclick: () => {
      flashSeenKey = lastKey(last);
      store.set('flashSeen', flashSeenKey);
      onDismiss();
    } }, '×'));
}

/* Updates the station did not install because the user stopped the download, or an upload waits for the user's
 * answer: nothing failed. */
const NOT_INSTALLED = ['same_image', 'rolled_back_ack_required', 'cancelled'];

/* A receiver package download the user cancelled (an update, or the package again for an interrupted one). */
const RX_CANCELLED = 'Download cancelled. Nothing was sent to the receiver.';

function resultText(m) {
  if (m.outcome && m.outcome !== 'none') return outcomeText(m);
  let t = m.result === 'not_available_yet' ? 'Verified. ' + notEnabled() : lbl(L.result, m.result);
  if (m.result === 'package_rejected' && m.failed_rule) t += ' (rule ' + m.failed_rule + (m.cross_layout ? ', other partition layout' : '') + ')';
  if (m.result !== 'ok' && m.result !== 'not_available_yet' && NOT_INSTALLED.indexOf(m.result) < 0 && m.message) t += ' (' + m.message + ')';
  return t;
}

function resultTone(m) {
  if (m.outcome && m.outcome !== 'none') return outcomeTone(m.outcome);
  if (NOT_INSTALLED.indexOf(m.result) >= 0) return '';
  return m.result === 'ok' ? 'ok' : m.result === 'not_available_yet' ? 'warn' : 'bad';
}

/* The last step of a receiver update: the new firmware runs, the station's settings go back on and are checked. */
const FINISHING_WAIT = 'Finishing, please wait. This can take a few minutes.';

/** Blocks, transfers and entry attempts of a receiver session's progress. */
function flashCounts(m) {
  const parts = [];
  if (m.frames_total) parts.push('block ' + fmt.num(m.frames_acked || 0) + ' of ' + fmt.num(m.frames_total));
  if (m.transfers > 1) parts.push('transfer ' + m.transfers);
  if (m.entry_attempts > 1) parts.push('loader attempt ' + m.entry_attempts);
  if (m.retries) parts.push(fmt.num(m.retries) + ' resent');
  return parts.join(', ');
}

/**
 * Steps of a receiver update, from the user's confirmation on (that is no
 * step: the dialog asked). state: download (the first step runs), failed
 * (the first step failed), ready (an uploaded package waits for its
 * confirmation: the first step done, the second not started), install or
 * check (the second step runs), refused (the second step failed), installed
 * or checked (all done). upload: the manual-upload wording. checkOnly: builds
 * without receiver flashing, whose second step checks the package instead of
 * installing it. Each step says its number ("1. Download and verify"), a done
 * one ends in a tick.
 */
function receiverSteps(state, upload, checkOnly) {
  const at = { download: 0, failed: 0, ready: 1, install: 1, check: 1, refused: 1, installed: 2, checked: 2 }[state] || 0;
  const names = [upload ? 'Upload and check' : 'Download and verify',
    checkOnly ? 'Check every rule (installing is not enabled in this build)' : 'Install on the receiver'];
  return h('ol', { class: 'steps' }, names.map((text, i) => {
    let cls = i < at ? 'done' : i === at && state !== 'ready' ? 'now' : '';
    if (i === at && (state === 'failed' || state === 'refused')) cls = 'na';
    return h('li', { class: cls }, (i + 1) + '. ' + text + (cls === 'done' ? ' ✓' : ''));
  }));
}

/* The receiver's line when nothing is offered, against the latest release's package for its model (g: u.gnss, app: u.app). */
function gnssReleaseText(g, app) {
  if (!app.latest) return 'No release information yet.';
  if (!g.release) return 'The latest release has no firmware for this receiver.';
  if (!g.running_build || !g.release_build) return 'The latest release has ' + fwText(g.release, g.release_build) + '.';
  return g.running_build > g.release_build ? 'Up to date: newer than ' + fwText(g.release, g.release_build) + ' in the latest release.' :
    'Up to date.';
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

/* A build with another partition layout (rule 12): its own confirmation, or why this app cannot install it. */
const XL_CHECK = 'I understand that this build changes the receiver\'s partition layout.';
function crossLayoutMsg(allowed) {
  return msg('warn', allowed ?
    'This build changes the receiver\'s partition layout. If the update stops partway, the station may not be able to finish it on its own.' :
    'This build changes the receiver\'s partition layout, which this app version cannot install.');
}

function receiverWarnings(flashAvailable, reason) {
  return [
    h('ul', null,
      h('li', null, 'Do not power off the station or plug anything into the receiver\'s USB port.'),
      h('li', null, 'The station profile is applied to the receiver again afterwards.')),
    flashAvailable ? null : msg('warn', notEnabled(reason) + ' The station verifies the package and checks it against every rule, then releases it. ' +
      'Nothing is sent to the receiver.'),
  ];
}

/* Receiver maintenance (Settings, Install manually): the duration, what it does and what a configuration saves, then start. */
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
    let flashFresh = false, flashReadError = '';
    let rflow = null;          /* receiver flow: {state, text, tone} */
    let gnssPick = 0;          /* receiver build chosen in the list of offered builds (0: the newest) */
    let gnssChoose = null;     /* that list, while one shows */
    let gone = false, revision = 0, mutations = 0, reading = null, readAgain = false;
    let progressPhase = '', receiverProgress = 0;
    let attempt = null;
    let stagedDeadline = 0, stagedTimer = 0;
    let appMain = false;       /* the app row shows the card's main button */
    let mainShown = false;     /* the app or receiver row does */
    let maintKey = null;
    const offs = [];
    const P = {
      check: progressWidget(), app_update: progressWidget(), app_upload: progressWidget(),
      gnss_update: progressWidget(), gnss_flash: progressWidget(), gnss_upload: progressWidget(), gnss_check: progressWidget(),
    };
    const headBox = h('div'), appBox = h('div'), gnssBox = h('div'), maintBox = h('div'), rbBox = h('div'), rolledBox = h('div');
    const rbErr = errBox();
    const sessionCard = h('section', { class: 'card wide tight-kv', hidden: true });
    const stagedBox = h('div', { class: 'sect' });
    const appErr = errBox(), gnssErr = errBox(), sessionErr = errBox(), maintErr = errBox();
    const maintLeft = h('span');

    const appControls = createAppUpdates({ getUpdate: () => u, load, P, headBox, appBox, rbBox, rbErr, rolledBox, appErr });
    const { renderCheck, renderApp, finishChecks, cancelDownload, appPick, appUploadErr } = appControls;
    const tooBig = uploadTooBig;

    /* Age changes on every read; it is not a new result. Without a job id,
     * an identical old result cannot be attributed to another attempt. */
    const resultKey = r => r ? JSON.stringify(Object.keys(r).filter(k => k !== 'ended_age_s').sort().map(k => [k, r[k]])) : '';
    const rememberAttempt = (f = flash, next = u) => ({ last: resultKey(f && f.last), download: resultKey(next && next.last_result), stationSeen: false });
    const hideReceiverProgress = () => { P.gnss_update.hide(); P.gnss_check.hide(); P.gnss_flash.hide(); };
    async function receiverAction(fn, accepted) {
      const own = ++revision;
      progressPhase = '';
      mutations++;
      const previous = attempt;
      attempt = rememberAttempt();
      try {
        const r = await fn();
        if (!gone && accepted) accepted(r, own === revision);
        return r;
      } catch (e) {
        /* A refused request started no new flow. A transport failure is
         * uncertain; the next snapshots establish what the station did. */
        if (own === revision && e instanceof ApiError && e.status >= 400) attempt = previous;
        throw e;
      } finally {
        mutations--;
        revision++;
        if (!gone) load().catch(() => {});
      }
    }

    function reconcileReceiver(next, f, keepProgress) {
      if (!f) return;   /* a failed read is not evidence that the operation ended */
      const installing = f.session_running || f.journal === 'running';
      const checking = f.check_running;
      const downloading = next.gnss_job_active || next.state === 'downloading_gnss';
      if (installing || checking || downloading) {
        if (!attempt || attempt.done) attempt = rememberAttempt(f, next);
        const upload = !!(rflow && rflow.upload);
        if (!keepProgress) hideReceiverProgress();
        if (installing) {
          attempt.stationSeen = true;
          rflow = { state: 'install', upload: upload };
          /* Before the journal exists the previous session's counters may
           * remain in the progress snapshot. Show only the accepted work. */
          if (!keepProgress) P.gnss_flash.set(Object.assign({ job: 'gnss_flash' }, f.journal === 'running' ? f.progress : { phase: 'checking' }, { done: false }));
        } else if (checking) {
          attempt.stationSeen = true;
          rflow = { state: 'check', upload: upload };
          if (!keepProgress) P.gnss_check.set({ job: 'gnss_check', phase: 'checking', done: false });
        } else {
          rflow = { state: 'download', upload: upload };
          if (!keepProgress) P.gnss_update.set({ job: 'gnss_update', phase: 'downloading', pct: next.state === 'downloading_gnss' ? next.progress_pct : null, done: false });
        }
        return;
      }
      /* Both fields must be present: an older snapshot cannot establish
       * that accepted, queued work has finished. */
      if (!attempt || next.gnss_job_active !== false || f.session_running !== false || f.check_running !== false) return;
      hideReceiverProgress();
      const last = f.last;
      const lr = next.last_result;
      if (last && (attempt.stationSeen || resultKey(last) !== attempt.last)) {
        rflow = { state: last.outcome === 'success' ? 'installed' : last.outcome === 'checked' ? 'checked' : 'refused',
          tone: outcomeTone(last.outcome), text: outcomeText(last), upload: !!(rflow && rflow.upload) };
      } else if (lr && lr.job === 'gnss_update' && lr.result !== 'ok' && lr.result !== 'not_available_yet' && resultKey(lr) !== attempt.download) {
        rflow = lr.result === 'cancelled' ? { state: 'cancelled', tone: 'info', text: RX_CANCELLED } :
          { state: 'failed', tone: resultTone(lr), text: 'Receiver package: ' + resultText(lr) + '.' };
      } else if (attempt.terminal) {
        rflow = attempt.terminal;   /* a final WS outcome is evidence too, even without a trusted clock */
      } else {
        rflow = { state: 'unknown', tone: 'warn', text: f.journal !== 'none' ? 'The receiver update session needs attention; see below.' :
          'The update is no longer running, but its final result is unavailable.' };
      }
      attempt.done = true;
      if (rflow.state !== 'unknown') attempt.terminal = rflow;
    }

    /* ---- Receiver (offered packages) ---- */
    /* The receiver builds offered, newest first; a snapshot without the list offers its one latest build. */
    function gnssOffers(g) {
      if (!g.available) return [];
      if (Array.isArray(g.offers) && g.offers.length) return g.offers;
      return g.latest_build ? [{ version: g.latest, build: g.latest_build, expected_downtime_s: g.expected_downtime_s,
        cross_layout: !!g.latest_cross_layout }] : [];
    }

    async function updateReceiver(button, o) {
      const g = u.gnss || {};
      const xl = !!o.cross_layout;
      const ok = await dialog({
        title: 'Update the receiver firmware?',
        ok: g.flash_available ? 'Update receiver' : 'Download and verify',
        check: xl ? XL_CHECK : undefined,
        body: [h('p', null, 'Install ' + o.version + ' (build ' + o.build + ') on the ' + (g.model || 'receiver') + ' (now ' + (g.running || 'unknown') + ').'),
          xl ? crossLayoutMsg(true) : null,
          downtimeRows(o.expected_downtime_s, g.budget_left_s), receiverWarnings(g.flash_available, g.flash_reason)],
      });
      if (!ok) return;
      gnssErr.hidden = true;
      try {
        await busy(button, () => receiverAction(() => api('POST', '/api/update/gnss', { build: o.build, cross_layout_ack: xl }), (r, current) => {
          if (!current) return;
          rflow = { state: 'download' };
          P.gnss_update.set({ job: 'gnss_update', phase: 'downloading', done: false });
          renderGnss();
        }));
      } catch (e) {
        if (e instanceof ApiError && e.code === 'not_offered') load();
        showErr(e, gnssErr);
      }
    }

    /*
     * "Receiver R4.10Build18975: up to date" or the offered build with its button; with several builds offered, a list
     * of them (newest first, the newest chosen) beside the button. The downtime against the Onocoy budget is in the dialog.
     * While an update runs, none of it (what is available, the list, the button): its steps and progress say what happens.
     */
    function renderGnss() {
      const g = u.gnss || {};
      let fin = null;
      const blocked = g.blocked && g.blocked !== 'none';
      /* The open session is this update itself while it runs: the steps below say so. */
      const running = g.blocked === 'journal_open' && !!(rflow || (flash && flash.journal === 'running'));
      const offers = gnssOffers(g);
      /* The build chosen in the list, kept over refreshes while it is offered; else the newest. */
      const pick = offers.find(o => o.build === gnssPick) || offers[0] || null;
      /* Another partition layout that this app refuses (rule 12): no button, the reason instead. */
      const xlRefused = !!pick && !!pick.cross_layout && !g.cross_layout_allowed;
      const held = blocked || (rflow && rflow.state === 'download');
      /* An update downloading, installing or checking its package: its steps show instead of the list and the button. */
      const updating = (!!rflow && ['download', 'install', 'check'].includes(rflow.state)) || !!(flash && flash.journal === 'running');
      const upd = pick && !xlRefused && !updating ? btn(g.flash_available ? 'Update receiver' : 'Download and verify', () => updateReceiver(upd, pick), appMain ? '' : 'primary',
        { disabled: held }) : null;
      /* While the package downloads (queued, or before its last byte): Cancel in the button's place. */
      const cancelRx = updating && u.cancellable && u.gnss_job_active ? btn('Cancel', () => cancelDownload(cancelRx,
        h('p', null, 'The station stops downloading the receiver package. Nothing is sent to the receiver.'), gnssErr)) : null;
      const choose = offers.length > 1 && !updating ? selectInput(offers.map(o => [o.build, fwText(o.version, o.build)]), pick.build,
        { 'aria-label': 'Receiver firmware to install', disabled: held }) : null;
      if (choose) {
        choose.addEventListener('change', () => {
          gnssPick = Number(choose.value);
          renderGnss();
          if (gnssChoose) gnssChoose.focus();
        });
      }
      gnssChoose = choose;
      const lr = u.last_result;
      const last = flash && flash.last;
      mainShown = appMain || !!upd;
      mount(gnssBox,
        settingRow('Receiver ' + (g.running ? fwText(g.running, g.running_build) : ''),
          updating ? null :   /* what is offered: not while one of them installs */
          offers.length > 1 ? offers.length + ' newer builds are available.' + (g.flash_available ? '' : ' This build can only download and check them.') :
          g.available ? fwText(g.latest, g.latest_build) + ' is available.' + (g.flash_available ? '' : ' This build can only download and check it.') :
            !g.running ? (((S.status || {}).receiver || {}).rx_maintenance ? NOT_CHECKED + '.' : 'Its firmware shows once the station finds it.') :
            gnssReleaseText(g, u.app || {}), choose ? h('div', { class: 'form-actions pick' }, choose, upd) : upd || cancelRx),
        xlRefused && !updating ? crossLayoutMsg(false) : null,
        blocked && !running && upd ? msg('warn', 'Not possible right now: ' + lbl(L.gnssBlock, g.blocked).toLowerCase() +
          (g.blocked_left_s ? ' (about ' + fmt.dur(g.blocked_left_s) + ' left).' : '.'),
          g.blocked === 'app_trial' && firstChecks((u.app || {}).trial) ?
            [' ', fin = btn('Finish checks now', () => finishChecks(fin), 'small')] : null) : null,
        rflow && rflow.state !== 'unknown' && rflow.state !== 'cancelled' ? receiverSteps(rflow.state, rflow.upload, !g.flash_available) : null,
        P.gnss_update.el,
        P.gnss_check.el,
        P.gnss_flash.el,
        rflow && rflow.text ? msg(rflow.tone || 'info', rflow.text) : null,
        !rflow ? lastOutcomeMsg(last, renderGnss) : null,
        !rflow && !last && lr && (lr.job === 'gnss_update' || lr.job === 'gnss_upload') && lr.result !== 'ok' ?
          h('p', { class: 'muted small' }, 'Last attempt: ' + resultText(lr)) : null,
        gnssErr);
    }

    /*
     * Receiver maintenance, under Install manually (from the status, every
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
        const more = btn('Add 30min', async () => {
          const x = await act(more, () => api('POST', '/api/gnss/maintenance', { duration_s: 1800 }), maintErr);
          if (x !== undefined) toast('Receiver maintenance: ' + fmt.dur(x.left_s) + ' left.', 'ok');
        });
        mount(maintBox, settingRow('Receiver maintenance is on', ['The station sends the receiver no commands.', maintLeft],
          h('div', { class: 'form-actions' }, more, btn('End now', endMaintenance, mainShown ? '' : 'primary'))), maintErr);
      } else {
        const start = btn('Start', () => startMaintenance(start, maintErr), 'small');
        mount(maintBox, settingRow('Receiver from a PC', 'Start receiver maintenance first.', start), maintErr);
      }
    }

    /* ---- Receiver update session ---- */
    function renderSession() {
      const f = flash;
      if ((!f && !flashReadError) || (flashFresh && f.journal === 'none' && !f.receiver_in_loader)) {
        sessionCard.hidden = true;
        return;
      }
      sessionCard.hidden = false;
      const stale = !flashFresh ? msg('warn', 'Receiver update status could not refresh. ' +
        (f ? 'The displayed session is the last known state. ' : 'The current session state is unknown. ') +
        'Recovery actions wait for a successful refresh. ' + flashReadError) : null;
      if (!f) {
        mount(sessionCard, cardHead('Receiver update session', badge('Unknown', 'warn')), stale, sessionErr);
        return;
      }
      const rec = f.record;
      const pr = f.progress;
      const acts = f.actions || {};
      const notNow = [];
      const buttons = FLASH_ACTIONS.map(a => {
        const info = acts[a[0]] || { allowed: false };
        const b = btn(a[2], async () => {
          const ok = await dialog({ title: a[3], ok: a[2], okKind: a[0] === 'clear' ? 'danger solid' : 'primary', body: h('p', null, a[4]) });
          if (!ok) return;
          if (gone || !flashFresh || !flash || !((flash.actions || {})[a[0]] || {}).allowed ||
              resultKey(f.record) !== resultKey(flash.record)) {
            if (!gone) toast('Receiver update status changed. Wait for a successful refresh and review the action again.', 'warn');
            return;
          }
          const r = await act(b, () => receiverAction(() => api('POST', '/api/gnss/flash/' + a[1], { confirm: true }), (answer, current) => {
            if (current && a[0] === 'clear') { attempt = null; rflow = null; hideReceiverProgress(); }
          }), sessionErr);
          if (r !== undefined) {
            toast(a[0] === 'clear' ? 'Session cleared.' : 'Started.', 'ok');
            load();
          }
        }, a[0] === 'clear' ? 'danger' : '', { disabled: !flashFresh || !info.allowed,
          title: !flashFresh ? 'Waiting for a successful refresh' : info.allowed ? null : info.why === 'not_available_yet' ? notEnabled(f.flash_reason) : lbl(L.why, info.why) });
        /* Not in this build: the card's own line says it once for both. */
        if (!info.allowed && info.why && info.why !== 'not_available_yet') notNow.push(h('li', null, a[2] + ': not now, ' + lbl(L.why, info.why) + '.'));
        return b;
      });
      /* The chip and one sentence say the state, then what the session recorded and the receiver's own text, all of it shown.
       * No firmware error code ("ESP_ERR_TIMEOUT"): Failure says it in words. */
      const fail = [pr, rec].map(x => x && x.failure).find(x => x && x !== 'none');
      const tries = pr || rec;
      mount(sessionCard,
        h('div', { class: 'card-head', id: 'session' }, h('h2', null, 'Receiver update session'), badge(lbl(L.journal, f.journal), f.journal === 'running' ? 'info' : 'warn')),
        stale,
        f.journal !== 'none' ? h('p', null, (L.journal[f.journal] || ['', ''])[1]) : null,
        f.receiver_in_loader ? msg('warn', L.salert.receiver_in_loader[1]) : null,
        f.package_needed ? msg('warn', packageNeededText(f.package_needed)) : null,
        rec || pr ? kv([
          rec ? ['Expected firmware', rec.expected_fw] : null,
          rec ? ['Before the update', [rec.pre_model, rec.pre_fw].filter(Boolean).join(' ')] : null,
          pr ? ['Phase', lbl(L.flashPhase, pr.phase)] : ['Step', JOURNAL_STATE[rec.state] || rec.state],
          rec && rec.phase && rec.phase !== rec.state ? ['Reached', JOURNAL_STATE[rec.phase] || rec.phase] : null,
          pr ? ['Sent', fmt.bytes(pr.bytes_sent) + ' of ' + fmt.bytes(pr.bytes_total)] : null,
          pr && pr.frames_total ? ['Blocks acknowledged', fmt.num(pr.frames_acked) + ' of ' + fmt.num(pr.frames_total) +
            ' (' + fmt.num(pr.frames_sent) + ' sent)'] : rec ? ['Last acknowledged block', fmt.num(rec.last_acked_block)] : null,
          ['Transfers / loader attempts', fmt.num(tries.transfers) + ' / ' + fmt.num(tries.entry_attempts)],
          pr && pr.retries ? ['Blocks resent', fmt.num(pr.retries)] : null,
          fail ? ['Failure', lbl(L.flashFailure, fail)] : null,
          rec ? ['Package', fmt.bytes(rec.size) + ', SHA-256 ' + String(rec.sha256 || '').slice(0, 16) + '…'] : null,
          rec ? ['Changed', fmt.time(rec.changed_unix)] : null,
        ]) : null,
        pr && pr.bytes_total ? bar(pr.bytes_sent / pr.bytes_total) : null,
        f.receiver_text ? [h('h3', { class: 'sub' }, 'What the receiver sent'), h('pre', { class: 'log short' }, f.receiver_text)] : null,
        f.last && f.last.ended_unix && f.journal === 'none' ? lastOutcomeMsg(f.last, renderSession) : null,
        f.flash_available ? null : msg('', notEnabled(f.flash_reason) + ' Retrying and restarting the receiver from here wait for that; clearing works.'),
        sessionErr,
        h('div', { class: 'form-actions' }, buttons.slice().reverse()),
        notNow.length ? h('ul', { class: 'muted small' }, notNow) : null);
    }

    /* ---- Files ---- */
    const pkgPick = fileButton('Choose file', 'Choose file: receiver package (.pkg)', '.pkg,application/octet-stream', f => uploadPkg(f));
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
      /* The interrupted update's package again (kept in memory only): its own firmware is what it should report. */
      const again = flash && flash.package_needed && flash.record ? flash.record.expected_fw : '';
      const latest = again || (u && u.gnss && u.gnss.latest ? u.gnss.latest : '');
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
      const xl = !!staged.cross_layout;
      const go = xl && !staged.cross_layout_allowed ? null : btn(staged.flash_available ? 'Start the receiver update' : 'Continue', async () => {
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
          check: xl ? XL_CHECK : undefined,
          body: [h('p', null, 'Expected afterwards: ' + v + '.'), xl ? crossLayoutMsg(true) : null, downtimeRows(staged.expected_downtime_s, staged.budget_left_s),
            receiverWarnings(staged.flash_available, staged.flash_reason)],
        });
        if (!ok) return;
        err.hidden = true;
        try {
          await busy(go, () => receiverAction(() => api('POST', '/api/upload/gnss/confirm', { sha256: s.sha256, expected_fw: v, cross_layout_ack: xl }), (r, current) => {
            staged = null;
            if (!current) { renderStaged(); return; }
            if (r && r.check_only) {
              /* No receiver flashing in this build: the station checks the package and releases it. */
              rflow = { state: 'check', upload: true, tone: 'warn', text: checkStarted() };
              P.gnss_check.set({ job: 'gnss_check', phase: 'checking', done: false });
              renderStaged(msg('info', 'The package check started. Its result shows above.'));
            } else {
              rflow = { state: 'install', upload: true };
              renderStaged(msg('info', 'Receiver update started. Its progress shows above.'));
            }
            renderGnss();
          }));
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
        receiverSteps('ready', true),
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
        xl ? crossLayoutMsg(!!staged.cross_layout_allowed) : null,
        downtimeRows(staged.expected_downtime_s, staged.budget_left_s),
        fExp.el,
        shaOk.el,
        staged.flash_available ? null : msg('warn', notEnabled(staged.flash_reason) + ' Continuing checks the package against every rule and releases it; nothing is sent to the receiver.'),
        err,
        h('div', { class: 'form-actions' }, cancel, go));
      tick();
      if (staged) stagedTimer = setInterval(tick, 1000);
    }

    /* The receiver package upload; a board without a package area (pkg_max_bytes null) says so instead. */
    const pkgRow = h('div', null, settingRow('Receiver package (.pkg)', 'Checked first. Nothing reaches the receiver until you confirm.', pkgPick.b),
      pkgPick.input, P.gnss_upload.el, pkgErr, stagedBox);
    const noPkgRow = settingRow('Receiver package (.pkg)', notEnabled('no_package_area'), null);
    noPkgRow.hidden = true;
    function renderPkgRow() {
      const none = !!u && u.pkg_max_bytes === null && !staged;
      pkgRow.hidden = none;
      noPkgRow.hidden = !none;
    }

    /* Install manually: every way in besides the offered release, one list, always shown. */
    const manualBox = h('div', null, h('h3', { class: 'sub' }, 'Install manually'),
      h('div', { class: 'rows' },
        h('div', null, settingRow('App file (.bin)', 'For pre-releases, older versions and your own builds.', appPick.b), appPick.input,
          P.app_upload.el, appUploadErr),
        pkgRow, noPkgRow,
        maintBox,
        rolledBox));
    const updCard = h('section', { class: 'card' }, headBox, h('div', { class: 'rows' }, appBox, gnssBox, manualBox));

    /* ---- Loading and live progress ---- */
    /* The 10 s poll re-renders only when something changed and nobody is
     * working in the card (keyboard focus, a text selection): every render
     * builds its buttons anew. Loads after an action or an event always
     * render. */
    let shownKey = '';
    function load(periodic) {
      if (gone || mutations) return Promise.resolve();
      if (reading) {
        if (!periodic) readAgain = true;
        return reading;
      }
      reading = read(periodic).finally(() => {
        reading = null;
        if (!gone && readAgain) {
          readAgain = false;
          load().catch(() => {});
        }
      });
      return reading;
    }
    async function read(periodic) {
      const own = revision;
      const progress = receiverProgress;
      /* The worker publishes the receiver job before releasing its busy
       * flag. Read in this order so a handoff cannot look idle in both. */
      let next;
      try {
        next = await api('GET', '/api/update');
      } catch (e) {
        if (!gone && own === revision && !mutations) {
          flashFresh = false;
          flashReadError = errText(e);
          renderSession();
        }
        throw e;
      }
      if (gone) return;
      let f = null, failure = '';
      try {
        f = await api('GET', '/api/gnss/flash');
        if (!f || typeof f.journal !== 'string') throw new Error('The receiver update status was incomplete.');
      } catch (e) {
        f = null;
        failure = errText(e);
      }
      if (gone) return;
      const pkg = staged || await api('GET', '/api/upload/gnss').catch(() => null);
      if (gone || own !== revision || mutations) return;
      const sessionKey = () => JSON.stringify([flashFresh, flashReadError, flash && flash.journal, flash && flash.receiver_in_loader, flash && flash.actions, flash && flash.record]);
      const previousSession = sessionKey();
      reconcileReceiver(next, f, progress !== receiverProgress);
      u = next;
      if (f) flash = f;
      flashFresh = !!f;
      flashReadError = failure;
      /* Freshness and admission changes must reach the buttons even while
       * keyboard focus or a selection holds ordinary periodic rendering. */
      if (sessionKey() !== previousSession) renderSession();
      if (!staged && pkg && pkg.staged) {
        staged = pkg;
        stagedDeadline = Date.now() + (staged.staged.expires_in_s || 900) * 1000;
        renderStaged();
      }
      if (u.state === 'downloading_app') P.app_update.set({ job: 'app_update', phase: 'downloading', pct: u.progress_pct, done: false });
      /* A cancelled app download says so in its row (Last attempt), also when no final progress message came. */
      else if (u.state === 'failed' && !u.cancellable && u.last_result && u.last_result.job === 'app_update' &&
        u.last_result.result === 'cancelled') P.app_update.hide();
      const key = JSON.stringify([u, flash, rflow, flashFresh, flashReadError]);
      if (periodic && (key === shownKey || interacting(updCard) || interacting(sessionCard))) return;
      shownKey = key;
      renderPkgRow();
      renderCheck();
      renderApp();
      appMain = appControls.main;
      renderGnss();
      renderSession();
      maintKey = null;
      renderMaint(S.status);
    }

    offs.push(bus.on('progress', m => {
      if (['gnss_update', 'gnss_flash', 'gnss_check'].includes(m.job)) {
        const phase = m.job + ':' + m.phase;
        if (m.done || phase !== progressPhase || (attempt && attempt.done)) revision++;
        progressPhase = m.done ? '' : phase;
        receiverProgress++;
        if (!attempt || (!m.done && attempt.done)) attempt = rememberAttempt();
        /* A fast flash can finish before the download worker's handoff
         * message reaches this page. It cannot reopen that finished flow. */
        if (m.job === 'gnss_update' && m.done && ['ok', 'not_available_yet'].includes(m.result) && attempt.terminal) {
          P.gnss_update.hide();
          load().catch(() => {});
          return;
        }
      }
      const w = P[m.job];
      if (m.done && (m.job === 'check' || m.result === 'cancelled')) {
        if (w) w.hide();   /* the result shows once, as the row's own message */
      } else if (w) {
        w.set(m);
      }
      if (m.job === 'gnss_update') {
        if (!m.done) {
          rflow = { state: 'download' };
        } else if (m.result === 'not_available_yet') {
          /* No receiver flashing in this build: the package check follows. */
          rflow = { state: 'check', tone: 'warn', text: notYetDownloaded() };
        } else if (m.result === 'ok') {
          rflow = { state: 'install' };
        } else {
          rflow = m.result === 'cancelled' ? { state: 'cancelled', tone: 'info', text: RX_CANCELLED } :
            { state: 'failed', tone: 'bad', text: 'The receiver package could not be prepared: ' + resultText(m) + '. Nothing on the receiver changed.' };
          attempt.terminal = rflow;
          attempt.done = true;
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
          attempt.terminal = rflow;
          attempt.done = true;
        }
        renderGnss();
      }
      if (m.done) load().catch(() => {});
    }));
    offs.push(bus.on('event', m => {
      if (m.event === 'update_state') { revision++; load().catch(() => {}); }
    }));

    mount(headBox, cardHead('Updates'));
    mount(appBox, h('p', { class: 'loading' }, 'Loading…'));
    renderMaint(S.status);
    const ready = load().catch(e => mount(updCard, msg('bad', errText(e)), btn('Try again', () => openView('settings', arg), 'primary')));
    const stop = poller(10000, () => load(true), true);

    return {
      ready: ready,
      cards: { updates: updCard, session: sessionCard },
      /* The revert row, for the Danger zone. */
      revert: rbBox,
      update: renderMaint,
      destroy() {
        gone = true;
        revision++;
        readAgain = false;
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

function stationProgress(m, frac) {
  const tone = m.done ? resultTone(m) : '';
  if (m.done) frac = tone === 'ok' || m.result === 'not_available_yet' ? 1 : frac;
  /* The receiver has the new firmware; the station puts its settings back and checks them: no count to show. */
  const finishing = m.job === 'gnss_flash' && !m.done && m.phase === 'finishing';
  if (finishing) frac = null;
  const counts = m.job === 'gnss_flash' && !m.done && !finishing ? flashCounts(m) : '';
  /* "failed" before the final message: a transfer failed and the session goes on (back to the loader,
   * the same package again within the caps, or a stop for your decision). */
  const phase = m.job === 'gnss_flash' && !m.done && m.phase === 'failed' ? TRANSFER_FAILED : lbl(L.phase, m.phase);
  const text = lbl(L.job, m.job) + ': ' + (m.done ? resultText(m) : finishing ? FINISHING_WAIT : phase +
    (frac !== null ? ' ' + Math.round(frac * 100) + '%' : '') + (counts ? ' (' + counts + ')' : ''));
  return { tone: tone, frac: frac, text: text };
}
