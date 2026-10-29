// Installs the extension's worker code explicitly in its service worker, then
// triggers a download, so a module that never self-installed can be told apart
// from a matcher that refuses.
import { writeFileSync } from 'node:fs';

const port = Number(process.argv[2] ?? 9334);
const sitePort = Number(process.argv[3] ?? 47654);
const outFile = process.argv[4] ?? 'tmp/browser-probe/install.json';
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
      logs.push(`${message.params.type}: ${message.params.args
        .map((arg) => arg.value ?? arg.description ?? arg.type).join(' ')}`);
    }
    if (message.method === 'Runtime.exceptionThrown') {
      logs.push(`exception: ${message.params.exceptionDetails.exception?.description
        ?? message.params.exceptionDetails.text}`);
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
  return response.json();
}

const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const workerTarget = targets.find((entry) => entry.type === 'service_worker'
  && entry.url.includes(extensionId));
const extensionTarget = workerTarget
  ?? await openTarget(`chrome-extension://${extensionId}/popup.html`);
const ext = await connect(extensionTarget.webSocketDebuggerUrl);
await ext.send('Runtime.enable');

const evaluate = async (expression) => {
  const answer = await ext.send('Runtime.evaluate',
    { expression, awaitPromise: true, returnByValue: true });
  if (answer.result?.exceptionDetails) {
    return { error: answer.result.exceptionDetails.exception?.description
      ?? answer.result.exceptionDetails.text };
  }
  return answer.result?.result?.value;
};

const imported = await evaluate(`(async () => {
  try {
    const module = await import(chrome.runtime.getURL('worker.mjs'));
    globalThis.__handoff = module.installWorker(chrome);
    return 'installed:' + typeof globalThis.__handoff.ready;
  } catch (error) { return 'import-failed:' + String(error); }
})()`);
const ready = await evaluate(`globalThis.__handoff ? globalThis.__handoff.ready.then(
  () => 'ready') : 'no-install'`);

await evaluate(`chrome.storage.local.set({'handoff.settings.v1':
  {enabled: true, origins: ['http://127.0.0.1:${sitePort}']}}).then(() => 'ok')`);
await new Promise((resolve) => setTimeout(resolve, 500));

const site = await openTarget(`http://127.0.0.1:${sitePort}/`);
const page = await connect(site.webSocketDebuggerUrl);
await new Promise((resolve) => setTimeout(resolve, 900));
await page.send('Runtime.evaluate', {
  expression: "(() => { const link = document.querySelector('a');"
    + " if (!link) return 'no link'; link.click(); return 'clicked'; })()",
  returnByValue: true,
});

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

const report = { imported, ready, context: workerTarget ? 'service-worker' : 'popup-page',
  record, logs: ext.logs.slice(-30) };
writeFileSync(outFile, JSON.stringify(report, null, 2), 'utf8');
console.log(JSON.stringify(report, null, 2));

page.socket.close();
ext.socket.close();
