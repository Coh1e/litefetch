// One process, one run: load the extension, attach to its service worker so it
// does not fall asleep, click a real link, and read the handoff journal.
import { writeFileSync } from 'node:fs';

const port = Number(process.argv[2] ?? 9334);
const sitePort = Number(process.argv[3] ?? 47654);
const outFile = process.argv[4] ?? 'tmp/browser-probe/e2e.json';
const extensionId = 'namdpafnjehflfkcgdbnmoccndohmjoj';
const extensionPath = process.argv[5] ?? `${process.cwd()}\\browser`;

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

const version = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
const browser = await connect(version.webSocketDebuggerUrl);
const loaded = await browser.send('Extensions.loadUnpacked', { path: extensionPath });
if (!loaded.result?.id) {
  console.error('extension load failed:', JSON.stringify(loaded));
  process.exit(2);
}

// Attach to the worker as soon as it exists; an attached worker stays alive.
let workerTarget = null;
for (let attempt = 0; attempt < 50 && !workerTarget; ++attempt) {
  const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
  workerTarget = targets.find((entry) => entry.type === 'service_worker'
    && entry.url.includes(extensionId)) ?? null;
  if (!workerTarget) await new Promise((resolve) => setTimeout(resolve, 200));
}
if (!workerTarget) {
  console.error('the extension worker never appeared');
  process.exit(3);
}
const worker = await connect(workerTarget.webSocketDebuggerUrl);
await worker.send('Runtime.enable');

const evaluate = async (expression) => {
  const answer = await worker.send('Runtime.evaluate',
    { expression, awaitPromise: true, returnByValue: true });
  if (answer.result?.exceptionDetails) {
    return { error: answer.result.exceptionDetails.exception?.description ?? 'exception' };
  }
  return answer.result?.result?.value;
};

await evaluate(`chrome.storage.local.set({'handoff.settings.v1':
  {enabled: true, origins: ['http://127.0.0.1:${sitePort}']}}).then(() => 'ok')`);
// Whether the worker registered its listeners at all decides where to look next.
const listenerCheck = await evaluate(`(async () => JSON.stringify({
  downloads: chrome.downloads.onCreated.hasListeners(),
  headers: chrome.webRequest.onHeadersReceived.hasListeners(),
  enabled: (await chrome.storage.local.get('handoff.settings.v1'))['handoff.settings.v1'].enabled,
}))()`);
console.log('listeners:', listenerCheck);
await new Promise((resolve) => setTimeout(resolve, 400));

const created = await fetch(`http://127.0.0.1:${port}/json/new?${encodeURIComponent(
  `http://127.0.0.1:${sitePort}/`)}`, { method: 'PUT' });
const page = await connect((await created.json()).webSocketDebuggerUrl);
let clickResult = 'no link';
for (let attempt = 0; attempt < 60; ++attempt) {
  const answer = await page.send('Runtime.evaluate', {
    expression: "document.readyState === 'complete' && document.querySelector('a') ? 'yes' : 'no'",
    returnByValue: true,
  });
  if (answer.result?.result?.value === 'yes') break;
  await new Promise((resolve) => setTimeout(resolve, 100));
}
const clicked = await page.send('Runtime.evaluate', {
  expression: "(() => { const link = document.querySelector('a');"
    + " if (!link) return 'no link'; link.click(); return 'clicked'; })()",
  returnByValue: true,
});
clickResult = clicked.result?.result?.value ?? 'no result';

let record = null;
let storage = {};
const deadline = Date.now() + 45000;
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

const recentExpression = "chrome.downloads.search({limit: 3}).then(list => JSON.stringify("
  + "list.map(item => ({id: item.id, state: item.state, paused: item.paused,"
  + " byExtensionId: item.byExtensionId ?? null, incognito: item.incognito,"
  + " url: item.url, bytes: item.bytesReceived}))))";
const recent = await evaluate(recentExpression);
const report = { clickResult, record, listenerCheck, recent, logs: worker.logs.slice(-20) };
writeFileSync(outFile, JSON.stringify(report, null, 2), 'utf8');
console.log(JSON.stringify(report, null, 2));

page.socket.close();
worker.socket.close();
browser.socket.close();
