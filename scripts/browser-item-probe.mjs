// Prints what the browser hands over at download creation, which is the input the
// matcher judges. One run should settle whether the item is paused, already
// finished, or eligible.
import { writeFileSync } from 'node:fs';

const port = Number(process.argv[2] ?? 9334);
const sitePort = Number(process.argv[3] ?? 47654);
const outFile = process.argv[4] ?? 'tmp/browser-probe/item.json';
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

const popup = await openTarget(`chrome-extension://${extensionId}/popup.html`);
const ext = await connect(popup.webSocketDebuggerUrl);
await ext.send('Runtime.enable');

const evaluate = async (expression) => {
  const answer = await ext.send('Runtime.evaluate',
    { expression, awaitPromise: true, returnByValue: true });
  if (answer.result?.exceptionDetails) {
    return { error: answer.result.exceptionDetails.exception?.description ?? 'exception' };
  }
  return answer.result?.result?.value;
};

await evaluate(`(async () => {
  await chrome.storage.local.set({'handoff.settings.v1':
    {enabled: true, origins: ['http://127.0.0.1:${sitePort}']}});
  const module = await import(chrome.runtime.getURL('worker.mjs'));
  globalThis.__handoff = module.installWorker(chrome);
  await globalThis.__handoff.ready;
  chrome.downloads.onCreated.addListener((item) => {
    console.log('ITEM', JSON.stringify({id: item.id, state: item.state, paused: item.paused,
      byExtensionId: item.byExtensionId ?? null, incognito: item.incognito,
      startTime: item.startTime, url: item.url}));
  });
  return 'ready';
})()`);

const site = await openTarget(`http://127.0.0.1:${sitePort}/`);
const page = await connect(site.webSocketDebuggerUrl);
// Wait for the page to have its link before clicking; a click on a blank page is
// indistinguishable from a matcher that refused.
let clickResult = 'no link';
for (let attempt = 0; attempt < 40; ++attempt) {
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
console.log('click:', clickResult);

await new Promise((resolve) => setTimeout(resolve, 15000));
const storage = await evaluate('chrome.storage.local.get(null)');
const records = await evaluate("JSON.stringify([...globalThis.__handoff.observer.requests"
  + ".entries()].map(([id, r]) => ({id, url: r.firstUrl, invalid: r.invalid, hops: r.hops})))");
const report = { clickResult, logs: ext.logs, observerRecords: records,
  journalKeys: Object.keys(storage ?? {}).filter((key) => key.startsWith('handoff.journal.')) };
writeFileSync(outFile, JSON.stringify(report, null, 2), 'utf8');
console.log(JSON.stringify(report, null, 2));

page.socket.close();
ext.socket.close();
