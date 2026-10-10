// SPDX-FileCopyrightText: 2026 Avery L.
// SPDX-License-Identifier: MIT
//
// Placeholder script so the firmware builds; the web UI replaces this file.
'use strict';

fetch('/api/session', { credentials: 'same-origin' })
  .then((r) => r.json())
  .then((s) => {
    const el = document.getElementById('state');
    if (!s.admin_set) {
      el.textContent = s.claim_allowed ? 'No admin password set yet.' : 'Connect to the station\'s setup Wi-Fi to set the admin password.';
    } else {
      el.textContent = s.authenticated ? 'Logged in.' : 'Admin password set.';
    }
  })
  .catch(() => {
    document.getElementById('state').textContent = 'The station did not answer.';
  });
