/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */

/* Product hooks are registered before theme/start. Defaults require no product
 * sections, services or commissioning. No hook is a browser global. */
const UI = {
  metadata: PRODUCT,
  navigation: [],
  viewAliases: {},
  redirect: () => null,
  mergeSections: {},
  classifyStatus: () => ({ key: 'unknown', tone: '', head: 'Unknown', word: 'Unknown' }),
  notices: () => [],
  pageIdentity: current => current ? current.name : '',
  brandName: () => '',
  titleName: () => '',
  navDot: () => false,
  navDotPage: '',
  errors: {},
  fieldLabels: {},
  errorText: (error, text) => text,
  fieldRule: 'this value was not accepted',
  tick: () => {},
  event: () => {},
  commissioning: null,
  setupHint: 'Join the setup hotspot.',
  scanWarning: 'The connection may pause for a few seconds during a scan.',
  progress: null,
  forgetSecrets: () => {},
  updateState: value => value,
  updateResultText: message => message.message || message.result || '',
  receiverUpdateActive: () => false,
  appDowntime: null,
  finishChecksText: 'Keep this version and continue watching for crashes.',
};

function storageKey(name) {
  return UI.metadata.browser_storage_prefix + name;
}

function classifyStatus(status, admin) {
  return UI.classifyStatus(status, admin) || { key: 'unknown', tone: '', head: 'Unknown', word: 'Unknown' };
}
