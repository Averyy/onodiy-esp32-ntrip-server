/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */

const MAP_ORIGIN = 'https://tiles.openfreemap.org';
const MAP_STYLE = MAP_ORIGIN + '/styles/liberty';

/** Horizontal separation, including across the date line. */
function mapDistance(a, b) {
  const rad = Math.PI / 180;
  const dlat = (b[1] - a[1]) * rad, dlon = (b[0] - a[0]) * rad;
  const s = Math.sin(dlat / 2) ** 2 + Math.cos(a[1] * rad) * Math.cos(b[1] * rad) * Math.sin(dlon / 2) ** 2;
  return 12742000 * Math.asin(Math.sqrt(Math.min(1, s)));
}

/* IndexedDB also works on the station's plain-HTTP LAN origin. Cache Storage and
 * service workers require HTTPS. Only resources requested by the visible map are
 * downloaded. No expiry timer: a confirmed 10 m change clears the tile store. */
const mapCache = {
  db: null,
  moving: Promise.resolve(),
  failed: false,
  open() {
    if (!this.db) this.db = new Promise(resolve => {
      try {
        const r = indexedDB.open('station-map', 1);
        r.onupgradeneeded = () => {
          for (const name of ['tiles', 'assets', 'meta']) r.result.createObjectStore(name);
        };
        r.onsuccess = () => resolve(r.result);
        r.onerror = r.onblocked = () => { this.failed = true; resolve(null); };
      } catch (e) { this.failed = true; resolve(null); }
    });
    return this.db;
  },
  async read(store, key) {
    const db = await this.open();
    if (!db) return undefined;
    return new Promise(resolve => {
      try {
        const r = db.transaction(store).objectStore(store).get(key);
        r.onsuccess = () => resolve(r.result);
        r.onerror = () => { this.failed = true; resolve(undefined); };
      } catch (e) { this.failed = true; resolve(undefined); }
    });
  },
  async write(store, key, value) {
    const db = await this.open();
    if (!db) return;
    return new Promise(resolve => {
      try {
        const tx = db.transaction(store, 'readwrite');
        if (key === null) tx.objectStore(store).clear();
        else if (value === undefined) tx.objectStore(store).delete(key);
        else tx.objectStore(store).put(value, key);
        tx.oncomplete = () => resolve();
        tx.onerror = tx.onabort = () => { this.failed = true; resolve(); };
      } catch (e) { this.failed = true; resolve(); }
    });
  },
  position(lngLat) {
    this.moving = this.moving.then(async () => {
      const anchor = await this.read('meta', 'position');
      if (!anchor || mapDistance(anchor, lngLat) > 10) {
        if (anchor) {
          await this.write('tiles', null);
          /* The tile source's metadata selects the current dataset. Fonts,
           * sprites and styles can stay cached when a station relocates. */
          await this.write('assets', MAP_ORIGIN + '/planet', undefined);
        }
        await this.write('meta', 'position', lngLat);
        return !!anchor;
      }
      return false;
    });
    return this.moving;
  },
  async resource(url, signal) {
    if (!url.startsWith(MAP_ORIGIN + '/')) throw new Error('Unexpected map resource origin');
    await this.moving;
    const path = new URL(url).pathname;
    const store = path.startsWith('/planet/') || path.startsWith('/natural_earth/') ? 'tiles' : 'assets';
    let bytes = await this.read(store, url);
    if (signal && signal.aborted) throw new DOMException('Map closed', 'AbortError');
    if (!bytes) {
      const r = await fetch(url, { signal: signal, credentials: 'omit', referrerPolicy: 'origin', cache: 'default' });
      if (!r.ok) throw new Error('Map resource: HTTP ' + r.status);
      bytes = await r.arrayBuffer();
      await this.write(store, url, bytes);
    }
    return bytes;
  },
};

let mapLibrary = null;
function loadMapLibrary() {
  if (!mapLibrary) mapLibrary = import('/vendor/maplibre-gl.mjs').then(lib => {
    lib.setWorkerUrl(location.origin + '/vendor/maplibre-gl-worker.mjs');
    lib.setWorkerCount(1);
    lib.addProtocol('stationmap', async (p, controller) => {
      const bytes = await mapCache.resource('https://' + p.url.slice('stationmap://'.length), controller.signal);
      const data = p.type === 'json' ? JSON.parse(new TextDecoder().decode(bytes)) :
        p.type === 'string' ? new TextDecoder().decode(bytes) : bytes;
      return { data: data };
    });
    return lib;
  }).catch(e => { mapLibrary = null; throw e; });
  return mapLibrary;
}
