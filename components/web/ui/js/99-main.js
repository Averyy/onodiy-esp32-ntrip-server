/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Entry point: theme first (no flash of the wrong colours), then the
 * session decides between setup, log in, public status and the app.
 *
 * A page that had a session when the station restarted (the restart
 * overlay noted it in sessionStorage 'station.restartedAt', 20-shell.js)
 * comes back at the login, which says why, also when public status would
 * otherwise show the public view: sessions do not survive a restart.
 */

function restartedRecently() {
  try {
    const at = Number(sessionStorage.getItem('station.restartedAt')) || 0;
    return at > 0 && Date.now() - at < 10 * 60 * 1000;
  } catch (e) {
    return false;
  }
}

async function start() {
  if (!restartedRecently()) {
    boot();
    return;
  }
  let s;
  try {
    s = await api('GET', '/api/session', undefined, { quiet401: true, timeout: 8000 });
  } catch (e) {
    boot();   /* no answer: boot() says so and offers to try again */
    return;
  }
  if (apiMismatch(s.api)) return;   /* it reloads the page */
  if (s.authenticated || !s.admin_set) {
    boot();
    return;
  }
  S.session = s;
  if (s.hostname) setBrand(s.hostname);
  showLogin();
}

applyTheme();
start();
