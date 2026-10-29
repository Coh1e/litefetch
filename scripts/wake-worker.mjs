// Wakes the extension's MV3 service worker over the browser endpoint and prints
// what the DevTools protocol says, so a silent refusal is visible.
const port = Number(process.argv[2] ?? 9334);
const extensionId = 'namdpafnjehflfkcgdbnmoccndohmjoj';

const version = await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
const socket = new WebSocket(version.webSocketDebuggerUrl);
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

console.log('enable:', JSON.stringify(await send('ServiceWorker.enable')));
console.log('start:', JSON.stringify(await send('ServiceWorker.startWorker',
  { scopeURL: `chrome-extension://${extensionId}/` })));
await new Promise((resolve) => setTimeout(resolve, 1500));
console.log('targets:', JSON.stringify(await send('Target.getTargets')));
socket.close();
