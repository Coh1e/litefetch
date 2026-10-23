import test from 'node:test';
import assert from 'node:assert/strict';
import {installWorker} from './worker.mjs';
import {SETTINGS_KEY, normalizeSettings} from './settings.mjs';
function event() {
  const listeners = [];
  return {listeners, addListener(fn, ...args) { listeners.push({fn, args}); },
    removeListener(fn) { const i = listeners.findIndex(v => v.fn === fn); if (i >= 0) listeners.splice(i, 1); },
    emit(...args) { for (const {fn} of [...listeners]) fn(...args); }};
}
async function waitFor(condition) {
  for (let index = 0; index < 200; index++) {
    if (condition()) return;
    await new Promise(resolve => setTimeout(resolve, 1));
  }
  assert.fail('Worker event did not settle');
}
function fixture({enabled = true, missingHost = false, records = {}} = {}) {
  const now = Date.parse('2026-10-20T00:00:00Z');
  const item = {id: 1, url: 'https://example.test/export', state: 'in_progress', paused: false,
    startTime: new Date(now).toISOString()};
  const values = {[SETTINGS_KEY]: {enabled, origins: ['https://example.test']}, ...records};
  const changes = event(), operations = [], ports = [], nativeStates = new Map();
  let sequence = 0, starts = 0, holdPrepare = false, held;
  const api = {
    storage: {onChanged: changes, local: {
      async get(key) { return structuredClone(key === null ? values : {[key]: values[key]}); },
      async set(update) {
        const eventChanges = {};
        for (const [key, value] of Object.entries(update)) {
          eventChanges[key] = {oldValue: values[key], newValue: value}; values[key] = structuredClone(value);
        }
        changes.emit(eventChanges, 'local');
      },
    }},
    webRequest: Object.fromEntries(['onBeforeRequest', 'onSendHeaders', 'onHeadersReceived', 'onBeforeRedirect', 'onErrorOccurred'].map(name => [name, event()])),
    downloads: {onCreated: event(), onChanged: event(),
      async search({id}) { return id === 1 ? [{...item}] : []; },
      async pause() { operations.push('pause'); item.paused = true; },
      async resume() { operations.push('resume'); item.paused = false; },
      async cancel() { operations.push('cancel'); item.state = 'interrupted'; item.paused = false; item.error = 'USER_CANCELED'; },
    },
    runtime: {onStartup: event(), onInstalled: event(), connectNative() {
      const port = {onMessage: event(), onDisconnect: event(), disconnect() {},
        postMessage(message) {
          const respond = () => {
            if (missingHost) { port.onDisconnect.emit(); return; }
            let result;
            if (message.method === 'prepare') {
              nativeStates.set(message.requestId, 'prepared');
              result = {state: 'prepared', taskId: 'task', expiresAt: now + 10_000};
            } else if (message.method === 'query') {
              result = {state: nativeStates.get(message.requestId) || 'missing', taskId: 'task', expiresAt: now + 10_000};
            } else if (message.method === 'commit') {
              assert.equal(item.state, 'interrupted'); starts++;
              nativeStates.set(message.requestId, 'committed'); result = {state: 'committed', taskId: 'task'};
            } else {
              operations.push('abort'); result = {state: 'aborted'};
            }
            port.onMessage.emit({version: 1, id: message.id, ok: true, result});
          };
          if (message.method === 'prepare' && holdPrepare) held = respond;
          else queueMicrotask(respond);
        }};
      ports.push(port); return port;
    }},
    action: {async setBadgeText() {}},
    alarms: {onAlarm: event(), async create() {}},
  };
  const worker = installWorker(api, {now: () => now, makeId: () => `id-${++sequence}`});
  return {api, worker, values, operations, item, ports, get starts() { return starts; },
    get held() { return held; }, hold() { holdPrepare = true; },
    observe() {
      const details = {requestId: 'request', url: item.url, method: 'GET', timeStamp: now};
      api.webRequest.onBeforeRequest.emit(details);
      api.webRequest.onSendHeaders.emit({...details, requestHeaders: [{name: 'Accept', value: '*/*'}]});
      api.webRequest.onHeadersReceived.emit({...details, statusCode: 200});
    },
  };
}
test('listeners register synchronously with non-blocking header observation', async () => {
  const f = fixture();
  assert.equal(f.api.downloads.onCreated.listeners.length, 1);
  assert.equal(f.api.webRequest.onBeforeRequest.listeners.length, 1);
  assert.deepEqual(f.api.webRequest.onSendHeaders.listeners[0].args[1], ['requestHeaders', 'extraHeaders']);
  await f.worker.ready; await f.worker.recover(); f.worker.close();
});
test('real worker wiring coordinates observed download through browser and native adapters', async () => {
  const f = fixture(); await f.worker.ready; f.observe();
  f.api.downloads.onCreated.emit({...f.item});
  await waitFor(() => f.values['handoff.v1.regular.1']?.phase === 'committed');
  assert.equal(f.starts, 1); assert.deepEqual(f.operations, ['pause', 'cancel']); f.worker.close();
});
test('disabled capture never pauses browser or connects native host', async () => {
  const f = fixture({enabled: false}); await f.worker.ready; f.observe();
  f.api.downloads.onCreated.emit({...f.item}); await f.worker.recover();
  assert.equal(f.ports.length, 0); assert.deepEqual(f.operations, []); f.worker.close();
});
test('missing host resumes browser and records release', async () => {
  const f = fixture({missingHost: true}); await f.worker.ready; f.observe();
  f.api.downloads.onCreated.emit({...f.item});
  await waitFor(() => f.values['handoff.v1.regular.1']?.phase === 'released');
  assert.equal(f.item.paused, false); assert.equal(f.starts, 0);
  assert.equal(f.operations.includes('cancel'), false); f.worker.close();
});
test('turning capture off during prepare prevents subsequent browser cancellation', async () => {
  const f = fixture(); await f.worker.ready; f.hold(); f.observe();
  f.api.downloads.onCreated.emit({...f.item}); await waitFor(() => Boolean(f.held));
  await f.api.storage.local.set({[SETTINGS_KEY]: {enabled: false, origins: []}});
  f.held(); await waitFor(() => f.values['handoff.v1.regular.1']?.phase === 'released');
  assert.equal(f.item.paused, false); assert.equal(f.starts, 0);
  assert.equal(f.operations.includes('cancel'), false); f.worker.close();
});
test('worker without observed request history leaves existing download alone', async () => {
  const f = fixture(); await f.worker.ready;
  f.api.downloads.onCreated.emit({...f.item}); await f.worker.recover();
  assert.deepEqual(f.operations, []); f.worker.close();
});
test('recovery releases pre-cancel reservation even when capture is disabled', async () => {
  const record = {downloadId: 1, requestId: 'old', taskId: 'task', expiresAt: 9999999999999,
    phase: 'prepared', ownsPause: true};
  const f = fixture({enabled: false, records: {'handoff.v1.regular.1': record}});
  f.item.paused = true; await f.worker.ready; await f.worker.recover();
  assert.equal(f.values['handoff.v1.regular.1'].phase, 'released');
  assert.equal(f.item.paused, false); f.worker.close();
});
test('origin settings reject malformed input and normalize explicit origins', () => {
  assert.deepEqual(normalizeSettings({enabled: true, origins: ['https://example.test:443']}),
    {enabled: true, origins: ['https://example.test']});
  for (const origin of ['https://example.test/path', 'https://user:pass@example.test', 'file:///tmp/a', '*.example.test']) {
    assert.deepEqual(normalizeSettings({enabled: true, origins: [origin]}), {enabled: false, origins: []});
  }
});
test('a malformed transaction cannot block recovery of another prepared download', async () => {
  const record = {downloadId: 1, requestId: 'old', taskId: 'task', expiresAt: 9999999999999,
    phase: 'prepared', ownsPause: true};
  const f = fixture({enabled: false, records: {
    'handoff.v1.regular.2': {downloadId: 2, requestId: 'broken', ownsPause: false, phase: 'invalid'},
    'handoff.v1.regular.1': record,
  }});
  f.item.paused = true;
  await f.worker.ready;
  await assert.doesNotReject(f.worker.recover());
  assert.equal(f.values['handoff.v1.regular.1'].phase, 'released');
  assert.equal(f.item.paused, false); f.worker.close();
});
test('a corrupt journal value cannot hide valid recovery records', async () => {
  const record = {downloadId: 1, requestId: 'old', taskId: 'task', expiresAt: 9999999999999,
    phase: 'prepared', ownsPause: true};
  const f = fixture({enabled: false, records: {'handoff.v1.regular.bad': null, 'handoff.v1.regular.1': record}});
  f.item.paused = true; await f.worker.ready;
  await assert.doesNotReject(f.worker.recover());
  assert.equal(f.values['handoff.v1.regular.1'].phase, 'released');
  assert.equal(f.values['handoff.v1.regular.bad'], null);
  f.worker.close();
});
