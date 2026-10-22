/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * How to reach the station: its mDNS name (<name>.local, answered on the
 * home network and on the station's own access point) and its IP address,
 * always as plain http:// links. Used by the setup wizard, the dashboard's
 * network card and Settings.
 */

/** The name in use (the DHCP name; after a rename, the old one until the restart): from the object itself, the status or
 * the session. */
function stationHostname(net) {
  const d = S.status && S.status.device;
  return (net && net.hostname) || (d && (d.hostname_active || d.hostname)) || (S.session && S.session.hostname) || '';
}

/** http://<mdns_host>.local/ while the mDNS responder runs, else null. */
function stationNameUrl(net) {
  return net && net.mdns_host ? 'http://' + net.mdns_host + '.local/' : null;
}

/** http://<ip>/ while the station has an address, else null. */
function stationIpUrl(net) {
  return net && net.ip ? 'http://' + net.ip + '/' : null;
}

/** The address to tell someone: the name when it answers, else the IP. */
function stationUrl(net) {
  return stationNameUrl(net) || stationIpUrl(net);
}

/**
 * Rows for kv(): ['Name', link to http://<mdns_host>.local/] (only while the
 * responder runs) and ['Address', link to http://<ip>/] (only with an
 * address). When another device holds the station's name, the responder
 * answers <name>-2 (or -3, ...) instead, and a note row says so.
 * `net`: GET /api/wifi, the status' network section, or anything with
 * mdns_host and ip (and optionally hostname).
 */
function stationAddressRows(net) {
  const rows = [];
  const name = stationNameUrl(net);
  if (name) rows.push(['Name', h('a', { href: name }, name)]);
  const host = stationHostname(net);
  if (name && host && net.mdns_host.toLowerCase() !== host.toLowerCase()) {
    rows.push(['', h('span', { class: 'muted small' }, host + '.local is used by another device on this network: this station answers as ' +
      net.mdns_host + '.local')]);
  }
  const ip = stationIpUrl(net);
  if (ip) rows.push(['Address', h('a', { href: ip }, ip)]);
  return rows;
}

/**
 * Copy text: the clipboard where the browser allows it (only on https or
 * localhost), else a copy from a selected field, else the field stays
 * selected for the user's own "Copy" (captive-portal browsers). `box` gets
 * the field and a line saying what happened.
 */
async function copyText(text, box) {
  if (navigator.clipboard && window.isSecureContext) {
    try {
      await navigator.clipboard.writeText(text);
      mount(box, h('span', { class: 'muted small' }, 'Copied.'));
      return true;
    } catch (e) {
      /* refused: the selection below */
    }
  }
  const field = h('input', { type: 'text', readonly: true, value: text, class: 'copy-field', 'aria-label': 'Address' });
  mount(box, field);
  field.focus();
  field.select();
  try {
    field.setSelectionRange(0, text.length);
  } catch (e) {
    /* not every input type takes a range */
  }
  let ok = false;
  try {
    ok = doc.execCommand('copy');
  } catch (e) {
    ok = false;
  }
  box.append(h('span', { class: 'muted small' }, ok ? 'Copied.' : 'Selected: copy it with your device\'s Copy command.'));
  return ok;
}
