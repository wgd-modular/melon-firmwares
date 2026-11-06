// Remembers the last recording, its settings and the picked drive between
// visits. Everything stays in this browser.

let opening;

function open() {
  opening ??= new Promise((resolve, reject) => {
    const request = indexedDB.open('melon-radio', 1);
    request.onupgradeneeded = () => request.result.createObjectStore('kv');
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
  return opening;
}

function run(mode, action) {
  return open().then(db => new Promise((resolve, reject) => {
    const request = action(db.transaction('kv', mode).objectStore('kv'));
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  }));
}

export function load(key) {
  return run('readonly', store => store.get(key)).catch(() => undefined);
}

export function save(key, value) {
  return run('readwrite', store => store.put(value, key)).catch(() => undefined);
}

export function forget(key) {
  return run('readwrite', store => store.delete(key)).catch(() => undefined);
}
