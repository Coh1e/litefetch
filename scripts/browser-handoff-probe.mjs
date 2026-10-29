// End-to-end browser handoff: enable the extension for a local test site, click
// a real link on a real page, and read back what the extension's journal says.
// The extension's own popup page is the evaluator, because an idle MV3 service
// worker has no attached target.
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

const popup = await openTarget(`chrome-extension://${extensionId}/popup.html`);
const ext = await connect(popup.webSocketDebuggerUrl);
const evaluateExt = async (expression) => {
  const answer = await ext.send('Runtime.evaluate',
    { expression, awaitPromise: true, returnByValue: true });
  if (answer.result?.exceptionDetails) throw new Error(JSON.stringify(answer.result.exceptionDetails));
  return answer.result?.result?.value;
};

await evaluateExt(`chrome.storage.local.set({'handoff.settings.v1':
  {enabled: true, origins: ['http://127.0.0.1:${sitePort}']}}).then(() => 'ok')`);
await new Promise((resolve) => setTimeout(resolve, 500));

const site = await openTarget(`http://127.0.0.1:${sitePort}/`);
const page = await connect(site.webSocketDebuggerUrl);
await new Promise((resolve) => setTimeout(resolve, 800));
const clicked = await page.send('Runtime.evaluate', {
  expression: `(() => { const link = document.querySelector('a');
    if (!link) return 'no link'; link.click(); return 'clicked'; })()`,
  returnByValue: true,
});
if (clicked.result?.result?.value !== 'clicked') {
  console.error('the link was not clicked', JSON.stringify(clicked.result));
}

let storage = {};
let record = null;
const deadline = Date.now() + 30000;
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
writeFileSync(outFile, JSON.stringify({ record, storage, downloads }, null, 2), 'utf8');
console.log(JSON.stringify({ record, downloads: downloads?.map((item) => ({
  id: item.id, state: item.state, error: item.error, url: item.url })) }, null, 2));
page.socket.close();
ext.socket.close();
