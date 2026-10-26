// Controlled HTTP peer for manual core-boundary verification.
// Usage: node scripts/core-http-fixture.mjs <port> [size]
// Serves deterministic bytes on every path except /missing (404) and prints
// one JSON line with the bound port and SHA-256 of the body.
import { createHash } from 'node:crypto';
import { createServer } from 'node:http';

const port = Number(process.argv[2] ?? '0');
const size = Number(process.argv[3] ?? '262144');
const body = Buffer.alloc(size);
for (let i = 0; i < size; i += 1) body[i] = (i * 37 + 11) & 0xff;

const server = createServer((request, response) => {
    if (request.url === '/missing') {
        response.writeHead(404, { 'Content-Length': '0' });
        response.end();
        return;
    }
    response.writeHead(200, {
        'Content-Type': 'application/octet-stream',
        'Content-Length': String(body.length),
    });
    response.end(body);
});

server.listen(port, '127.0.0.1', () => {
    console.log(JSON.stringify({
        port: server.address().port,
        size: body.length,
        sha256: createHash('sha256').update(body).digest('hex'),
    }));
});

for (const signal of ['SIGINT', 'SIGTERM']) {
    process.on(signal, () => server.close(() => process.exit(0)));
}
