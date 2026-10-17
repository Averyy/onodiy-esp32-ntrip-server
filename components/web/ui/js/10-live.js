/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Live data: one status model fed by the WebSocket (1 Hz deltas, events,
 * job progress, log text), with GET /api/status polling whenever the socket
 * is down. Views read S.status and listen on the bus:
 *   status (model)  event (message)  progress (message)  log (message)
 *   unauthorized    rebooting (reason)
 */

const S = {
  session: null,    /* GET /api/session */
  status: null,     /* GET /api/status, kept current by the WebSocket */
  statusAt: 0,      /* Date.now() of the last status data */
  progress: {},     /* job -> last progress message */
  rates: {},        /* slot -> [[uptime_s, bytes_sent], ...] over the last minute */
  publicView: false,
};

function isAdmin() {
  return !!(S.session && S.session.authenticated);
}

let apiWarned = false;

/**
 * The station speaks another API version than this page (a firmware update
 * this tab did not see): load the page again, which brings the matching UI.
 * At most once a minute, so a page that still differs after a reload only
 * says so. @return true on a mismatch.
 */
function apiMismatch(v) {
  if (v === undefined || v === null || v === API_VERSION) return false;
  let last = 0;
  try {
    last = Number(sessionStorage.getItem('station.reloadAt')) || 0;
  } catch (e) {
    last = 0;
  }
  if (Date.now() - last > 60000) {
    try {
      sessionStorage.setItem('station.reloadAt', String(Date.now()));
    } catch (e) {
      /* without storage the next mismatch reloads again, a minute apart at best */
    }
    location.reload();
  } else if (!apiWarned) {
    apiWarned = true;
    toast('This page does not match the station\'s firmware. Reload the page.', 'warn');
  }
  return true;
}

/**
 * The station answered as if this page had no session (it restarted, or the
 * session ended while this page could not hear about it): show the login.
 */
function sessionLost() {
  if (isAdmin()) bus.emit('unauthorized');
}

/** Merge one WebSocket status message (full snapshot or changed sections). */
function applyStatusMessage(m) {
  const data = m.data || {};
  if (m.full || !S.status) S.status = {};
  for (const k of Object.keys(data)) {
    if (k === 'casters' && !m.full && Array.isArray(S.status.casters)) {
      const bySlot = {};
      for (const c of S.status.casters) bySlot[c.slot] = c;
      for (const c of data.casters) bySlot[c.slot] = c;
      S.status.casters = Object.keys(bySlot).map(Number).sort((a, b) => a - b).map(n => bySlot[n]);
    } else {
      S.status[k] = data[k];
    }
  }
  if (m.uptime_s !== undefined) S.status.uptime_s = m.uptime_s;
  statusChanged();
}

function statusChanged() {
  S.statusAt = Date.now();
  sampleRates();
  bus.emit('status', S.status);
}

/* Output rate per caster from bytes_sent (device uptime as the clock). */
function sampleRates() {
  const st = S.status;
  if (!st || !Array.isArray(st.casters) || st.uptime_s === undefined) return;
  for (const c of st.casters) {
    if (c.bytes_sent === undefined || c.bytes_sent === null) continue;
    let l = S.rates[c.slot] || (S.rates[c.slot] = []);
    const last = l[l.length - 1];
    if (last && (c.bytes_sent < last[1] || st.uptime_s < last[0])) l = S.rates[c.slot] = [];
    if (!last || st.uptime_s > last[0]) l.push([st.uptime_s, c.bytes_sent]);
    while (l.length > 2 && l[0][0] < st.uptime_s - 61) l.shift();
  }
}

/** Bits per second a slot sent over the last `window` seconds (null = not enough data). */
function slotRate(slot, window) {
  const l = S.rates[slot];
  if (!l || l.length < 2) return null;
  const end = l[l.length - 1];
  let start = l[0];
  for (const p of l) {
    if (p[0] >= end[0] - window) {
      start = p;
      break;
    }
  }
  if (end[0] - start[0] < Math.min(window, 3)) return null;
  return ((end[1] - start[1]) * 8) / (end[0] - start[0]);
}

/*
 * Reconnecting: the delay doubles from 1 s to 15 s, each one drawn within
 * +-20 % (WS_JITTER) so pages that lost the station together do not all
 * come back in the same second, and starts over only after a connection
 * that lived WS_STABLE_MS. A handshake the station refuses (no session,
 * public status off, too many clients) or a connection it closes at once
 * never resets it, so a refused or evicted page backs off instead of
 * retrying every second: at most 8 handshakes in its first minute, then one
 * every 12 to 18 s. Only live.stop() starts the delay over without a stable
 * connection, and every caller of stop() leaves this page for another one
 * (the login, the setup wizard, the restart overlay): start() again follows
 * a user's action (a login, "View the status") or a page load, never a
 * refusal. After a refusal the page asks GET /api/session once: a lost
 * session shows the login, a public view whose public status was turned off
 * shows the login page and stops (no more handshakes or polls).
 */
const WS_STABLE_MS = 10000;
const WS_RETRY_MAX_MS = 15000;
const WS_JITTER = 0.2;

const live = {
  ws: null,
  state: 'off',        /* off, connecting, ws, poll */
  topics: null,        /* desired topic set; null = server default */
  subExtra: null,      /* () => more fields of the subscribe message (the log cursor) */
  lastMsg: 0,
  lastPing: 0,
  retryMs: 1000,
  reconnectAt: 0,
  retryTimer: 0,       /* the reconnect at reconnectAt */
  lastPoll: 0,
  polling: false,
  tick: 0,
  running: false,
  openedAt: 0,         /* when the current socket opened (0 = not open) */
  checked: false,      /* GET /api/session asked since the last stable connection */
  checking: false,     /* that request is on its way */

  start() {
    if (this.running) return;
    this.running = true;
    this.connect();
    this.tick = setInterval(() => this.watch(), 1000);
  },

  stop() {
    this.running = false;
    this.retryMs = 1000;
    this.checked = false;
    clearInterval(this.tick);
    clearTimeout(this.retryTimer);
    if (this.ws) {
      const ws = this.ws;
      this.ws = null;
      ws.onclose = null;
      try {
        ws.close();
      } catch (e) {
        /* already closed */
      }
    }
    this.state = 'off';
  },

  connect() {
    if (!this.running || typeof WebSocket !== 'function') return;
    let ws;
    try {
      ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
    } catch (e) {
      this.scheduleReconnect();
      return;
    }
    this.ws = ws;
    this.state = 'connecting';
    this.lastMsg = Date.now();
    this.openedAt = 0;
    ws.onopen = () => {
      this.openedAt = Date.now();
      this.lastMsg = Date.now();
      if (this.topics) this.send(this.subMessage());
    };
    ws.onmessage = ev => {
      this.lastMsg = Date.now();
      let m;
      try {
        m = JSON.parse(ev.data);
      } catch (e) {
        return;
      }
      this.handle(m);
    };
    ws.onclose = ev => {
      if (this.ws !== ws) return;
      this.ws = null;
      const opened = this.openedAt;
      this.openedAt = 0;
      if (ev.code === 4001) {
        bus.emit('unauthorized');   /* a page with a session shows the login (it stops this) */
        if (this.running) this.scheduleReconnect();
        return;
      }
      if (ev.code === 1012) {
        bus.emit('rebooting', 'restart');   /* the restart overlay takes over (it stops this) */
        if (this.running) this.scheduleReconnect();
        return;
      }
      /* Only a connection that lived a while starts the backoff over. */
      if (opened && Date.now() - opened >= WS_STABLE_MS) {
        this.retryMs = 1000;
        this.checked = false;
      }
      /* Refused at the handshake: is the session (or the public view) still
       * there? Asked once, not on every retry. Public status turned off (1008)
       * is asked about even after an earlier refusal was: it is news. */
      if (!opened || ev.code === 1008) this.checkSession(ev.code === 1008);
      this.scheduleReconnect();
    };
    ws.onerror = () => {
      /* onclose follows */
    };
  },

  scheduleReconnect() {
    if (!this.running) return;
    this.state = 'poll';
    const delay = Math.round(this.retryMs * (1 - WS_JITTER + 2 * WS_JITTER * Math.random()));
    this.reconnectAt = Date.now() + delay;
    this.retryMs = Math.min(this.retryMs * 2, WS_RETRY_MAX_MS);
    /* On time, not at the next once-a-second watch(). */
    clearTimeout(this.retryTimer);
    this.retryTimer = setTimeout(() => this.reconnectIfDue(), delay);
  },

  /* A hidden tab with a session waits until it is shown (see watch()). */
  reconnectIfDue() {
    if (!this.ws && this.running && Date.now() >= this.reconnectAt && (visible() || !isAdmin())) this.connect();
  },

  /* After a refused handshake: a page whose session is gone shows the login;
   * a public view whose public status was turned off shows the login page.
   * force: ask even when this refusal streak was asked about already. */
  async checkSession(force) {
    if (this.checking || (this.checked && !force)) return;
    this.checked = true;
    this.checking = true;
    let s;
    try {
      s = await api('GET', '/api/session', undefined, { quiet401: true, timeout: 4000 });
    } catch (e) {
      this.checked = false;   /* no answer: ask again after the next refusal */
      return;
    } finally {
      this.checking = false;
    }
    if (!this.running) return;
    if (isAdmin() && !s.authenticated) {
      sessionLost();
    } else if (!isAdmin() && S.publicView && !s.public_status) {
      this.stop();
      S.session = s;
      showLogin('Public status was turned off. Login to see the station.');
    }
  },

  send(obj) {
    if (this.ws && this.ws.readyState === 1) {
      try {
        this.ws.send(JSON.stringify(obj));
      } catch (e) {
        /* the close handler reconnects */
      }
    }
  },

  subMessage() {
    return Object.assign({ type: 'subscribe', topics: this.topics }, this.subExtra ? this.subExtra() : null);
  },

  /**
   * Replace the topic set ("status", "progress", "log"). extra: optional
   * function giving more fields of the message, asked again on every
   * reconnect ({log_cursor: n}: log lines after n).
   */
  subscribe(topics, extra) {
    this.topics = topics;
    this.subExtra = extra || null;
    this.send(this.subMessage());
  },

  handle(m) {
    switch (m.type) {
      case 'hello':
        if (apiMismatch(m.api)) return;
        if (m.admin === false && isAdmin()) {
          /* The socket was taken as a public viewer's: the session is gone. */
          sessionLost();
          return;
        }
        this.state = 'ws';
        break;
      case 'status':
        this.state = 'ws';
        applyStatusMessage(m);
        break;
      case 'event':
        if (m.event === 'rebooting') bus.emit('rebooting', m.reason || 'restart');
        bus.emit('event', m);
        break;
      case 'progress':
        S.progress[m.job] = m;
        bus.emit('progress', m);
        break;
      case 'log':
        bus.emit('log', m);
        break;
      default:
        break;
    }
  },

  /* Once a second: liveness, keep-alive pings, polling while the socket is down. */
  watch() {
    const now = Date.now();
    const ws = this.ws;
    if (ws && ws.readyState === 1) {
      if (now - this.lastMsg > 5000) {
        /* Status comes every second (admin and public pages alike); silence
         * means a dead connection. Its backoff goes on: no reset. */
        this.ws = null;
        this.openedAt = 0;
        ws.onclose = null;
        try {
          ws.close();
        } catch (e) {
          /* ignore */
        }
        this.scheduleReconnect();
      } else if (isAdmin() && doc.visibilityState === 'visible' && now - this.lastPing > 60000) {
        /* A ping extends the session; only a visible tab keeps it alive. */
        this.lastPing = now;
        this.send({ type: 'ping' });
      }
    } else if (ws && ws.readyState === 0 && now - this.lastMsg > 10000) {
      /* A handshake that never completes. */
      this.ws = null;
      this.openedAt = 0;
      ws.onclose = null;
      try {
        ws.close();
      } catch (e) {
        /* ignore */
      }
      this.scheduleReconnect();
    } else if (!ws) {
      this.reconnectIfDue();
    }
    /* A hidden tab with a session neither polls nor reconnects: every
     * request would extend the session, and a forgotten tab must not stay
     * logged in. It catches up as soon as it is shown again. */
    if (this.state !== 'ws' && this.running && !this.polling && now - this.lastPoll >= 2000 && (visible() || !isAdmin())) {
      this.poll();
    }
  },

  async poll() {
    this.polling = true;
    this.lastPoll = Date.now();
    try {
      const st = await api('GET', '/api/status', undefined, { timeout: 4000 });
      if (apiMismatch(st && st.api)) return;
      if (st && st.public && isAdmin()) {
        sessionLost();   /* the public form: the station no longer knows this page's session */
        return;
      }
      if (this.state !== 'ws') {
        S.status = st;
        statusChanged();
      }
    } catch (e) {
      /* GET /api/status answers 403 only to a page without a session while
       * public status is off: an admin page lost its session, a public view
       * its public status (the session check stops it). */
      if (e instanceof ApiError && e.status === 403) {
        if (isAdmin()) sessionLost();
        else if (S.publicView) this.checkSession(true);
      }
    } finally {
      this.polling = false;
    }
  },
};

function visible() {
  return doc.visibilityState === 'visible';
}

/* Shown again: reconnect and refresh at once instead of waiting out the backoff. */
doc.addEventListener('visibilitychange', () => {
  if (!visible() || !live.running) return;
  if (!live.ws) live.reconnectAt = 0;
  live.watch();
});

/** Load the status once (before the socket delivers). */
async function loadStatus() {
  const st = await api('GET', '/api/status', undefined, { quiet401: true });
  S.status = st;
  statusChanged();
  return st;
}
