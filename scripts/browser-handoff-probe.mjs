// Runs the extension's real worker code against a real page download and reports
// what the handoff did.
//
// An MV3 service worker that this CDP-loaded session never starts has no target
// to attach to, so the popup page is used instead: same extension origin, same
// chrome APIs. The result says which context was used.
import { writeFileSync } from 'node:fs';

const port = Number(process.argv[2] ?? 9334);
const sitePort = Number(process.argv[3] ?? 47654);
const outFile = process.argv[4] ?? 'tmp/browser-probe/handoff.json';
const extensionId = 'namdpafnjehflfkcgdbnmoccndohmjoj';

async function connect(wsUrl) {
  const socket = new WebSocket(wsUrl);
  const pending = new Map();
  let nextId = 1;
  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    if (message.id && pending.has(message.id)) {
      pending.get(message.id)(message);
      pending.delete(message.id);
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
  return { socket, send };
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

const evaluateExt = async (expression) => {
  const answer = await ext.send('Runtime.evaluate',
    { expression, awaitPromise: true, returnByValue: true });
  if (answer.result?.exceptionDetails) {
    throw new Error(JSON.stringify(answer.result.exceptionDetails.exception?.description
      ?? answer.result.exceptionDetails));
  }
  return answer.result?.result?.value;
};

const context = workerTarget ? 'service-worker' : 'popup-page';
if (!workerTarget) {
  await evaluateExt(`(async () => {
    const module = await import(chrome.runtime.getURL('worker.mjs'));
    module.installWorker(chrome);
    return 'installed';
  })()`);
}
await evaluateExt(`chrome.storage.local.set({'handoff.settings.v1':
  {enabled: true, origins: ['http://127.0.0.1:${sitePort}']}}).then(() => 'ok')`);
await new Promise((resolve) => setTimeout(resolve, 600));

// A real page, a real link, and a real download of the same address.
const site = await openTarget(`http://127.0.0.1:${sitePort}/`);
const page = await connect(site.webSocketDebuggerUrl);
await new Promise((resolve) => setTimeout(resolve, 900));
const clicked = await page.send('Runtime.evaluate', {
  expression: "(() => { const link = document.querySelector('a');"
    + " if (!link) return 'no link'; link.click(); return 'clicked'; })()",
  returnByValue: true,
});
const clickResult = clicked.result?.result?.value;
await page.send('Runtime.evaluate', {
  expression: "fetch('/handoff.bin').then(() => 'fetched').catch(() => 'failed')",
  awaitPromise: true, returnByValue: true,
});
const downloadId = await evaluateExt(
  `chrome.downloads.download({url: 'http://127.0.0.1:${sitePort}/handoff.bin',
    conflictAction: 'uniquify'})`);

let storage = {};
let record = null;
const deadline = Date.now() + 40000;
while (Date.now() < deadline) {
  storage = await evaluateExt('chrome.storage.local.get(null)');
  const entries = Object.entries(storage).filter(([key]) => key.startsWith('handoff.journal.'));
  if (entries.length) {
    record = entries[0][1];
    if (['committed', 'attention', 'released', 'cancelled'].includes(record.phase)) break;
  }
  await new Promise((resolve) => setTimeout(resolve, 400));
}

const downloads = await evaluateExt('chrome.downloads.search({limit: 5})');
writeFileSync(outFile,
  JSON.stringify({ context, clickResult, downloadId, record, storage, downloads }, null, 2), 'utf8');
console.log(JSON.stringify({
  context,
  clickResult,
  downloadId,
  record,
  downloads: downloads?.map((item) => ({ id: item.id, state: item.state, error: item.error })),
}, null, 2));

page.socket.close();
ext.socket.close();
