import {Handoff} from './handoff.mjs';
import {storageJournal, downloadsAdapter, NativeClient} from './adapters.mjs';
import {RequestObserver} from './request-observer.mjs';
import {SETTINGS_KEY, normalizeSettings} from './settings.mjs';

// Exported for event-driven integration tests; importing does not require Chrome.
export function installWorker(api, {now = Date.now, makeId = () => crypto.randomUUID()} = {}) {
  let settings = normalizeSettings(undefined), recovery, settingsRevision = 0;
  const journal = storageJournal(api.storage.local);
  const browser = downloadsAdapter(api.downloads);
  const native = new NativeClient(api.runtime, {makeId});
  const allowed = origin => settings.enabled && settings.origins.includes(origin);
  const observer = new RequestObserver({now, allowReplay: allowed});
  const controller = new Handoff({journal, browser, native, now, makeId,
    canTakeOver: request => settings.enabled && request.origins?.every(allowed) === true});
  const badge = text => api.action.setBadgeText({text}).catch(() => {});
  const reportFailure = () => badge('!');
  let ready;
  async function consider(item) {
    await ready;
    if (!settings.enabled) return;
    const result = observer.match(item);
    // Say why a download was left to the browser: without it, "the handoff did
    // nothing" and "the handoff is broken" look identical from the outside.
    if (!result.request) {
      console.debug('handoff: ignored', item.id, result.reason);
      return;
    }
    const record = await controller.run(item.id, result.request);
    if (record.phase === 'attention' || record.phase === 'cancelled') await badge('!');
  }
  async function recover() {
    if (recovery) return recovery;
    recovery = (async () => {
      await ready;
      const {records, invalidCount} = await journal.scan();
      if (invalidCount) await reportFailure();
      for (const record of records) {
        // Recover existing transactions even when capture has been disabled:
        // ownership transfer may already have happened before that preference.
        if (record.phase === 'attention') { await badge('!'); continue; }
        if (['committed', 'released'].includes(record.phase)) continue;
        try {
          const result = await controller.run(record.downloadId);
          if (result.phase === 'attention' || result.phase === 'cancelled') await badge('!');
        } catch {
          // Keep the original damaged record for inspection, but do not strand
          // unrelated paused/cancelled downloads behind it in the recovery queue.
          await reportFailure();
        }
      }
    })().finally(() => { recovery = undefined; });
    return recovery;
  }
  const filter = {urls: ['http://*/*', 'https://*/*']};
  // All listeners register synchronously at worker evaluation, before storage IO.
  api.webRequest.onBeforeRequest.addListener(details => { if (settings.enabled) observer.begin(details); }, filter);
  api.webRequest.onSendHeaders.addListener(details => { if (settings.enabled) observer.headers(details); }, filter, ['requestHeaders', 'extraHeaders']);
  api.webRequest.onHeadersReceived.addListener(details => { if (settings.enabled) observer.response(details); }, filter);
  api.webRequest.onBeforeRedirect.addListener(details => { if (settings.enabled) observer.redirect(details); }, filter);
  api.webRequest.onErrorOccurred.addListener(details => observer.error(details), filter);
  api.downloads.onCreated.addListener(item => { void consider(item).catch(reportFailure); });
  api.downloads.onChanged.addListener(delta => {
    void (async () => {
      await ready;
      const item = await browser.get(delta.id);
      if (item) await consider(item);
    })().catch(reportFailure);
  });
  api.storage.onChanged.addListener((changes, area) => {
    if (area === 'local' && Object.hasOwn(changes, SETTINGS_KEY)) {
      settingsRevision++;
      settings = normalizeSettings(changes[SETTINGS_KEY].newValue);
      observer.requests.clear(); // Never mix history captured under old policy.
    }
  });
  api.runtime.onStartup.addListener(() => { void recover().catch(reportFailure); });
  api.runtime.onInstalled.addListener(() => { void recover().catch(reportFailure); });
  api.alarms.onAlarm.addListener(alarm => {
    if (alarm.name === 'handoff-recovery') void recover().catch(reportFailure);
  });
  ready = api.storage.local.get(SETTINGS_KEY).then(values => {
    if (settingsRevision === 0) settings = normalizeSettings(values[SETTINGS_KEY]);
  });
  // Recreate the alarm on each worker evaluation; browser restarts may clear it.
  void api.alarms.create('handoff-recovery', {periodInMinutes: 1}).catch(reportFailure);
  void recover().catch(reportFailure);
  return {ready, recover, observer, controller, close: () => native.close()};
}

if (globalThis.chrome?.runtime?.id) installWorker(globalThis.chrome);
