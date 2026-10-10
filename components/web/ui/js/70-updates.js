/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Updates. Every update starts with a click here: the station only checks
 * once a day (or on "Check now") and shows the banner.
 *   App: install the offered release (download, verify, restart, trial), go
 *        back to the previous version, or install a .bin file.
 *   Receiver: download and verify the offered package or upload a .pkg,
 *        confirm its SHA-256 and the expected firmware, then install. While
 *        this firmware cannot flash the receiver yet, the flow ends after
 *        the package was verified and says so; nothing on the receiver
 *        changes.
 *   Session: a receiver update session waiting for a decision (retry,
 *        restart the receiver, clear).
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

const NOT_YET_DOWNLOADED = 'Receiver package downloaded and verified; installing it is not available yet. Nothing on the receiver changed.';
const NOT_YET_UPLOADED = 'Installing receiver firmware is not available in this firmware version yet. The package was released; nothing on the receiver changed.';

function resultText(m) {
  let t = lbl(L.result, m.result);
  if (m.result === 'package_rejected' && m.failed_rule) t += ' (rule ' + m.failed_rule + (m.cross_layout ? ', other partition layout' : '') + ')';
  if (m.result !== 'ok' && m.result !== 'not_available_yet' && m.message) t += ' (' + m.message + ')';
  return t;
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
      if (m.done) frac = m.result === 'ok' || m.result === 'not_available_yet' ? 1 : frac;
      b.className = 'bar ' + (m.done ? (m.result === 'ok' ? 'ok' : m.result === 'not_available_yet' ? 'warn' : 'bad') : '') +
        (frac === null && !m.done ? ' indet' : '');
      fill.style.width = frac === null ? '' : (Math.max(0, Math.min(1, frac)) * 100).toFixed(1) + '%';
      label.textContent = lbl(L.job, m.job) + ': ' + (m.done ? resultText(m) : lbl(L.phase, m.phase) +
        (frac !== null ? ' ' + Math.round(frac * 100) + ' %' : ''));
    },
    hide() {
      el.hidden = true;
    },
  };
}

/**
 * Steps of a receiver update. state: confirm, download, install, na (the
 * install step is not built yet), failed. upload: the manual-upload wording.
 */
function receiverSteps(state, upload) {
  const order = ['confirm', 'download', 'install'];
  const at = state === 'na' || state === 'install' ? 2 : state === 'download' || state === 'failed' ? 1 : 0;
  const names = upload ? ['Upload and check', 'Confirm', 'Install on the receiver'] : ['Confirm', 'Download and verify', 'Install on the receiver'];
  return h('ol', { class: 'steps' }, order.map((s, i) => {
    let cls = i < at ? 'done' : i === at ? 'now' : '';
    let text = names[i];
    if (i === 2 && state === 'na') {
      cls = 'na';
      text += ': not available yet';
    }
    if (i === 1 && state === 'failed') cls = 'na';
    return h('li', { class: cls }, (cls === 'done' ? '✓ ' : '') + text);
  }));
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
    flashAvailable ? null : msg('warn', 'Installing receiver firmware is not available in this firmware version yet. The station downloads ' +
      'and verifies the package, then releases it. Nothing on the receiver changes.'),
  ];
}

Views.updates = {
  title: 'Updates',
  admin: true,
  create(el, arg) {
    let u = null, staged = null, flash = null;
    let rflow = null;          /* receiver flow: {state, text, tone} */
    let stagedDeadline = 0, stagedTimer = 0;
    const offs = [];
    const P = {
      check: progressWidget(), app_update: progressWidget(), app_upload: progressWidget(),
      gnss_update: progressWidget(), gnss_flash: progressWidget(), gnss_upload: progressWidget(),
    };
    const versionsCard = h('section', { class: 'card' });
    const appCard = h('section', { class: 'card' });
    const gnssCard = h('section', { class: 'card' });
    const sessionCard = h('section', { class: 'card wide', hidden: true });
    const filesCard = h('section', { class: 'card wide' });
    const stagedBox = h('div');
    const appErr = errBox(), gnssErr = errBox(), sessionErr = errBox();

    /* ---- Versions and "check now" ---- */
    function renderVersions() {
      const a = u.app || {}, g = u.gnss || {};
      const lr = u.last_result;
      const checkBtn = btn('Check now', () => act(checkBtn, async () => {
        await api('POST', '/api/update/check');
        P.check.set({ job: 'check', phase: 'checking', done: false });
      }), '', { disabled: !u.checks_enabled });
      mount(versionsCard,
        h('h2', null, 'Versions', u.state && u.state !== 'idle' ? badge(lbl(L.updState, u.state), 'info') : null),
        kv([
          ['App', [a.running || DASH, a.dev_build ? ' ' : null, a.dev_build ? badge('development build', 'plain') : null]],
          ['Receiver', g.running ? g.running + (g.model ? ' on ' + g.model : '') : DASH],
          ['Last check', u.last_check_unix ? fmt.time(u.last_check_unix) : 'not yet'],
          lr ? ['Last job', lbl(L.job, lr.job) + ': ' + resultText(lr)] : null,
        ]),
        u.checks_enabled ? h('p', { class: 'muted small' }, 'The station checks for new releases once a day. Nothing is downloaded until you click.') :
          msg('', ERR.checks_disabled),
        P.check.el,
        h('div', { class: 'form-actions' }, checkBtn));
    }

    /* ---- App ---- */
    async function installApp(version, ack, button) {
      const ok = await dialog({
        title: 'Install app ' + version + '?',
        ok: 'Install',
        body: [h('p', null, 'The station downloads ' + version + ', checks it against the signed release list, installs it and restarts. ' +
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

    function renderApp() {
      const a = u.app || {};
      const trial = a.trial || {};
      const parts = [];
      if (a.reverted_in_stage1 && a.reverted) {
        const again = btn('Install ' + a.reverted + ' again', () => installApp(a.reverted, false, again), 'primary');
        parts.push(msg('warn', 'Update ' + a.reverted + ' was rolled back during its first minute (a crash or a power blip). If it rolls back again, it is not offered any more.',
          h('div', { class: 'form-actions' }, again)));
      }
      if (a.available && a.latest && !(a.reverted_in_stage1 && a.reverted === a.latest)) {
        const inst = btn('Install ' + a.latest, () => installApp(a.latest, false, inst), 'primary');
        parts.push(msg('info', 'Version ' + a.latest + ' is available.', h('div', { class: 'form-actions' }, inst)));
      } else if (a.latest_rolled_back && a.latest) {
        const anyway = btn('Install ' + a.latest + ' anyway', () => installApp(a.latest, true, anyway));
        parts.push(msg('warn', 'The latest release, ' + a.latest + ', was rolled back on this station before, so it is not offered.',
          h('div', { class: 'form-actions' }, anyway)));
      } else if (a.dev_build) {
        parts.push(h('p', { class: 'muted' }, 'Development builds are not offered releases. Install a release from a file below.'));
      } else {
        parts.push(h('p', { class: 'muted' }, a.latest ? 'This is the latest release.' : 'No release information yet.'));
      }
      const rb = btn('Go back to the previous version', async () => {
        const ok = await dialog({ title: 'Go back to the previous version?', ok: 'Go back', okKind: 'danger solid',
          body: h('p', null, 'The station marks ' + a.running + ' as rolled back and restarts into the version installed before it. ' +
            a.running + ' is not offered again; you can still install it by hand.') });
        if (!ok) return;
        const r = await act(rb, () => api('POST', '/api/update/rollback', { confirm: true }), appErr);
        if (r !== undefined) showRestarting('Going back to the previous version');
      }, 'danger small');
      mount(appCard,
        h('h2', null, 'App', a.available ? badge('Update available', 'info') : null),
        kv([
          ['Running', a.running],
          ['Latest release', a.latest || DASH],
          trial.stage && trial.stage !== 'none' ? ['Trial', lbl(L.trial, trial.stage) + (trial.left_s ? ', ' + fmt.dur(trial.left_s) + ' left' : '')] : null,
        ]),
        parts,
        P.app_update.el,
        appErr,
        a.rollback_available ? h('div', { class: 'form-actions' }, rb) : null);
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

    function renderGnss() {
      const g = u.gnss || {};
      const blocked = g.blocked && g.blocked !== 'none';
      const upd = btn(g.flash_available ? 'Update receiver…' : 'Download and verify…', () => updateReceiver(upd), 'primary',
        { disabled: blocked || !g.available || (rflow && rflow.state === 'download') });
      const lr = u.last_result;
      mount(gnssCard,
        h('h2', null, 'Receiver firmware', g.available ? badge('Update available', 'info') : null),
        kv([
          ['Receiver', g.model],
          ['Running', g.running ? g.running + (g.running_build ? ' (build ' + g.running_build + ')' : '') : DASH],
          ['Latest release', g.latest ? g.latest + ' (build ' + g.latest_build + ')' : DASH],
        ]),
        g.available ? downtimeRows(g.expected_downtime_s, g.budget_left_s) : h('p', { class: 'muted' }, g.latest ? 'No newer receiver firmware is offered.' : 'No release information yet.'),
        blocked ? msg('warn', 'Not possible right now: ' + lbl(L.gnssBlock, g.blocked).toLowerCase() +
          (g.blocked_left_s ? ' (about ' + fmt.dur(g.blocked_left_s) + ' left).' : '.')) : null,
        g.flash_available ? null : msg('', 'Installing receiver firmware is not available in this firmware version yet. You can download and verify the offered package; nothing on the receiver changes.'),
        rflow ? receiverSteps(rflow.state) : null,
        P.gnss_update.el,
        P.gnss_flash.el,
        rflow && rflow.text ? msg(rflow.tone || 'info', rflow.text) : null,
        !rflow && lr && (lr.job === 'gnss_update' || lr.job === 'gnss_upload') ? h('p', { class: 'muted small' }, 'Last attempt: ' + resultText(lr)) : null,
        gnssErr,
        g.available ? h('div', { class: 'form-actions' }, upd) : null);
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
        return h('div', null, b, !info.allowed && info.why ? h('small', { class: 'hint' }, 'Not now: ' + lbl(L.why, info.why)) : null);
      });
      mount(sessionCard,
        h('h2', { id: 'session' }, 'Receiver update session', badge(lbl(L.journal, f.journal), f.journal === 'running' ? 'info' : 'warn')),
        f.journal !== 'none' ? h('p', null, (L.journal[f.journal] || ['', ''])[1]) : null,
        f.receiver_in_loader ? msg('warn', L.salert.receiver_in_loader[1]) : null,
        rec ? kv([
          ['Step', JOURNAL_STATE[rec.state] || rec.state],
          ['Phase', lbl(L.flashPhase, rec.phase)],
          ['Package', fmt.bytes(rec.size) + ', SHA-256 ' + String(rec.sha256 || '').slice(0, 16) + '…'],
          ['Expected firmware', rec.expected_fw],
          ['Before the update', [rec.pre_model, rec.pre_fw].filter(Boolean).join(' ')],
          ['Transfers / entry attempts', rec.transfers + ' / ' + rec.entry_attempts],
          ['Last acknowledged block', fmt.num(rec.last_acked_block)],
          rec.failure && rec.failure !== 'none' ? ['Failure', lbl(L.flashFailure, rec.failure)] : null,
          ['Changed', fmt.time(rec.changed_unix)],
        ]) : null,
        pr ? [h('h3', { class: 'section-title' }, 'Progress'),
          pr.bytes_total ? bar(pr.bytes_sent / pr.bytes_total) : null,
          kv([
            ['Phase', lbl(L.flashPhase, pr.phase)],
            ['Sent', fmt.bytes(pr.bytes_sent) + ' of ' + fmt.bytes(pr.bytes_total)],
            ['Retries', fmt.num(pr.retries)],
            pr.failure && pr.failure !== 'none' ? ['Failure', lbl(L.flashFailure, pr.failure)] : null,
            pr.last_err ? ['Last error', pr.last_err] : null,
          ])] : null,
        f.receiver_text ? h('details', { class: 'more' }, h('summary', null, 'Receiver output'), h('pre', { class: 'log' }, f.receiver_text)) : null,
        f.flash_available ? null : msg('', 'Flashing the receiver is not available in this firmware version yet, so retrying and restarting wait for a later version.'),
        sessionErr,
        h('div', { class: 'form-actions' }, buttons));
    }

    /* ---- Files ---- */
    const appFile = h('input', { type: 'file', accept: '.bin,application/octet-stream' });
    const appUploadBtn = btn('Install this file', () => uploadApp(false));
    const appUploadErr = errBox();
    async function uploadApp(anyProject) {
      const file = appFile.files && appFile.files[0];
      if (!file) {
        say(appUploadErr, 'Choose a .bin file first.');
        return;
      }
      if (!anyProject) {
        const ok = await dialog({ title: 'Install ' + file.name + '?', ok: 'Install',
          body: [h('p', null, fmt.bytes(file.size) + '. The station writes it to its spare app slot and restarts into it. ' +
            'The new version runs on trial and goes back to this one if it fails.')] });
        if (!ok) return;
      }
      appUploadErr.hidden = true;
      let r;
      try {
        r = await busy(appUploadBtn, () => upload('/api/upload/app' + (anyProject ? '?any_project=1' : ''), file, (sent, total) =>
          P.app_upload.set({ job: 'app_upload', phase: 'receiving', bytes: sent, total: total, done: false })));
      } catch (e) {
        P.app_upload.hide();
        if (e instanceof ApiError && e.code === 'not_this_project' && !anyProject) {
          const ok = await dialog({ title: 'Firmware for another project?', ok: 'Install anyway', okKind: 'danger solid',
            body: h('p', null, 'This image does not say it is firmware for this station. Firmware for something else can leave the station ' +
              'unreachable until it is flashed over USB.'), check: 'I know what this image is.' });
          if (ok) uploadApp(true);
          return;
        }
        showErr(e, appUploadErr);
        return;
      }
      showRestarting('Installing ' + (r && r.version ? r.version : 'the new app'),
        'The image was written' + (r && r.sha256 ? ' (SHA-256 ' + r.sha256.slice(0, 16) + '…)' : '') +
        '. The station restarts into it now; this page reconnects by itself.');
    }

    const pkgFile = h('input', { type: 'file', accept: '.pkg,application/octet-stream' });
    const pkgUploadBtn = btn('Upload and check', () => uploadPkg());
    const pkgErr = errBox();
    async function uploadPkg() {
      const file = pkgFile.files && pkgFile.files[0];
      if (!file) {
        say(pkgErr, 'Choose a .pkg file first.');
        return;
      }
      pkgErr.hidden = true;
      try {
        staged = await busy(pkgUploadBtn, () => upload('/api/upload/gnss', file, (sent, total) =>
          P.gnss_upload.set({ job: 'gnss_upload', phase: sent < total ? 'receiving' : 'inspecting', bytes: sent, total: total, done: false })));
        P.gnss_upload.hide();
        stagedDeadline = Date.now() + (staged.staged.expires_in_s || 900) * 1000;
        pkgFile.value = '';
        renderStaged();
      } catch (e) {
        P.gnss_upload.hide();
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
          await busy(go, () => api('POST', '/api/upload/gnss/confirm', { sha256: s.sha256, expected_fw: v, cross_layout_ack: false }));
          staged = null;
          rflow = { state: 'install' };
          renderStaged(msg('info', 'Receiver update started. Progress shows under Receiver firmware.'));
          renderGnss();
        } catch (e) {
          if (e instanceof ApiError && e.code === 'not_available_yet') {
            staged = null;
            renderStaged([receiverSteps('na', true), msg('warn', NOT_YET_UPLOADED)]);
            return;
          }
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
        staged.flash_available ? null : msg('warn', 'Installing receiver firmware is not available in this firmware version yet.'),
        err,
        h('div', { class: 'form-actions' }, go, cancel));
      tick();
      if (staged) stagedTimer = setInterval(tick, 1000);
    }

    mount(filesCard,
      h('h2', null, 'Install from a file'),
      h('h3', { class: 'section-title' }, 'App image (.bin)'),
      h('p', { class: 'muted small' }, 'For pre-releases, downgrades and self-built firmware.'),
      h('div', { class: 'row' }, appFile, appUploadBtn),
      P.app_upload.el,
      appUploadErr,
      h('h3', { class: 'section-title' }, 'Receiver package (.pkg)'),
      h('p', { class: 'muted small' }, 'The station stores and checks the package first. Nothing is sent to the receiver until you confirm.'),
      h('div', { class: 'row' }, pkgFile, pkgUploadBtn),
      P.gnss_upload.el,
      pkgErr,
      stagedBox);

    /* ---- Loading and live progress ---- */
    async function load() {
      const res = await Promise.all([
        api('GET', '/api/update'),
        api('GET', '/api/gnss/flash').catch(() => null),
        staged ? Promise.resolve(staged) : api('GET', '/api/upload/gnss').catch(() => null),
      ]);
      u = res[0];
      flash = res[1];
      if (!staged && res[2]) {
        staged = res[2];
        stagedDeadline = Date.now() + (staged.staged.expires_in_s || 900) * 1000;
        renderStaged();
      }
      if (u.state === 'downloading_gnss' && !rflow) {
        rflow = { state: 'download' };
        P.gnss_update.set({ job: 'gnss_update', phase: 'downloading', pct: u.progress_pct, done: false });
      }
      if (u.state === 'downloading_app') P.app_update.set({ job: 'app_update', phase: 'downloading', pct: u.progress_pct, done: false });
      renderVersions();
      renderApp();
      renderGnss();
      renderSession();
    }

    offs.push(bus.on('progress', m => {
      const w = P[m.job];
      if (w) w.set(m);
      if (m.job === 'gnss_update') {
        if (!m.done) {
          rflow = { state: 'download' };
        } else if (m.result === 'not_available_yet') {
          rflow = { state: 'na', tone: 'warn', text: NOT_YET_DOWNLOADED };
        } else if (m.result === 'ok') {
          rflow = { state: 'install' };
        } else {
          rflow = { state: 'failed', tone: 'bad', text: 'The receiver package could not be prepared: ' + resultText(m) + '. Nothing on the receiver changed.' };
        }
        if (u) renderGnss();
      }
      if (m.job === 'gnss_flash' && m.done && u) {
        rflow = m.result === 'ok' ? { state: 'install', tone: 'ok', text: 'Receiver firmware installed.' } :
          { state: 'install', tone: 'bad', text: 'Receiver update: ' + resultText(m) };
        renderGnss();
      }
      if (m.done) load().catch(() => {});
    }));
    offs.push(bus.on('event', m => {
      if (m.event === 'update_state') load().catch(() => {});
    }));

    mount(el, h('div', { class: 'grid' }, versionsCard, appCard, gnssCard, sessionCard, filesCard));
    mount(versionsCard, h('p', { class: 'loading' }, 'Loading…'));
    load().then(() => {
      if (arg === 'session' && !sessionCard.hidden) sessionCard.scrollIntoView({ block: 'start' });
    }, e => mount(versionsCard, msg('bad', errText(e)), btn('Try again', () => openView('updates', arg), 'primary')));
    const stop = poller(10000, () => load(), true);

    return {
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
