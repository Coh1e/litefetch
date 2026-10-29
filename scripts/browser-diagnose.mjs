// Diagnostic run for the click-to-handoff gap: watches the extension's own logs
// while a real page download happens, so the next step is chosen from evidence.
//
// It installs probe listeners in the extension context (download created, request
// observed) and prints everything the extension logs, plus the journal at the end.
import { writeFileSync } from 'node:fs';

const port = Number(process.argv[2] ?? 9334);
const sitePort = Number(process.argv[3] ?? 47654);
const outFile = process.argv[4] ?? 'tmp/browser-probe/diagnose.json';
const extensionId = 'namdpafnjehflfkcgdbnmoccndohmjoj';

async function connect(wsUrl) {
  const socket = new WebSocket(wsUrl);
  const pending = new Map();
  const logs = [];
  let nextId = 1;
  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    if (message.id && pending.has(message.id)) {
      pending.get(message.id)(message);
      pending.delete(message.id);
      return;
    }
    if (message.method === 'Runtime.consoleAPICalled') {
      const text = message.params.args
        .map((arg) => arg.value ?? arg.description ?? arg.type).join(' ');
      logs.push(`${message.params.type}: ${text}`);
    }
    if (message.method === 'Log.entryAdded') {
      logs.push(`log ${message.params.entry.level}: ${message.params.entry.text}`);
    }
  });
  await new Promise((resolve, reject) => {
    socket.addEventListener('open', resolve, { once: true });
    socket.addEventListener('error', reject, { once: true });
  });
  const send = (method, params) => {
    const id = nextId++;
    return new Promise((resolve) => {
      pending.set(id, resolve);
      socket.send(JSON.stringify({ id, method, params }));
    });
  };
  return { socket, send, logs };
}

async function openTarget(url) {
  const response = await fetch(`http://127.0.0.1:${port}/json/new?${encodeURIComponent(url)}`,
    { method: 'PUT' });
  if (!response.ok) throw new Error(`cannot open ${url}: ${response.status}`);
  return response.json();
}

const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const workerTarget = targets.find((entry) => entry.type === 'service_worker'
  && entry.url.includes(extensionId));
const extensionTarget = workerTarget
  ?? await openTarget(`chrome-extension://${extensionId}/popup.html`);
const ext = await connect(extensionTarget.webSocketDebuggerUrl);
await ext.send('Runtime.enable');
await ext.send('Log.enable');

const evaluate = async (expression) => {
  const answer = await ext.send('Runtime.evaluate',
    { expression, awaitPromise: true, returnByValue: true });
  if (answer.result?.exceptionDetails) {
    return { error: answer.result.exceptionDetails.exception?.description ?? 'exception' };
  }
  return answer.result?.result?.value;
};

const stored = await evaluate('chrome.storage.local.get(null)');
await evaluate(`chrome.storage.local.set({'handoff.settings.v1':
  {enabled: true, origins: ['http://127.0.0.1:${sitePort}']}}).then(() => 'ok')`);
await new Promise((resolve) => setTimeout(resolve, 400));

// Probe listeners: prove the events reach this context and what they carry.
await evaluate(`
  if (!globalThis.__probeInstalled) {
    globalThis.__probeInstalled = true;
    chrome.downloads.onCreated.addListener((item) => {
      console.log('PROBE created', item.id, item.url, item.state);
    });
    const probeFilter = {urls: ['http://*/*']};
    const log = (phase, details) => {
      if (String(details.url).includes('/handoff.bin')) {
        console.log('PROBE', phase, details.requestId, details.statusCode ?? '', details.type);
      }
    };
    chrome.webRequest.onBeforeRequest.addListener((d) => log('begin', d), probeFilter, []);
    chrome.webRequest.onSendHeaders.addListener((d) => log('headers', d), probeFilter,
      ['requestHeaders', 'extraHeaders']);
    chrome.webRequest.onHeadersReceived.addListener((d) => log('response', d), probeFilter,
      ['responseHeaders', 'extraHeaders']);
    chrome.webRequest.onBeforeRedirect.addListener((d) => log('redirect', d), probeFilter, []);
    chrome.webRequest.onErrorOccurred.addListener((d) => log('error', d), probeFilter, []);
  }
  'probes-ready'`);

const site = await openTarget(`http://127.0.0.1:${sitePort}/`);
const page = await connect(site.webSocketDebuggerUrl);
await new Promise((resolve) => setTimeout(resolve, 900));
await page.send('Runtime.evaluate', {
  expression: "(() => { const link = document.querySelector('a');"
    + " if (!link) return 'no link'; link.click(); return 'clicked'; })()",
  returnByValue: true,
});

// Watch the extension for a while, then read the journal and the downloads.
let record = null;
let storage = {};
const deadline = Date.now() + 30000;
while (Date.now() < deadline) {
  storage = await evaluate('chrome.storage.local.get(null)');
  const entries = Object.entries(storage ?? {})
    .filter(([key]) => key.startsWith('handoff.journal.'));
  if (entries.length) {
    record = entries[0][1];
    if (['committed', 'attention', 'released', 'cancelled'].includes(record.phase)) break;
  }
  await new Promise((resolve) => setTimeout(resolve, 400));
}

const downloads = await evaluate('chrome.downloads.search({limit: 3})');
const report = {
  context: workerTarget ? 'service-worker' : 'popup-page',
  settingsBefore: stored?.['handoff.settings.v1'] ?? null,
  record,
  logs: ext.logs.slice(-40),
  downloads: downloads?.map?.((item) => ({ id: item.id, state: item.state, url: item.url,
    bytesReceived: item.bytesReceived, totalBytes: item.totalBytes })),
};
writeFileSync(outFile, JSON.stringify(report, null, 2), 'utf8');
console.log(JSON.stringify(report, null, 2));

page.socket.close();
ext.socket.close();
