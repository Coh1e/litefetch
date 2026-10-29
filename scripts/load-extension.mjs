// Loads the unpacked extension through the browser endpoint and prints its id.
import { resolve } from 'node:path';

const port = Number(process.argv[2] ?? 9334);
const extensionPath = resolve(process.argv[3] ?? 'browser');

const version = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
const socket = new WebSocket(version.webSocketDebuggerUrl);
let nextId = 1;
const pending = new Map();
socket.addEventListener('message', (event) => {
  const message = JSON.parse(event.data);
  if (message.id && pending.has(message.id)) {
    pending.get(message.id)(message);
    pending.delete(message.id);
  }
});
await new Promise((resolve2, reject) => {
  socket.addEventListener('open', resolve2, { once: true });
  socket.addEventListener('error', reject, { once: true });
});
const send = (method, params) => {
  const id = nextId++;
  return new Promise((resolve2) => {
    pending.set(id, resolve2);
    socket.send(JSON.stringify({ id, method, params }));
  });
};
const answer = await send('Extensions.loadUnpacked', { path: extensionPath });
console.log(JSON.stringify(answer));
socket.close();
