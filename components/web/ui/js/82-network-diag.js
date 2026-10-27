/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */

/* Read every page from a stopped recording. Never merge two generations if
 * another admin starts a recording while this download is being assembled. */
async function networkDiagDownload(read, stop) {
  const first = await stop();
  const samples = [];
  let page = first, cursor = 0;
  for (;;) {
    if (page.generation !== first.generation || page.active) throw new Error('Recording restarted. Stop it before downloading.');
    samples.push(...page.samples);
    if (!page.samples.length || samples[samples.length - 1].seq >= first.last_seq) break;
    const next = samples[samples.length - 1].seq;
    if (next <= cursor) throw new Error('Incomplete recording.');
    cursor = next;
    page = await read(cursor);
  }
  if ((samples.length ? samples[samples.length - 1].seq : 0) !== first.last_seq) throw new Error('Recording changed during download.');
  return { ...first, samples };
}

function networkDiagTab(box) {
  const path = '/api/monitor/diagnostics';
  const summary = h('div'), err = h('div'), refreshErr = h('div'), send = h('div');
  let gone = false, last = null;
  const read = cursor => api('GET', path + '?cursor=' + cursor);
  const render = d => {
    if (gone) return;
    last = d;
    start.disabled = d.active;
    stop.disabled = !d.active;
    mount(summary, kv([
      ['Recording', d.active ? 'Running · ' + fmt.dur(d.expires_in_s) + ' left' : d.last_seq ? 'Stopped' : 'Not started'],
      ['Samples', fmt.num(d.last_seq)],
      d.skipped_cycles ? ['Skipped samples', fmt.num(d.skipped_cycles)] : null,
    ]));
    mount(send, kv((d.transmit || []).filter(t => t.generation).map(t => [
      'Server ' + (t.slot + 1),
      fmt.num(t.slow_writes) + ' slow writes · ' + networkDiagTcpText(t),
    ])));
  };
  const refresh = async () => {
    try {
      const d = await read(last ? last.last_seq : 0);
      if (gone) return;
      mount(refreshErr);
      render(d);
    } catch (e) {
      if (!gone) mount(refreshErr, msg('warn', 'Cannot refresh diagnostics; displayed values may be old. ' + errText(e)));
    }
  };
  const action = async (button, fn) => {
    await act(button, fn, err);
    if (last) render(last);   /* busy() restores the button; reapply recording state */
  };
  const start = btn('Record for 10 minutes', () => action(start, async () => render(await api('POST', path, {}))));
  const stop = btn('Stop recording', () => action(stop, async () => render(await api('DELETE', path))));
  const download = btn('Stop and download', () => act(download, async () => {
    const d = await networkDiagDownload(read, () => api('DELETE', path));
    render(d);
    const url = URL.createObjectURL(new Blob([JSON.stringify(d, null, 2)], { type: 'application/json' }));
    const a = h('a', { href: url, download: 'network-diagnostics.json', hidden: true });
    doc.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }, err));
  mount(box, h('section', { class: 'card' }, h('h2', null, 'Network diagnostics'),
    h('p', null, 'Record router, internet and server pings every second for 10 minutes. '
      + 'Kept until the next recording or restart.'),
    h('p', { class: 'hint' }, 'Servers may ignore pings. These are not rover correction ages.'),
    refreshErr, summary, err, h('div', { class: 'form-actions' }, start, stop, download),
    h('h3', { class: 'sub' }, 'Outgoing streams'), send,
    h('p', { class: 'hint' }, 'Slow writes and TCP samples are automatic. Acknowledgement confirms TCP receipt, not use by the server. '
      + 'The download includes recent slow writes and network addresses.')));
  const cancel = poller(5000, refresh);
  return { stop() { gone = true; cancel(); } };
}

function networkDiagTcpText(t) {
  if (!t.tcp.available || !t.tcp.sampled_us) return 'TCP sample unavailable';
  const age = Math.max(0, Math.floor((t.captured_us - t.tcp.sampled_us) / 1000));
  if (age > 3000) return 'TCP sample is ' + fmt.ms(age) + ' old; current backlog unknown';
  return fmt.bytes(t.tcp.state.outstanding_bytes) + ' awaiting TCP acknowledgement at the last sample (' + fmt.ms(age) + ' ago)';
}
