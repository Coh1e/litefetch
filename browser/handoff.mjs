// Browser-independent transaction logic. Adapters must implement durable atomic
// journal writes and a native prepare that cannot start network activity.
const phases = new Set(['pausing', 'paused', 'preparing', 'prepared', 'cancelling',
  'cancelled', 'committing', 'committed', 'released', 'attention']);
function sameRequest(item, request) {
  const normalize = value => {
    const url = new URL(value); url.hash = ''; return url.href;
  };
  try {
    return normalize(item.url) === normalize(request.url)
      && (!request.finalUrl || normalize(item.finalUrl || item.url) === normalize(request.finalUrl));
  } catch { return false; }
}
export class Handoff {
  constructor({journal, browser, native, now = Date.now, makeId = () => crypto.randomUUID(), canTakeOver = () => true}) {
    Object.assign(this, {journal, browser, native, now, makeId, canTakeOver});
    this.active = new Map();
  }
  run(downloadId, request) {
    if (!Number.isSafeInteger(downloadId) || downloadId < 0) throw new Error('Invalid download ID');
    if (this.active.has(downloadId)) return this.active.get(downloadId);
    const operation = this.#run(downloadId, request).finally(() => this.active.delete(downloadId));
    this.active.set(downloadId, operation);
    return operation;
  }
  async #write(record, phase, extra = {}) {
    // Never mutate our last durable snapshot before storage confirms success.
    const next = {...record, ...extra, phase};
    await this.journal.set(record.downloadId, next);
    return next;
  }
  async #release(record, reason) {
    // Failed abort cannot start a merely prepared native task. Its lease must
    // expire on the native side; no commit is sent along this path.
    try { await this.native.abort(record.requestId); } catch { /* lease owns cleanup */ }
    const item = await this.browser.get(record.downloadId);
    if (item?.state === 'in_progress' && item.paused && record.ownsPause) {
      try { await this.browser.resume(record.downloadId); }
      catch { return this.#write(record, 'attention', {reason: 'browser_resume_failed'}); }
    }
    return this.#write(record, 'released', {reason});
  }
  async #finish(record) {
    let status;
    try { status = await this.native.query(record.requestId); }
    catch { return this.#write(record, 'cancelled', {reason: 'native_status_unavailable'}); }
    if (status?.state === 'committed') {
      return this.#write(record, status.taskId === record.taskId ? 'committed' : 'attention',
        status.taskId === record.taskId ? {} : {reason: 'native_task_mismatch'});
    }
    if (status?.state !== 'prepared') {
      return this.#write(record, 'attention', {reason: 'native_task_not_prepared'});
    }
    if (status.taskId !== record.taskId || !Number.isSafeInteger(status.expiresAt)
      || status.expiresAt <= this.now()) {
      return this.#write(record, 'attention', {reason: 'native_lease_invalid'});
    }
    // Re-read the browser on recovery. A completed or still-active browser job
    // must never be treated as proof that our cancellation succeeded.
    const item = await this.browser.get(record.downloadId);
    if (item?.state !== 'interrupted' || item.error !== 'USER_CANCELED') {
      return this.#write(record, 'attention', {reason: 'browser_cancel_not_confirmed'});
    }
    record = await this.#write(record, 'committing');
    try {
      const result = await this.native.commit(record.requestId);
      if (result?.state === 'committed' && result.taskId === record.taskId) {
        return this.#write(record, 'committed');
      }
    } catch { /* Query on the next run; never recreate the browser request. */ }
    return this.#write(record, 'cancelled', {reason: 'commit_confirmation_missing'});
  }
  async #run(downloadId, request) {
    let record = await this.journal.get(downloadId);
    if (record) {
      if (record.downloadId !== downloadId || typeof record.requestId !== 'string'
        || !record.requestId || record.requestId.length > 128 || !phases.has(record.phase)
        || typeof record.ownsPause !== 'boolean') throw new Error('Invalid journal record');
      if (['prepared', 'cancelling', 'cancelled', 'committing', 'committed'].includes(record.phase)
        && (typeof record.taskId !== 'string' || !record.taskId
          || !Number.isSafeInteger(record.expiresAt) || record.expiresAt < 0)) throw new Error('Invalid journal record');
      if (['committed', 'released', 'attention'].includes(record.phase)) return record;
      if (['cancelled', 'committing'].includes(record.phase)) return this.#finish(record);
      if (['cancelling', 'pausing'].includes(record.phase)) {
        // A worker crash between the API call and the journal acknowledgement
        // makes ownership ambiguous. Preserve user actions for manual resolution.
        try { await this.native.abort(record.requestId); } catch { /* lease */ }
        return this.#write(record, 'attention', {reason: 'browser_action_ambiguous'});
      }
      // A prepare may have succeeded even if its acknowledgement was lost.
      // Before cancellation we can always release safely without replaying GET.
      return this.#release(record, 'worker_restarted_before_cancel');
    }
    let url;
    try { url = new URL(request?.url); } catch { return {phase: 'released', reason: 'unsupported_request'}; }
    if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password
      || request.method !== 'GET' || request.uniqueMatch !== true || request.replaySafe !== true) {
      return {phase: 'released', reason: 'unsupported_request'};
    }
    const initial = await this.browser.get(downloadId);
    if (initial?.state !== 'in_progress' || initial.paused || !sameRequest(initial, request)) {
      return {phase: 'released', reason: 'browser_not_available'};
    }
    record = {downloadId, requestId: this.makeId(), ownsPause: false};
    record = await this.#write(record, 'pausing');
    try { await this.browser.pause(downloadId); }
    catch { return this.#write(record, 'attention', {reason: 'browser_pause_failed'}); }
    record = await this.#write(record, 'paused', {ownsPause: true});
    record = await this.#write(record, 'preparing');
    let prepared;
    try { prepared = await this.native.prepare(record.requestId, request); }
    catch { return this.#release(record, 'native_prepare_unavailable'); }
    if (prepared?.state !== 'prepared' || typeof prepared.taskId !== 'string' || !prepared.taskId
      || !Number.isSafeInteger(prepared.expiresAt) || prepared.expiresAt <= this.now()
      || prepared.expiresAt > this.now() + 300_000) {
      return this.#release(record, 'native_prepare_rejected');
    }
    record = await this.#write(record, 'prepared', {taskId: prepared.taskId, expiresAt: prepared.expiresAt});
    const item = await this.browser.get(downloadId);
    if (item?.state !== 'in_progress' || !item.paused || !sameRequest(item, request)
      || record.expiresAt <= this.now() || !this.canTakeOver(request)) {
      return this.#release(record, 'browser_changed_before_cancel');
    }
    record = await this.#write(record, 'cancelling');
    if (!this.canTakeOver(request)) return this.#release(record, 'policy_changed_before_cancel');
    try { await this.browser.cancel(downloadId); }
    catch {
      // A rejected API promise is not proof of either success or failure.
      return this.#write(record, 'attention', {reason: 'browser_cancel_failed'});
    }
    const cancelled = await this.browser.get(downloadId);
    if (cancelled?.state !== 'interrupted' || cancelled.error !== 'USER_CANCELED') {
      return this.#release(record, 'browser_cancel_not_confirmed');
    }
    record = await this.#write(record, 'cancelled');
    return this.#finish(record);
  }
}
