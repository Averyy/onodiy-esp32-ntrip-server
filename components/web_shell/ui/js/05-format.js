/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */

/* Minutes for a sentence ("4 min"; under a minute in seconds). */
function inMinutes(s) {
  if (s === null || s === undefined) return DASH;
  return s < 60 ? fmt.dur(s) : fmt.dur(Math.ceil(s / 60) * 60);
}

/**
 * "Good", "Fair", "Weak" plus bars for an RSSI in dBm; noDbm: the word only
 * (the dashboard, the network list). A reading is -127 to -1 dBm: null and 0
 * are unknown and show a dash.
 */
function rssiView(rssi, noDbm) {
  if (rssi === null || rssi === undefined || rssi >= 0) return DASH;
  const n = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
  const word = ['', 'Weak', 'Fair', 'Good', 'Excellent'][n];
  return h('span', { class: 'nowrap' }, h('span', { class: 'sig s' + n, 'aria-hidden': 'true' }, h('i'), h('i'), h('i'), h('i')),
    ' ' + word + (noDbm ? '' : ' (' + dbmText(rssi) + ')'));
}

/** A signal in dBm with a true minus sign: "−82 dBm". */
function dbmText(v) {
  return String(v).replace('-', '\u2212') + NB + 'dBm';
}

