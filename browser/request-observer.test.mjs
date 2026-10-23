import test from 'node:test';
import assert from 'node:assert/strict';
import {RequestObserver} from './request-observer.mjs';

function fixture(options = {}) {
  let clock = Date.parse('2026-10-20T00:00:00Z');
  const observer = new RequestObserver({now: () => clock, allowReplay: () => true, ...options});
  const url = 'https://example.test/export?token=opaque';
  const begin = (id = 'r', requestUrl = url, method = 'GET') => observer.begin({requestId: id, url: requestUrl, method, timeStamp: clock});
  const headers = (id = 'r', requestUrl = url, extra = []) => observer.headers({requestId: id, url: requestUrl, requestHeaders: [{name: 'Accept', value: '*/*'}, ...extra]});
  const response = (id = 'r', requestUrl = url, statusCode = 200) => observer.response({requestId: id, url: requestUrl, statusCode});
  return {observer, url, begin, headers, response,
    full(id = 'r', requestUrl = url, method = 'GET') { begin(id, requestUrl, method); headers(id, requestUrl); response(id, requestUrl); },
    item(extra = {}) { return {id: 1, url, state: 'in_progress', paused: false, startTime: new Date(clock).toISOString(), ...extra}; },
    advance(ms) { clock += ms; }};
}
test('extensionless download with complete GET observation is eligible', () => {
  const f = fixture(); f.full();
  const result = f.observer.match(f.item());
  assert.equal(result.request.url, f.url);
  assert.equal(result.request.uniqueMatch, true);
});
test('origin policy defaults to disabled', () => {
  const f = fixture({allowReplay: undefined}); f.full();
  assert.equal(f.observer.match(f.item()).reason, 'origin_not_enabled');
});
test('same URL concurrent requests remain ambiguous even if one is POST', () => {
  const f = fixture(); f.full('get'); f.full('post', f.url, 'POST');
  assert.equal(f.observer.match(f.item()).reason, 'ambiguous_request');
});
test('missing history after worker restart cannot be inferred from URL', () => {
  const f = fixture(); assert.equal(f.observer.match(f.item()).reason, 'request_not_observed');
});
test('headers and response must both be observed', () => {
  for (const missing of ['headers', 'response']) {
    const f = fixture(); f.begin();
    if (missing !== 'headers') f.headers();
    if (missing !== 'response') f.response();
    assert.equal(f.observer.match(f.item()).reason, 'request_not_replayable');
  }
});
test('Cookie, authentication, Referer, Range and custom headers stay in browser', () => {
  for (const name of ['Cookie', 'Authorization', 'Proxy-Authorization', 'Referer', 'Range', 'X-Api-Key', 'Origin']) {
    const f = fixture(); f.begin(); f.headers('r', f.url, [{name, value: 'sensitive'}]); f.response();
    assert.equal(f.observer.match(f.item()).reason, 'request_not_replayable');
    assert.equal(JSON.stringify([...f.observer.requests.values()]).includes('sensitive'), false);
  }
});
test('observed POST, partial response and failed response are rejected', () => {
  for (const [method, status] of [['POST', 200], ['GET', 206], ['GET', 404]]) {
    const f = fixture(); f.begin('r', f.url, method); f.headers(); f.response('r', f.url, status);
    assert.equal(f.observer.match(f.item()).reason, 'request_not_replayable');
  }
});
test('redirect chain preserves original URL and checks every observed hop', () => {
  const f = fixture(), target = 'https://cdn.test/object';
  f.begin(); f.headers(); f.response('r', f.url, 302);
  f.observer.redirect({requestId: 'r', url: f.url, redirectUrl: target});
  f.begin('r', target); f.headers('r', target); f.response('r', target);
  const result = f.observer.match(f.item({finalUrl: target}));
  assert.equal(result.request.url, f.url); assert.equal(result.request.finalUrl, target);
});
test('unobserved redirect or disabled redirect origin cannot be accepted', () => {
  const f = fixture({allowReplay: origin => origin === 'https://example.test'}), target = 'https://cdn.test/object';
  f.full(); f.begin('r', target); f.headers('r', target); f.response('r', target);
  assert.equal(f.observer.match(f.item({finalUrl: target})).reason, 'request_not_replayable');
});
test('fully observed redirect still requires permission from origin policy at destination', () => {
  const f = fixture({allowReplay: origin => origin === 'https://example.test'}), target = 'https://cdn.test/object';
  f.begin(); f.headers(); f.response('r', f.url, 302);
  f.observer.redirect({requestId: 'r', url: f.url, redirectUrl: target});
  f.begin('r', target); f.headers('r', target); f.response('r', target);
  assert.equal(f.observer.match(f.item({finalUrl: target})).reason, 'origin_not_enabled');
});
test('authentication challenge cannot be hidden by a subsequent successful response', () => {
  const f = fixture(); f.begin(); f.headers(); f.response('r', f.url, 401); f.response();
  assert.equal(f.observer.match(f.item()).reason, 'request_not_replayable');
});
test('redirect history remains bounded when the same request ID repeats', () => {
  const f = fixture();
  for (let index = 0; index < 1000; index++) f.begin();
  assert.equal(f.observer.requests.get('r').hops.length, 10);
  assert.equal(f.observer.match(f.item()).reason, 'observation_overflow');
});
test('overflow quarantine prevents evicted competing request becoming a unique match', () => {
  const f = fixture({maxEntries: 1}); f.full('first'); f.full('second');
  assert.equal(f.observer.match(f.item()).reason, 'observation_overflow');
  f.advance(30_001); f.full('new');
  assert.ok(f.observer.match(f.item()).request);
});
test('claimed web request cannot be assigned to a second download', () => {
  const f = fixture(); f.full();
  assert.ok(f.observer.match(f.item()).request);
  assert.ok(f.observer.match(f.item()).request);
  assert.equal(f.observer.match(f.item({id: 2})).reason, 'request_already_claimed');
});
test('private, extension initiated, complete and paused downloads are untouched', () => {
  for (const extra of [{incognito: true}, {byExtensionId: 'another'}, {state: 'complete'}, {paused: true}]) {
    const f = fixture(); f.full(); assert.equal(f.observer.match(f.item(extra)).reason, 'browser_not_eligible');
  }
});
test('expired observation and unsupported blob downloads are not matched', () => {
  const f = fixture(); f.full(); f.advance(30_001);
  assert.equal(f.observer.match(f.item()).reason, 'request_not_observed');
  assert.equal(f.observer.match(f.item({url: 'blob:https://example.test/id'})).reason, 'download_context_missing');
});
