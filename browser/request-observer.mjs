// In-memory, bounded correlation. Missing worker history is never reconstructed
// by guessing that the latest request for a URL must be the download request.
function httpUrl(value) {
  if (typeof value !== 'string' || value.length > 8192) return undefined;
  try {
    const url = new URL(value);
    if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password) return undefined;
    url.hash = '';
    return url.href.length <= 8192 ? url.href : undefined;
  } catch { return undefined; }
}
const ordinaryHeaders = new Set(['accept', 'accept-encoding', 'accept-language', 'host',
  'user-agent', 'connection', 'cache-control', 'pragma', 'upgrade-insecure-requests', 'priority']);

export class RequestObserver {
  constructor({now = Date.now, ttlMs = 30_000, maxEntries = 256,
    allowReplay = () => false} = {}) {
    if (!Number.isSafeInteger(ttlMs) || ttlMs < 1 || ttlMs > 120_000
      || !Number.isSafeInteger(maxEntries) || maxEntries < 1 || maxEntries > 4096) throw new Error('Invalid observer limits');
    Object.assign(this, {now, ttlMs, maxEntries, allowReplay});
    this.requests = new Map();
    this.quarantineUntil = 0;
  }
  #prune() {
    for (const [id, record] of this.requests) {
      if (this.now() - record.seen > this.ttlMs) this.requests.delete(id);
    }
  }
  begin(details) {
    this.#prune();
    const url = httpUrl(details.url);
    if (!url || typeof details.requestId !== 'string' || !details.requestId || details.requestId.length > 128
      || !Number.isFinite(details.timeStamp)) return;
    let record = this.requests.get(details.requestId);
    if (record) {
      if (record.hops.length >= 10) {
        record.invalid = true;
        this.quarantineUntil = this.now() + this.ttlMs;
        return;
      }
      if (record.expected !== url) record.invalid = true;
      record.expected = undefined;
      record.hops.push({url, method: details.method, headers: false, response: false});
      record.seen = this.now();
      return;
    }
    if (this.requests.size >= this.maxEntries) {
      // Eviction loses evidence of competing requests. Suppress all matching
      // until the entire affected correlation window has elapsed.
      this.requests.delete(this.requests.keys().next().value);
      this.quarantineUntil = this.now() + this.ttlMs;
    }
    record = {firstUrl: url, start: details.timeStamp, seen: this.now(),
      private: details.incognito === true, invalid: false, hops: [
        {url, method: details.method, headers: false, response: false},
      ]};
    this.requests.set(details.requestId, record);
  }
  headers(details) {
    const record = this.requests.get(details.requestId);
    const hop = record?.hops.at(-1);
    if (!hop) return;
    if (hop.url !== httpUrl(details.url) || !Array.isArray(details.requestHeaders)) {
      record.invalid = true; return;
    }
    hop.headers = true;
    // No credential/header values are retained. The first adapter only hands
    // off ordinary public GETs; authenticated/referrer/custom-header replay is
    // a separate feature requiring support in the native transfer engine.
    for (const header of details.requestHeaders) {
      const name = String(header.name).toLowerCase();
      if (ordinaryHeaders.has(name) || name.startsWith('sec-fetch-') || name.startsWith('sec-ch-')) continue;
      // A link click carries a Referer. The native engine fetches without it, so
      // the replay drops the header; what matters is that the referring origin is
      // one the user already allowed, which is checked here.
      if (name === 'referer') {
        try {
          if (this.allowReplay(new URL(String(header.value)).origin) === true) continue;
        } catch { /* an unparsable referer is treated like any other extra header */ }
      }
      record.invalid = true;
    }
  }
  response(details) {
    const record = this.requests.get(details.requestId), hop = record?.hops.at(-1);
    if (!hop) return;
    if (hop.url !== httpUrl(details.url)) { record.invalid = true; return; }
    if (![200, 301, 302, 303, 307, 308].includes(details.statusCode)) record.invalid = true;
    hop.response = true; hop.statusCode = details.statusCode;
  }
  redirect(details) {
    const record = this.requests.get(details.requestId), hop = record?.hops.at(-1);
    if (!hop) return;
    const target = httpUrl(details.redirectUrl);
    if (!target || hop.url !== httpUrl(details.url) || !hop.headers || !hop.response
      || ![301, 302, 303, 307, 308].includes(hop.statusCode)) record.invalid = true;
    record.expected = target;
  }
  error(details) {
    const record = this.requests.get(details.requestId);
    if (!record) return;
    // When a navigation turns into a download, the browser aborts the navigation
    // after the response has already arrived; that abort is the download taking
    // over, not a failed request.
    const hop = record.hops.at(-1);
    if (details.error === 'net::ERR_ABORTED' && hop?.response && hop.statusCode === 200) return;
    record.invalid = true;
  }
  match(item) {
    this.#prune();
    if (this.now() < this.quarantineUntil) return {reason: 'observation_overflow'};
    if (!Number.isSafeInteger(item.id) || item.id < 0 || item.incognito === true
      || item.byExtensionId || item.state !== 'in_progress' || item.paused) return {reason: 'browser_not_eligible'};
    const url = httpUrl(item.url), finalUrl = httpUrl(item.finalUrl || item.url);
    const started = Date.parse(item.startTime);
    if (!url || !finalUrl || !Number.isFinite(started) || Math.abs(this.now() - started) > this.ttlMs) {
      return {reason: 'download_context_missing'};
    }
    const candidates = [...this.requests.entries()].filter(([, record]) => record.firstUrl === url
      && record.hops.at(-1).url === finalUrl && Math.abs(started - record.start) <= this.ttlMs);
    if (candidates.length !== 1) return {reason: candidates.length ? 'ambiguous_request' : 'request_not_observed'};
    const [requestId, record] = candidates[0];
    if (record.claimedBy !== undefined && record.claimedBy !== item.id) return {reason: 'request_already_claimed'};
    if (record.invalid || record.private || record.expected || record.hops.some(hop =>
      hop.method !== 'GET' || !hop.headers || !hop.response) || record.hops.at(-1).statusCode !== 200) {
      return {reason: 'request_not_replayable'};
    }
    // A policy must explicitly permit every origin in a redirect chain. Header
    // observation alone cannot establish absence of hidden browser credentials.
    try {
      if (!record.hops.every(hop => this.allowReplay(new URL(hop.url).origin) === true)) {
        return {reason: 'origin_not_enabled'};
      }
    } catch { return {reason: 'origin_not_enabled'}; }
    record.claimedBy = item.id;
    return {request: {url, finalUrl, method: 'GET', uniqueMatch: true, replaySafe: true,
      observationId: requestId, origins: [...new Set(record.hops.map(hop => new URL(hop.url).origin))]}};
  }
}
