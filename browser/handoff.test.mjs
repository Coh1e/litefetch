import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {Handoff} from './handoff.mjs';

const request = {url: 'https://example.test/file?signature=opaque', method: 'GET', uniqueMatch: true, replaySafe: true};
function fixture() {
  const records = new Map(), events = [];
  let item = {url: request.url, state: 'in_progress', paused: false};
  let nativeState = 'missing', starts = 0;
  const adapters = {
    journal: {
      async get(id) { return structuredClone(records.get(id)); },
      async set(id, value) { records.set(id, structuredClone(value)); events.push(`write:${value.phase}`); },
    },
    browser: {
      async get() { return {...item}; },
      async pause() { item.paused = true; events.push('pause'); },
      async resume() { item.paused = false; events.push('resume'); },
      async cancel() { item = {state: 'interrupted', paused: false, error: 'USER_CANCELED'}; events.push('cancel'); },
    },
    native: {
      async prepare() { nativeState = 'prepared'; events.push('prepare'); return {state: nativeState, taskId: 'task', expiresAt: 10_000}; },
      async query() { return {state: nativeState, taskId: 'task', expiresAt: 10_000}; },
      async commit() {
        assert.equal(item.state, 'interrupted');
        if (nativeState !== 'committed') starts++;
        nativeState = 'committed'; events.push('commit'); return {state: nativeState, taskId: 'task'};
      },
      async abort() { if (nativeState !== 'committed') nativeState = 'aborted'; events.push('abort'); },
    },
    now: () => 1000,
    makeId: () => 'request-id',
  };
  return {adapters, records, events, controller: () => new Handoff(adapters),
    get item() { return item; }, set item(value) { item = value; },
    get starts() { return starts; }};
}

test('prepare is durable before cancel; cancel is confirmed before native start', async () => {
  const f = fixture();
  assert.equal((await f.controller().run(1, request)).phase, 'committed');
  assert.ok(f.events.indexOf('write:prepared') < f.events.indexOf('cancel'));
  assert.ok(f.events.indexOf('write:cancelled') < f.events.indexOf('commit'));
  assert.equal(f.starts, 1);
  assert.equal(JSON.stringify([...f.records.values()]).includes('signature'), false);
  await f.controller().run(1, request);
  assert.equal(f.starts, 1);
});
test('concurrent duplicate browser events share one transaction', async () => {
  const f = fixture(), controller = f.controller();
  const results = await Promise.all(Array.from({length: 20}, () => controller.run(1, request)));
  assert.ok(results.every(r => r.phase === 'committed'));
  assert.equal(f.events.filter(e => e === 'prepare').length, 1);
  assert.equal(f.starts, 1);
});
test('native offline leaves browser able to continue', async () => {
  const f = fixture();
  f.adapters.native.prepare = async () => { throw new Error('offline'); };
  assert.equal((await f.controller().run(1, request)).phase, 'released');
  assert.equal(f.item.paused, false);
  assert.equal(f.item.state, 'in_progress');
  assert.equal(f.events.includes('cancel'), false);
});
test('lost prepare acknowledgement aborts and resumes without network start', async () => {
  const f = fixture(), prepare = f.adapters.native.prepare;
  f.adapters.native.prepare = async (...args) => { await prepare(...args); throw new Error('disconnected'); };
  await f.controller().run(1, request);
  assert.equal(f.item.paused, false);
  assert.equal(f.starts, 0);
});
test('commit acknowledgement loss is resolved by status after worker restart', async () => {
  const f = fixture(), commit = f.adapters.native.commit;
  f.adapters.native.commit = async (...args) => { await commit(...args); throw new Error('lost reply'); };
  assert.equal((await f.controller().run(1, request)).phase, 'cancelled');
  assert.equal((await f.controller().run(1)).phase, 'committed');
  assert.equal(f.starts, 1);
});
test('commit never reached server can be retried from durable cancelled state', async () => {
  const f = fixture(), commit = f.adapters.native.commit;
  f.adapters.native.commit = async () => { throw new Error('offline'); };
  await f.controller().run(1, request);
  assert.equal(f.starts, 0);
  f.adapters.native.commit = commit;
  assert.equal((await f.controller().run(1)).phase, 'committed');
  assert.equal(f.starts, 1);
});
test('storage failure before cancellation cannot cancel the browser', async () => {
  const f = fixture(), set = f.adapters.journal.set;
  f.adapters.journal.set = async (id, record) => {
    if (record.phase === 'cancelling') throw new Error('disk full');
    return set(id, record);
  };
  await assert.rejects(f.controller().run(1, request), /disk full/);
  assert.equal(f.events.includes('cancel'), false);
  f.adapters.journal.set = set;
  assert.equal((await f.controller().run(1)).phase, 'released');
  assert.equal(f.item.paused, false);
});
test('crash after browser cancel but before journal confirmation requires attention', async () => {
  const f = fixture(), set = f.adapters.journal.set;
  f.adapters.journal.set = async (id, record) => {
    if (record.phase === 'cancelled') throw new Error('worker terminated');
    return set(id, record);
  };
  await assert.rejects(f.controller().run(1, request));
  f.adapters.journal.set = set;
  assert.equal((await f.controller().run(1)).reason, 'browser_action_ambiguous');
  assert.equal(f.starts, 0);
});
test('user cancellation while native prepare is pending must not start native task', async () => {
  const f = fixture(), prepare = f.adapters.native.prepare;
  f.adapters.native.prepare = async (...args) => {
    const result = await prepare(...args);
    f.item = {state: 'interrupted', error: 'USER_CANCELED', paused: false};
    return result;
  };
  assert.equal((await f.controller().run(1, request)).phase, 'released');
  assert.equal(f.starts, 0);
});
test('browser completion during pause/prepare releases native reservation', async () => {
  const f = fixture(), prepare = f.adapters.native.prepare;
  f.adapters.native.prepare = async (...args) => {
    const result = await prepare(...args); f.item = {state: 'complete'}; return result;
  };
  assert.equal((await f.controller().run(1, request)).phase, 'released');
  assert.equal(f.events.includes('cancel'), false);
});
test('expired prepare lease prevents browser cancellation', async () => {
  const f = fixture();
  f.adapters.native.prepare = async () => ({state: 'prepared', taskId: 'task', expiresAt: 999});
  assert.equal((await f.controller().run(1, request)).phase, 'released');
  assert.equal(f.events.includes('cancel'), false);
});
test('ambiguous and unsupported requests never pause browser', async () => {
  for (const change of [{method: 'POST'}, {url: 'blob:https://example.test/id'},
    {uniqueMatch: false}, {replaySafe: false}, {url: 'https://user:password@example.test/file'}]) {
    const f = fixture();
    assert.equal((await f.controller().run(1, {...request, ...change})).phase, 'released');
    assert.deepEqual(f.events, []);
  }
});
test('pre-existing user pause is preserved', async () => {
  const f = fixture(); f.item.paused = true;
  await f.controller().run(1, request);
  assert.deepEqual(f.events, []);
});
test('different native task cannot confirm a commit', async () => {
  const f = fixture();
  f.records.set(1, {downloadId: 1, requestId: 'r', taskId: 'task', expiresAt: 10_000, ownsPause: true, phase: 'committing'});
  f.adapters.native.query = async () => ({state: 'committed', taskId: 'other'});
  assert.equal((await f.controller().run(1)).phase, 'attention');
});
test('native disconnection after cancel preserves recoverable record without replay', async () => {
  const f = fixture(); f.adapters.native.query = async () => { throw new Error('offline'); };
  assert.equal((await f.controller().run(1, request)).reason, 'native_status_unavailable');
  assert.equal(f.item.state, 'interrupted');
  assert.equal(f.events.includes('resume'), false);
  assert.equal(f.starts, 0);
});
test('file-backed journal survives a fresh controller and lost commit acknowledgement', async () => {
  // Test files stay inside the project; the path does not depend on the cwd.
  const parent = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../tmp/verification');
  await fs.mkdir(parent, {recursive: true});
  const directory = await fs.mkdtemp(path.join(parent, 'handoff-'));
  try {
    const f = fixture(), file = path.join(directory, 'journal.json');
    f.adapters.journal = {
      async get() { try { return JSON.parse(await fs.readFile(file, 'utf8')); } catch (e) { if (e.code === 'ENOENT') return undefined; throw e; } },
      async set(id, record) { await fs.writeFile(file + '.new', JSON.stringify(record)); await fs.rename(file + '.new', file); },
    };
    const commit = f.adapters.native.commit;
    f.adapters.native.commit = async (...args) => { await commit(...args); throw new Error('lost'); };
    await f.controller().run(1, request);
    assert.equal((await f.controller().run(1)).phase, 'committed');
    assert.equal(f.starts, 1);
  } finally {
    assert.equal(path.dirname(path.resolve(directory)), parent);
    await fs.rm(directory, {recursive: true});
  }
});
test('rejected cancel never sends native commit', async () => {
  const f = fixture();
  f.adapters.browser.cancel = async () => { throw new Error('browser rejected'); };
  assert.equal((await f.controller().run(1, request)).reason, 'browser_cancel_failed');
  assert.equal(f.starts, 0);
});
test('resolved cancel promise without a cancelled download resumes browser', async () => {
  const f = fixture(); f.adapters.browser.cancel = async () => {};
  assert.equal((await f.controller().run(1, request)).phase, 'released');
  assert.equal(f.item.paused, false);
  assert.equal(f.starts, 0);
});
test('corrupt journal fails closed without browser or native side effects', async () => {
  const f = fixture(); f.records.set(1, {downloadId: 9, requestId: 'r', phase: 'committed'});
  await assert.rejects(f.controller().run(1), /Invalid journal/);
  assert.deepEqual(f.events, []);
});
test('cancelled journal missing native identity cannot be promoted to committed', async () => {
  const f = fixture();
  f.records.set(1, {downloadId: 1, requestId: 'r', ownsPause: true, phase: 'cancelled'});
  f.adapters.native.query = async () => ({state: 'committed'});
  await assert.rejects(f.controller().run(1), /Invalid journal/);
  assert.deepEqual(f.events, []);
});
test('observed request must match browser URL before pausing', async () => {
  const f = fixture(); f.item.url = 'https://example.test/other';
  assert.equal((await f.controller().run(1, request)).phase, 'released');
  assert.deepEqual(f.events, []);
});
test('redirect destination changing during native preparation prevents cancellation', async () => {
  const f = fixture(), prepare = f.adapters.native.prepare;
  f.adapters.native.prepare = async (...args) => {
    const result = await prepare(...args); f.item.finalUrl = 'https://example.test/different'; return result;
  };
  assert.equal((await f.controller().run(1, {...request, finalUrl: request.url})).phase, 'released');
  assert.equal(f.events.includes('cancel'), false);
  assert.equal(f.item.paused, false);
});
