// Drives the loaded extension's service worker over the DevTools protocol and
// asks it to call the native messaging host, which is the half of the handoff a
// test cannot reach without a real browser.
import { writeFileSync } from 'node:fs';

const port = Number(process.argv[2] ?? 9334);
const outFile = process.argv[3] ?? 'tmp/browser-probe/sw-answer.json';

const listResponse = await fetch(`http://127.0.0.1:${port}/json/list`);
const targets = await listResponse.json();
const target = targets.find((entry) => entry.type === 'service_worker'
  && entry.url.includes('namdpafnjehflfkcgdbnmoccndohmjoj'));
if (!target) {
  console.error('extension service worker not found');
  process.exit(2);
}

const socket = new WebSocket(target.webSocketDebuggerUrl);
let nextId = 1;
const pending = new Map();

socket.addEventListener('message', (event) => {
  const message = JSON.parse(event.data);
  if (message.id && pending.has(message.id)) {
    pending.get(message.id)(message);
    pending.delete(message.id);
  }
});

function send(method, params) {
  const id = nextId++;
  return new Promise((resolve) => {
    pending.set(id, resolve);
    socket.send(JSON.stringify({ id, method, params }));
  });
}

await new Promise((resolve, reject) => {
  socket.addEventListener('open', resolve, { once: true });
  socket.addEventListener('error', reject, { once: true });
});

const expression = `
new Promise((resolve) => {
  let done = false;
  const finish = (value) => { if (!done) { done = true; resolve(value); } };
  try {
    const port = chrome.runtime.connectNative('org.native_downloader.bridge');
    port.onMessage.addListener((message) => finish({kind: 'message', message}));
    port.onDisconnect.addListener(() => finish({kind: 'disconnect',
      error: chrome.runtime.lastError ? chrome.runtime.lastError.message : 'no error'}));
    port.postMessage({version: 1, id: 'chrome-probe-1', method: 'prepare',
      requestId: 'chrome-probe-1',
      payload: {url: 'http://127.0.0.1:9/chrome.bin', method: 'GET',
        uniqueMatch: true, replaySafe: true}});
    setTimeout(() => finish({kind: 'timeout'}), 8000);
  } catch (error) {
    finish({kind: 'threw', error: String(error)});
  }
});`;

const answer = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
const value = answer.result?.result?.value ?? answer.result?.exceptionDetails ?? answer;
writeFileSync(outFile, JSON.stringify(value, null, 2), 'utf8');
console.log(JSON.stringify(value));
socket.close();
