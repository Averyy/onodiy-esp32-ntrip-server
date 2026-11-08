/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */

/* ---- The last update check, in words ---- */

/** {tone, text} for a failed check (GET /api/update), or null when it went well (or none ran). The firmware's error code is in the log. */
function checkFailure(u) {
  const r = u.last_check_result;
  if (!r || r === 'ok') return null;
  const repo = u.release_repo ? 'github.com/' + u.release_repo : 'the release repository';
  const retry = u.next_check_in_s === null || u.next_check_in_s === undefined ? '' :
    u.next_check_in_s <= 0 ? productText(' The {product} tries again as soon as it is online.') : productText(' The {product} tries again in ') + inMinutes(u.next_check_in_s) + '.';
  const http = u.last_http_status ? ' (HTTP ' + u.last_http_status + ')' : '';
  let text;
  switch (r) {
    case 'no_release':
      text = 'No release found at ' + repo + http + '. The repository may be private or have no releases yet.';
      break;
    case 'no_response':
      text = 'GitHub did not answer.' + (retry || productText(' The {product} tries again later.'));
      break;
    case 'http_error':
      text = 'GitHub answered with an error' + http + productText('. The {product} tries again with the next daily check.');
      break;
    case 'untrusted_redirect':
      text = productText('GitHub sent the request somewhere this firmware does not trust, so it stopped there. The {product} tries again with the next daily check.');
      break;
    case 'bad_manifest':
      text = 'The latest release at ' + repo + ' has no release manifest this build accepts.';
      break;
    default:
      text = 'The update check failed (' + r + ').';
  }
  return { tone: 'warn', text: text };
}

/* "Checking the new version, 23h 59min left: confirming the start (15s)." The checks' whole time left, then the
 * stage, with its own time while another stage follows it. */
const TRIAL_NOW = { confirming: 'confirming the start', verifying: 'verifying it works as before', watching: 'watching for crashes' };
function trialText(t) {
  const end = t.ends_in_s || t.left_s;
  const own = t.stage !== 'watching' && t.left_s && t.left_s !== end ? ' (' + fmt.dur(t.left_s) + ')' : '';
  return 'Checking the new version' + (end ? ', ' + fmt.dur(end) + ' left' : '') + ': ' + (TRIAL_NOW[t.stage] || t.stage) + own + '.';
}

/* The first checks (confirming, verifying) hold receiver updates back; the crash watch does not. */
const firstChecks = t => !!t && (t.stage === 'confirming' || t.stage === 'verifying');

/** "2h ago": the age of an update check in whole minutes, hours or days. */
function checkedAgo(unix) {
  const s = Date.now() / 1000 - unix;
  return s < 90 ? 'just now' : s < 5400 ? Math.round(s / 60) + 'min ago' : s < 172800 ? Math.round(s / 3600) + 'h ago' :
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

/*
 * A file chooser that looks and works like the other buttons (the
 * browser's own input stays hidden): onFile(file) once one is chosen; name:
 * the label with what it chooses, for assistive technology. {input, b}:
 * both go on the page.
 */
function fileButton(label, name, accept, onFile) {
  const input = h('input', { type: 'file', accept: accept, hidden: true, 'aria-label': name, onchange: () => {
    const f = input.files && input.files[0];
    input.value = '';   /* the same file again fires again */
    if (f) onFile(f);
  } });
  return { input: input, b: btn(label, () => input.click(), 'small', { 'aria-label': name }) };
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
      const info = UI.progress ? UI.progress(m, frac) : {
        tone: m.done ? (m.result === 'ok' ? 'ok' : 'bad') : '', frac: frac,
        text: (m.job || '') + ': ' + (m.done ? UI.updateResultText(m) : m.phase || '') +
          (frac !== null && !m.done ? ' ' + Math.round(frac * 100) + '%' : ''),
      };
      b.className = 'bar ' + info.tone + (info.frac === null && !m.done ? ' indet' : '');
      fill.style.width = info.frac === null ? '' : (Math.max(0, Math.min(1, info.frac)) * 100).toFixed(1) + '%';
      label.textContent = info.text;
    },
    hide() {
      el.hidden = true;
    },
  };
}

/* Only app controls live here. Receiver/session/maintenance panels are
 * selected by the product composition and own their service requests. */
function createAppUpdates({ getUpdate, load, P, headBox, appBox, rbBox, rbErr, rolledBox, appErr }) {
  let appMain = false;
  const model = { get update() { return getUpdate(); } };
    /* ---- The card's title row: the last check and "Check now" (a tool, like Refresh) ---- */
    function renderCheck() {
      const fail = checkFailure(model.update);
      const checkBtn = model.update.checks_enabled ? btn('Check now', () => act(checkBtn, async () => {
        await api('POST', '/api/update/check');
        P.check.set({ job: 'check', phase: 'checking', done: false });
      }), 'small') : null;
      /* The last attempt, successful or not; the last success when no attempt ran this start. */
      const at = model.update.last_attempt_unix || model.update.last_check_unix;
      const head = cardHead('Updates', model.update.state && model.update.state !== 'idle' && model.update.state !== 'failed' ? badge(UI.updateState(model.update.state), 'info') : null,
        checkBtn ? h('div', { class: 'tools' }, h('span', { class: 'muted small' }, at ? 'Checked ' + checkedAgo(at) +
          (fail && model.update.last_attempt_unix ? ', failed' : '') : 'Not checked yet'), checkBtn) : null);
      head.classList.add('fit');
      mount(headBox, head,
        model.update.checks_enabled ? null : h('p', { class: 'muted small' }, ERR.checks_disabled),
        fail && model.update.state !== 'checking' ? msg(fail.tone, fail.text) : null,
        P.check.el);
    }

    /* ---- App ---- */
    async function installApp(version, ack, button) {
      const ok = await dialog({
        title: 'Install app ' + version + '?',
        ok: 'Install',
        body: [h('p', null, productText('The {product} downloads ') + version + ' from the release over HTTPS, checks its size and SHA-256 against the release\'s manifest, installs it and restarts. ' +
          productText('The {product} then checks the new version: if it fails, it goes back to this version by itself.')),
        UI.appDowntime ? UI.appDowntime() : null],
        check: ack ? productText('I know this version was rolled back on this {product} before.') : null,
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

    /* The user stops the download running (the app image or a receiver package) before its last byte: the station
     * ends it as "cancelled" (the final progress message and last_result say so). */
    async function cancelDownload(button, body, out) {
      const ok = await dialog({ title: 'Cancel the download?', ok: 'Cancel download', cancel: 'Keep downloading', body: body });
      if (!ok) return;
      out.hidden = true;
      try {
        await busy(button, () => api('POST', '/api/update/cancel'));
      } catch (e) {
        showErr(e, out);
      }
      load().catch(() => {});
    }

    function cancelApp(button) {
      const a = model.update.app || {};
      const rb = a.rollback_available && a.rollback_version && a.rollback_version !== a.running;
      return cancelDownload(button, [
        h('p', null, productText('The {product} stops downloading and keeps running ') + (a.running || 'this version') + '. Nothing is installed.'),
        rb ? h('p', { class: 'muted' }, 'Once a download has started, Revert to ' + a.rollback_version +
          ' is no longer offered: the download writes over it.') : null], appErr);
    }

    /* The user ends the new version's first checks (it works): receiver updates are possible at once. */
    async function finishChecks(button) {
      const ok = await dialog({
        title: 'Finish the checks now?',
        ok: 'Finish checks',
        body: h('p', null, UI.finishChecksText),
      });
      if (!ok) return;
      appErr.hidden = true;
      try {
        await busy(button, () => api('POST', '/api/update/accept', { confirm: true }));
        setTimeout(() => load(), 1500);   /* the update task takes it within a second */
      } catch (e) {
        if (e instanceof ApiError && e.code === 'no_app_check') load();
        showErr(e, appErr);
      }
    }

    /* "App v0.3.0: up to date" or what is offered, with its one button; going back is under Install manually. */
    function renderApp() {
      const a = model.update.app || {};
      const trial = a.trial || {};
      const lr = model.update.last_result;
      /* An app download queued or running (cancellable to its last byte), its checks, an upload, the restart: Cancel while
       * that is possible, else no button (the station would refuse a second install as busy). */
      const appCancellable = !!model.update.cancellable && !UI.receiverUpdateActive(model.update);
      const appBusy = appCancellable || ['downloading_app', 'uploading_app', 'reboot_pending'].includes(model.update.state);
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
        text = a.dev_build ? 'Development build: no newer release.' : a.latest ? 'Up to date.' : 'No release information yet.';
      }
      if (trial.stage && trial.stage !== 'none') text += ' ' + trialText(trial);
      if (appBusy) b = appCancellable ? btn('Cancel', () => cancelApp(b)) : null;
      if (!b && !appBusy && firstChecks(trial)) b = btn('Finish checks now', () => finishChecks(b));
      appMain = !!b && b.classList.contains('primary');
      mount(appBox, settingRow('App ' + (a.running || ''), text, b),
        /* The attempt before: not while a new one runs (its progress says what happens). */
        !appBusy && lr && ['app_update', 'app_upload', 'rollback'].indexOf(lr.job) >= 0 && lr.result !== 'ok' ?
          h('p', { class: 'muted small' }, 'Last attempt: ' + UI.updateResultText(lr)) : null,
        P.app_update.el, appErr);
      /* The version the station would start: the other app slot's, else (unreadable) the one before this. */
      const to = a.rollback_version || 'the version installed before ' + a.running;
      /* Nothing to go back to when the other slot holds this same version (a reinstall). */
      const rb = a.rollback_available && a.rollback_version !== a.running ? btn('Revert', async () => {
        const ok = await dialog({ title: 'Revert to ' + to + '?', ok: 'Revert', okKind: 'danger solid',
          body: h('p', null, productText('The {product} restarts into ') + to + '. ' + a.running +
            ' is not offered again; you can still install it from a file.') });
        if (!ok) return;
        const r = await act(rb, () => api('POST', '/api/update/rollback', { confirm: true }), rbErr);
        if (r !== undefined) showRestarting('Reverting to ' + to);
      }, 'danger') : null;
      /* The revert row is a row of the Danger zone (Settings shows it there); the versions rolled back stay here. */
      mount(rbBox, rb ? settingRow('Revert firmware', 'Revert to ' + to + '.', rb) : null, rb ? rbErr : null);
      rbBox.hidden = !rb;
      mount(rolledBox, (a.rolled_back || []).length ?
        h('p', { class: 'muted small' }, 'Rolled back here, not offered again: ' + a.rolled_back.join(', ') + '.') : null);
      rolledBox.hidden = !rolledBox.firstChild;
    }

    /* An image that does not name this project: install only after the warning (?any_project=1). */
    function anotherProject(project) {
      return dialog({ title: 'Firmware for another project?', ok: 'Install anyway', okKind: 'danger solid',
        body: h('p', null, (project ? 'This image says it is firmware for "' + project + productText('", not for this {product}.') :
          project === '' ? 'This image names no project.' : productText('This image does not say it is firmware for this {product}.')) +
          productText(' Firmware for something else can leave the {product} unreachable until it is flashed over USB.')),
        check: 'I know what this image is.' });
    }
    /* The same image as the one running, or a version rolled back here before: install only after asking. */
    const sameOk = v => dialog({ title: 'Install the same version again?', ok: 'Install again',
      body: h('p', null, 'This is the version already running' + (v ? ' (' + v + ')' : '') + '. Install it again? ' +
        productText('The {product} restarts into it; nothing else changes.')) });
    const rolledBackOk = v => dialog({ title: 'Install a version that was rolled back?', ok: 'Install anyway',
      body: h('p', null, (v ? 'Version ' + v : 'This version') + productText(' was rolled back on this {product} before: its checks failed, or someone ') +
        productText('went back from it. If it fails again, the {product} goes back to the version running now.')),
      check: productText('I know this version was rolled back on this {product} before.') });
    const appPick = fileButton('Choose file', 'Choose file: app (.bin)', '.bin,application/octet-stream', f => uploadApp(f, {}));
    const appUploadErr = errBox();
    /* Too big for the station: said before anything is sent (and when a refusal ends in a reset). */
    const tooBig = uploadTooBig;
    /* o.anyProject, o.rolledBackAck, o.reinstall: what the user already confirmed. */
    async function uploadApp(file, o) {
      appUploadErr.hidden = true;
      const a = (model.update && model.update.app) || {};
      const max = model.update && model.update.app_max_bytes;
      /* Read the image's own description before sending a byte. */
      if (max && file.size > max) {
        say(appUploadErr, tooBig(file, max, productText('the {product}\'s app slot')));
        return;
      }
      const img = await readAppImage(file);
      if (!img) {
        say(appUploadErr, productText('This file is not an app image for the {product} (it does not start like one). Choose the .bin file of a release or of your own build.'));
        return;
      }
      if (img.project !== UI.metadata.project_name && !o.anyProject) {
        /* The same warning the station's own refusal (not_this_project) leads to: a fork under another
         * project name is installable after it. */
        if (await anotherProject(img.project)) uploadApp(file, Object.assign({}, o, { anyProject: true }));
        return;
      }
      const same = model.update && model.update.running_elf_sha256 && img.elfSha === model.update.running_elf_sha256;
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
          body: [h('p', null, file.name + ', ' + fmt.bytes(file.size) + productText('. The {product} writes it to its spare app slot and restarts into it. ') +
            productText('The {product} checks the new version and goes back to this one if it fails.'))] });
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
          say(appUploadErr, tooBig(file, max, productText('the {product}\'s app slot')));
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
        productText('. The {product} restarts into it now; this page reconnects by itself.'));
    }

  return { renderCheck, renderApp, finishChecks, cancelDownload, appPick, appUploadErr,
    get main() { return appMain; } };
}

function uploadTooBig(file, max, what) {
  return 'The file is ' + fmt.bytes(file.size) + '; ' + what + ' holds at most ' + fmt.bytes(max) + '.';
}
