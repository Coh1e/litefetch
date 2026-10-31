import {JOURNAL_PREFIX, journalKey} from './settings.mjs';

const MAX_MESSAGE_BYTES = 64 * 1024;
const encodedSize = value => new TextEncoder().encode(JSON.stringify(value)).byteLength;

export function storageJournal(storage, scope = 'regular') {
  if (!['regular', 'private'].includes(scope)) throw new Error('Invalid journal scope');
  const prefix = `${JOURNAL_PREFIX}${scope}.`;
  function key(id) {
    if (!Number.isSafeInteger(id) || id < 0) throw new Error('Invalid download ID');
    return journalKey(scope, id);
  }
  async function scan() {
    const values = await storage.get(null), records = [];
    let invalidCount = 0;
    for (const [name, record] of Object.entries(values)) {
      if (!name.startsWith(prefix)) continue;
      try {
        if (!record || typeof record !== 'object' || Array.isArray(record) || name !== key(record.downloadId)) {
          throw new Error('Invalid journal key');
        }
        records.push(record);
      } catch { invalidCount++; }
    }
    return {records, invalidCount};
  }
  return {
    async get(id) { return (await storage.get(key(id)))[key(id)]; },
    async set(id, record) {
      if (record.downloadId !== id || encodedSize(record) > 4096) throw new Error('Invalid journal record');
      await storage.set({[key(id)]: structuredClone(record)});
    },
    async list() {
      const result = await scan();
      if (result.invalidCount) throw new Error('Invalid journal key');
      return result.records;
    },
    scan,
  };
}

function deadline(operation, timeoutMs) {
  let timer;
  return Promise.race([
    Promise.resolve().then(operation),
    new Promise((_, reject) => { timer = setTimeout(() => reject(new Error('Browser operation timed out')), timeoutMs); }),
  ]).finally(() => clearTimeout(timer));
}

// Promise APIs, as provided by current Chromium MV3 and Firefox WebExtensions.
export function downloadsAdapter(downloads, timeoutMs = 5000) {
  if (!Number.isSafeInteger(timeoutMs) || timeoutMs < 1) throw new Error('Invalid timeout');
  function id(value) {
    if (!Number.isSafeInteger(value) || value < 0) throw new Error('Invalid download ID');
    return value;
  }
  return {
    async get(value) {
      const results = await deadline(() => downloads.search({id: id(value)}), timeoutMs);
      if (!Array.isArray(results) || results.length > 1) throw new Error('Unexpected browser search result');
      const item = results[0];
      if (item && (item.id !== value || !['in_progress', 'interrupted', 'complete'].includes(item.state))) {
        throw new Error('Unexpected browser download');
      }
      return item;
    },
    pause: value => deadline(() => downloads.pause(id(value)), timeoutMs),
    resume: value => deadline(() => downloads.resume(id(value)), timeoutMs),
    cancel: value => deadline(() => downloads.cancel(id(value)), timeoutMs),
  };
}

// Native Messaging JSON envelope, v1. The native executable owns stdin/stdout
// framing; the extension API already supplies parsed JSON objects here.
export class NativeClient {
  constructor(runtime, {host = 'org.native_downloader.bridge', timeoutMs = 5000,
    makeId = () => crypto.randomUUID()} = {}) {
    if (!/^[a-z0-9_]+(?:\.[a-z0-9_]+)+$/.test(host) || !Number.isSafeInteger(timeoutMs) || timeoutMs < 1) {
      throw new Error('Invalid native client options');
    }
    Object.assign(this, {runtime, host, timeoutMs, makeId});
    this.pending = new Map();
    this.port = undefined;
  }
  #disconnect(reason) {
    const port = this.port;
    this.port = undefined;
    if (port) {
      port.onMessage.removeListener(this.onMessage);
      port.onDisconnect.removeListener(this.onDisconnect);
      try { port.disconnect(); } catch { /* already disconnected */ }
    }
    for (const {reject, timer} of this.pending.values()) {
      clearTimeout(timer); reject(new Error(reason));
    }
    this.pending.clear();
  }
  #connect() {
    if (this.port) return;
    const port = this.runtime.connectNative(this.host);
    this.port = port;
    this.onMessage = message => {
      try {
        if (!message || encodedSize(message) > MAX_MESSAGE_BYTES || message.version !== 1
          || typeof message.id !== 'string' || typeof message.ok !== 'boolean') throw new Error('Malformed native reply');
        const entry = this.pending.get(message.id);
        if (!entry) throw new Error('Unknown native reply ID');
        this.pending.delete(message.id); clearTimeout(entry.timer);
        if (!message.ok) entry.reject(new Error('Native request rejected'));
        else if (!message.result || typeof message.result !== 'object' || Array.isArray(message.result)) {
          entry.reject(new Error('Malformed native result'));
        } else entry.resolve(message.result);
      } catch { this.#disconnect('Invalid native protocol'); }
    };
    this.onDisconnect = () => {
      // Reading lastError consumes Chromium's disconnect error. Never log its
      // text or outgoing payload, which can contain signed download URLs.
      void this.runtime.lastError;
      this.#disconnect('Native host disconnected');
    };
    port.onMessage.addListener(this.onMessage);
    port.onDisconnect.addListener(this.onDisconnect);
  }
  async #call(method, requestId, payload) {
    if (typeof requestId !== 'string' || !requestId || requestId.length > 128) throw new Error('Invalid request ID');
    if (this.pending.size >= 32) throw new Error('Native request limit reached');
    const id = this.makeId();
    if (typeof id !== 'string' || !id || this.pending.has(id)) throw new Error('Invalid native message ID');
    const message = {version: 1, id, method, requestId, ...(payload === undefined ? {} : {payload})};
    if (encodedSize(message) > MAX_MESSAGE_BYTES) throw new Error('Native request too large');
    this.#connect();
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => this.#disconnect('Native operation timed out'), this.timeoutMs);
      this.pending.set(id, {resolve, reject, timer});
      try { this.port.postMessage(message); }
      catch { this.#disconnect('Native message failed'); }
    });
  }
  prepare(requestId, request) { return this.#call('prepare', requestId, request); }
  query(requestId) { return this.#call('query', requestId); }
  commit(requestId) { return this.#call('commit', requestId); }
  abort(requestId) { return this.#call('abort', requestId); }
  close() { this.#disconnect('Native client closed'); }
}
