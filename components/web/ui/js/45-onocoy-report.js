/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */

const ONOCOY_API_ACCESS_URL = 'https://console.onocoy.com/settings/api-access';

/* A local deadline keeps the countdown moving between reads of the station's cache. */
function stampOnocoyReport(r) {
  return { ...r, retry_until_ms: Date.now() + (r.manual_in_s || 0) * 1000 };
}

function reportRetrySeconds(r, now) {
  return Math.max(0, Math.ceil(((r.retry_until_ms ?? now + (r.manual_in_s || 0) * 1000) - now) / 1000));
}

function tickOnocoyReportRetry() {
  for (const b of doc.querySelectorAll('[data-onocoy-retry-until]')) {
    const left = Math.max(0, Math.ceil((Number(b.getAttribute('data-onocoy-retry-until')) - Date.now()) / 1000));
    const text = left > 0 ? 'Refresh in ' + fmt.dur(left) : 'Refresh';
    if (b.textContent !== text) b.textContent = text;
    b.disabled = left > 0;
    if (!left) b.removeAttribute('data-onocoy-retry-until');
  }
}

function onocoyReportEnabled(s) {
  return !!s && !s.public && !s.safe_mode?.active && (s.casters || []).some(c =>
    c.protected && c.enabled && c.onocoy_ready && c.onocoy_stats);
}

function reportDate(date) {
  return new Date(date + 'T00:00:00Z').toLocaleDateString(undefined,
    { year: 'numeric', month: 'short', day: 'numeric', timeZone: 'UTC' });
}

function reportUptime(s) {
  return s === null || s === undefined ? DASH :
    (100 * s / 86400).toFixed(2) + '% · ' + fmt.dur(s);
}

function reportLatency(d) {
  const val = v => v === null || v === undefined ? DASH : Number(v).toLocaleString(undefined, { maximumFractionDigits: 3 }) + 's';
  return 'avg ' + val(d.latency_avg) + ' · worst ' + val(d.latency_max);
}

function reportRewards(v) {
  return v === null || v === undefined ? DASH : Number(v).toLocaleString(undefined, { maximumFractionDigits: 6 }) + ' ONO';
}

function reportNote(r) {
  if (!r) return 'Waiting for the station’s first API check.';
  if (r.busy || r.state === 'pending') return 'Downloading daily statistics…';
  if (r.state === 'waiting_network') return 'Waiting for an internet connection.';
  if (r.state === 'waiting_time') return 'Waiting for the station’s clock to be set.';
  if (r.state === 'auth') return ['API access was refused. Go to ',
    extLink('Onocoy Settings → API access', ONOCOY_API_ACCESS_URL),
    ' and turn on API access so these station credentials can download reports. ' +
    'If it is already on, check the credentials and that the station passed validation within the last 7 days.'];
  if (r.state === 'error') return 'Could not download a new report. The last downloaded readings are shown when available.';
  if (r.state === 'empty') return 'No completed daily reports are available yet.';
  return '';
}

function onocoyReportCard(r) {
  const d = r?.days?.[0];
  const previous = r?.days?.[1];
  const meta = 'Onocoy' + (d ? ' · ' + reportDate(d.date) : ' API') + ' · updates daily';
  const note = reportNote(r);
  const failed = r && (r.state === 'auth' || r.state === 'error');
  const left = r ? reportRetrySeconds(r, Date.now()) : 0;
  return h('section', { class: 'card fit-kv' },
    cardHead('Onocoy API report', link('History', '#/logs/history', 'head-link')),
    h('p', { class: 'muted small nowrap report-meta', title: meta }, meta),
    note ? h('p', { class: 'muted small' }, note) : null,
    d ? kv([['Online time', reportUptime(d.uptime_s)], ['Data latency', reportLatency(d)],
      ['Latest rewards', reportRewards(d.rewards)], ['Previous rewards', reportRewards(previous?.rewards)]]) : null,
    failed ? h('div', { class: 'row' },
      h('button', { type: 'button', class: 'btn small', 'data-act': 'refresh-onocoy',
        'data-onocoy-retry-until': !r.busy && left > 0 ? r.retry_until_ms ?? Date.now() + left * 1000 : null,
        disabled: r.busy || left > 0 },
      r.busy ? 'Refreshing…' : left > 0 ? 'Refresh in ' + fmt.dur(left) : 'Refresh')) : null);
}

function onocoyReportHistory(card, r) {
  card.hidden = !r?.enabled;
  if (card.hidden) return;
  const days = r.days || [];
  mount(card, h('h2', null, 'Onocoy reports'),
    reportNote(r) ? h('p', { class: 'muted small' }, reportNote(r)) : null,
    days.length ? scrollBox('Onocoy reports', h('table', null,
      h('thead', null, h('tr', null, ['Date', 'Online time', 'Data latency', 'Rewards'].map(x => h('th', null, x)))),
      h('tbody', null, days.map(d => h('tr', null,
        h('td', null, reportDate(d.date)), h('td', null, reportUptime(d.uptime_s)),
        h('td', null, reportLatency(d)), h('td', null, reportRewards(d.rewards))))))) : null,
    h('p', { class: 'muted small foot' }, 'Reported by Onocoy, per completed UTC date. Gaps mean no report was returned.'));
}
