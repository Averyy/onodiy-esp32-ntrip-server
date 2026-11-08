/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */

/* Optional product steps. The legacy commissioning hook takes precedence.
 * Load/save belong to the product: no shared endpoint or stored step encoding.
 * create({body, state, next}) owns its inputs until next succeeds.
 * A save failure leaves the current step and its inputs intact; a step whose
 * create() throws or rejects shows the error with a way to try it again. */
async function startCommissioning(claim) {
  live.stop();
  const steps = UI.commissioningSteps;
  const frame = (key, title, ...body) => bare(h('div', { class: 'narrow' }, pageCard(title, ...body)));
  if (claim) return claimPassword(frame, () => startCommissioning(false));
  let state;
  try {
    state = await UI.commissioningLoad();
  } catch (e) {
    bare(pageCard('Setup could not be loaded', msg('bad', errText(e)),
      btn('Try again', () => startCommissioning(claim))));
    return;
  }
  const go = async index => {
    if (index >= steps.length) return startAuthed();
    const step = steps[index];
    const body = h('div');
    const err = errBox();
    let pending = false;
    frame(step.key, step.title, body, err);
    const next = async (values = {}) => {
      if (pending || !body.isConnected) return;
      pending = true;
      const following = steps[index + 1];
      try {
        const saved = await UI.commissioningSave(Object.assign({}, values,
          { setup_step: following ? following.key : step.key, setup_done: !following }));
        state = Object.assign({}, state, saved);
        S.session = Object.assign({}, S.session, { setup_done: !following });
      } catch (e) {
        showErr(e, err);
        pending = false;
        return;
      }
      if (body.isConnected) go(index + 1);
    };
    try {
      await step.create({ body, state, next });
    } catch (e) {
      if (!body.isConnected) return;
      showErr(e, err);
      mount(body, btn('Try again', () => go(index)));
    }
  };
  const resume = () => {
    const index = steps.findIndex(step => step.key === state.setup_step);
    go(Math.max(0, index));
  };
  resume();
}
