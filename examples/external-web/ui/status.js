/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */

UI.navigation = [['', 'Status'], ['settings', 'Settings']];
Object.assign(UI.errors, {
  not_on_setup_ap: 'The first admin password can only be set over the setup hotspot.',
});
UI.titleName = () => UI.metadata.default_brand;
UI.brandName = () => UI.metadata.default_brand;
UI.classifyStatus = status => {
  const online = status && status.network && status.network.online;
  if (online === true) return { key: 'online', tone: 'ok', head: 'Online', word: 'Online' };
  if (online === false) return { key: 'offline', tone: 'warn', head: 'Network offline', word: 'Offline' };
  return { key: 'unknown', tone: '', head: 'Network unknown', word: 'Unknown' };
};
UI.commissioningLoad = () => api('GET', '/api/product/settings');
UI.commissioningSave = values => api('PUT', '/api/product/settings', values);
UI.commissioningSteps = [
  {
    key: 'network', title: 'Check access',
    create({ body, next }) {
      mount(body, h('p', null, 'This device reports its uptime, network and its own counter. No receiver is required.'),
        h('p', null, 'Recovery access: hold BOOT for 10 seconds, then join the setup hotspot and set a new admin password.'),
        h('div', { class: 'form-actions' }, btn('Continue', () => next(), 'primary')));
    },
  },
  {
    key: 'visibility', title: 'Choose status visibility',
    create({ body, state, next }) {
      const publicStatus = checkbox('Allow status without login', state.public_status === true);
      mount(body, h('p', null, 'Public status includes uptime, the counter and whether the network is online. Network names and addresses stay private.'),
        publicStatus.el, h('div', { class: 'form-actions' },
          btn('Go to Status', () => next({ public_status: publicStatus.input.checked }), 'primary')));
    },
  },
];

Views[''] = {
  title: 'Status', admin: false,
  create(el) {
    const render = status => {
      const st = status || {};
      const p = st.product || {}, net = st.network || {};
      const age = S.statusAt ? (Date.now() - S.statusAt) / 1000 : null;
      const stale = age !== null && age >= CONTACT_S;
      const state = classifyStatus(st, isAdmin());
      mount(el, pageCard(state.head,
        stale ? msg('warn', 'Last known values: no recent contact.') : null,
        kv([['Uptime', st.uptime_s === undefined || st.uptime_s === null ? DASH : fmt.dur(st.uptime_s)],
          ['Counter', p.counter === undefined || p.counter === null ? DASH : String(p.counter)],
          ['Network', net.online === true ? 'Online' : net.online === false ? 'Offline' : 'Unknown'],
          ...(isAdmin() && !st.public ? [['Network name', net.ssid || DASH], ['Address', net.ip || DASH]] : [])])));
    };
    render(S.status);
    const ticker = setInterval(() => render(S.status), 1000);
    return { update: render, destroy: () => clearInterval(ticker) };
  },
};

Views.settings = {
  title: 'Settings', admin: true, showTitle: true,
  create(el) {
    let dirty = false, gone = false, edits = 0;
    const reload = async () => {
      let state;
      try { state = await UI.commissioningLoad(); }
      catch (e) { if (!gone && !dirty) mount(el, msg('bad', errText(e))); return; }
      if (gone || dirty) return;
      const pub = checkbox('Allow status without login', state.public_status === true);
      pub.input.disabled = state.settings_available === false;
      const err = errBox();
      const submit = h('button', { type: 'submit', class: 'btn primary', disabled: state.settings_available === false }, 'Save');
      mount(el, pageCard('Status visibility',
        state.settings_available === false ? msg('warn', 'Settings are unavailable. Recovery access remains available with BOOT.') : null,
        h('form', { oninput: () => { dirty = true; edits++; }, onsubmit: async e => {
          e.preventDefault();
          const sent = edits;   /* an edit made while this save runs is still unsaved */
          const saved = await act(submit, () => UI.commissioningSave({ public_status: pub.input.checked }), err);
          if (saved !== undefined) { if (edits === sent) dirty = false; toast('Saved.', 'ok'); }
        } }, pub.el, err, h('div', { class: 'form-actions' }, submit))));
    };
    reload();
    return { dirty: () => dirty, reload, destroy: () => { gone = true; } };
  },
};
