import test from 'node:test';
import assert from 'node:assert/strict';
import {storageJournal, downloadsAdapter, NativeClient} from './adapters.mjs';
import {Handoff} from './handoff.mjs';

function event() {
  const listeners = new Set();
  return {addListener: f => listeners.add(f), removeListener: f => listeners.delete(f),
    emit(value) { for (const f of [...listeners]) f(value); }, get count() { return listeners.size; }};
}
function runtimeFixture() {
  const ports = [];
  let sequence = 0;
  const runtime = {connectNative() {
    const port = {onMessage: event(), onDisconnect: event(), messages: [], disconnected: false,
      postMessage(message) { this.messages.push(message); },
      disconnect() { this.disconnected = true; this.onDisconnect.emit(); }};
    ports.push(port); return port;
  }};
  return {runtime, ports, client: () => new NativeClient(runtime, {timeoutMs: 50, makeId: () => `message-${++sequence}`}),
    reply(port, message, result) { port.onMessage.emit({version: 1, id: message.id, ok: true, result}); }};
}
function storageFixture() {
  const values = {};
  return {async get(key) { return structuredClone(key === null ? values : {[key]: values[key]}); },
    async set(update) { Object.assign(values, structuredClone(update)); }};
}
test('journal uses separate keys, scopes, and survives adapter replacement', async () => {
  const storage = storageFixture(), journal = storageJournal(storage);
  await journal.set(1, {downloadId: 1, phase: 'prepared'});
  await journal.set(2, {downloadId: 2, phase: 'cancelled'});
  assert.equal((await storageJournal(storage).get(1)).phase, 'prepared');
  assert.equal((await journal.list()).length, 2);
  assert.deepEqual(await storageJournal(storage, 'private').list(), []);
});
test('journal propagates quota errors before acknowledging writes', async () => {
  const storage = storageFixture(); storage.set = async () => { throw new Error('quota'); };
  await assert.rejects(storageJournal(storage).set(1, {downloadId: 1}), /quota/);
});
test('browser adapter preserves unavailable downloads and validates IDs', async () => {
  const adapter = downloadsAdapter({search: async () => []});
  assert.equal(await adapter.get(1), undefined);
  await assert.rejects(adapter.get(-1), /Invalid download ID/);
  await assert.rejects(downloadsAdapter({search: async () => [{id: 2, state: 'complete'}]}).get(1));
});
test('browser operation timeout rejects instead of hanging', async () => {
  const adapter = downloadsAdapter({pause: () => new Promise(() => {})}, 5);
  await assert.rejects(adapter.pause(1), /timed out/);
});
test('native requests can resolve out of order using separate message IDs', async () => {
  const f = runtimeFixture(), client = f.client();
  const a = client.query('a'), b = client.query('b'), port = f.ports[0];
  assert.equal(port.messages.length, 2);
  f.reply(port, port.messages[1], {state: 'missing'});
  f.reply(port, port.messages[0], {state: 'prepared', taskId: 'task'});
  assert.equal((await a).state, 'prepared'); assert.equal((await b).state, 'missing');
  client.close(); assert.equal(port.onMessage.count, 0);
});
test('native disconnection rejects all pending requests and permits reconnect', async () => {
  const f = runtimeFixture(), client = f.client();
  const a = client.query('a'), b = client.query('b');
  const results = Promise.allSettled([a, b]);
  f.ports[0].onDisconnect.emit();
  assert.ok((await results).every(r => r.status === 'rejected'));
  const c = client.query('c'); const port = f.ports[1];
  f.reply(port, port.messages[0], {state: 'committed', taskId: 'task'});
  assert.equal((await c).state, 'committed'); client.close();
});
test('native timeout disconnects; late reply cannot resolve a future operation', async () => {
  const f = runtimeFixture(), client = f.client();
  await assert.rejects(client.commit('a'), /timed out/);
  assert.equal(f.ports[0].disconnected, true);
  const next = client.query('a'), port = f.ports[1];
  f.reply(f.ports[0], f.ports[0].messages[0], {state: 'committed'});
  f.reply(port, port.messages[0], {state: 'prepared'});
  assert.equal((await next).state, 'prepared'); client.close();
});
test('malformed or uncorrelated native reply closes the channel', async () => {
  for (const message of [{version: 9, id: 'message-1', ok: true}, {version: 1, id: 'unknown', ok: true}]) {
    const f = runtimeFixture(), client = f.client();
    const request = client.query('a');
    f.ports[0].onMessage.emit(message);
    await assert.rejects(request, /Invalid native protocol/); client.close();
  }
});
test('oversized native request is rejected without opening a port', async () => {
  const f = runtimeFixture(), client = f.client();
  await assert.rejects(client.prepare('a', {url: 'x'.repeat(70_000)}), /too large/);
  assert.equal(f.ports.length, 0);
});
test('explicit native rejection is propagated without logging its payload', async () => {
  const f = runtimeFixture(), client = f.client(), pending = client.prepare('a', {url: 'secret'});
  const port = f.ports[0];
  port.onMessage.emit({version: 1, id: port.messages[0].id, ok: false, error: 'secret'});
  await assert.rejects(pending, error => error.message === 'Native request rejected'); client.close();
});
test('coordinator plus adapters resumes browser if Native Messaging disconnects during prepare', async () => {
  const f = runtimeFixture(), client = f.client(), storage = storageFixture();
  const item = {id: 1, url: 'https://example.test/file', state: 'in_progress', paused: false};
  let cancels = 0;
  const downloads = {search: async () => [{...item}], pause: async () => { item.paused = true; },
    resume: async () => { item.paused = false; }, cancel: async () => { cancels++; }};
  const controller = new Handoff({journal: storageJournal(storage), browser: downloadsAdapter(downloads),
    native: client, makeId: () => 'transaction', now: () => 1});
  const operation = controller.run(1, {url: 'https://example.test/file', method: 'GET', uniqueMatch: true, replaySafe: true});
  // Port messages run through the real adapter; the simulated host disappears.
  while (!f.ports.length) await new Promise(resolve => setTimeout(resolve, 1));
  f.ports[0].onDisconnect.emit();
  // Abort reconnects, then times out: a merely prepared host task cannot start.
  assert.equal((await operation).phase, 'released');
  assert.equal(item.paused, false); assert.equal(cancels, 0); client.close();
});
