// Rasterize the Figma-exported command glyphs into Windows .ico assets.
//
// The Figma MCP export is SVG. The Win32 toolbar and window icon need .ico
// bitmaps, so this project-local script flattens the small SVG subset used by
// the design (M/L/H/V/C/S/Q/T/Z paths, fill or stroke, optional rect mask) and
// emits 32-bpp ICO files at the sizes Windows asks for. No network access and
// no third-party packages are used.
//
// Usage: node scripts/build-win32-icons.mjs
import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const sourceDir = join(root, 'src', 'win32', 'assets', 'figma');
const outputDir = join(root, 'src', 'win32', 'assets', 'icons');
const sizes = [16, 20, 24, 32, 48];
const supersample = 4;

function attribute(text, name) {
    const match = text.match(new RegExp(`\\b${name}="([^"]*)"`));
    return match ? match[1] : null;
}

function parseColor(value, fallback) {
    if (!value) return fallback;
    if (value === 'black') return [0, 0, 0];
    if (value === 'white') return [255, 255, 255];
    const hex = value.replace('#', '');
    if (hex.length === 3) {
        return [0, 1, 2].map((i) => parseInt(hex[i] + hex[i], 16));
    }
    if (hex.length === 6) {
        return [0, 2, 4].map((i) => parseInt(hex.slice(i, i + 2), 16));
    }
    throw new Error(`unsupported color ${value}`);
}

// Flatten the small SVG path subset into one or more polylines.
function flattenPath(d, toleranceSteps = 20) {
    const tokens = d.match(/[a-zA-Z]|-?(?:\d+\.?\d*|\.\d+)(?:e[-+]?\d+)?/g) ?? [];
    const subpaths = [];
    let current = null;
    let index = 0;
    let command = null;
    let x = 0;
    let y = 0;
    let startX = 0;
    let startY = 0;
    let lastControlX = 0;
    let lastControlY = 0;
    let lastCommand = null;

    const number = () => {
        if (index >= tokens.length || /[a-zA-Z]/.test(tokens[index])) {
            throw new Error(`malformed path near token ${index}: ${d.slice(0, 60)}`);
        }
        return Number(tokens[index++]);
    };
    const addPoint = (px, py) => {
        if (!current) {
            current = [[px, py]];
            subpaths.push(current);
        } else {
            current.push([px, py]);
        }
        x = px;
        y = py;
    };
    const addCubic = (x1, y1, x2, y2, x3, y3) => {
        const x0 = x;
        const y0 = y;
        for (let step = 1; step <= toleranceSteps; step += 1) {
            const t = step / toleranceSteps;
            const mt = 1 - t;
            const px = mt ** 3 * x0 + 3 * mt * mt * t * x1 + 3 * mt * t * t * x2 + t ** 3 * x3;
            const py = mt ** 3 * y0 + 3 * mt * mt * t * y1 + 3 * mt * t * t * y2 + t ** 3 * y3;
            addPoint(px, py);
        }
    };
    const addQuadratic = (x1, y1, x2, y2) => {
        const x0 = x;
        const y0 = y;
        for (let step = 1; step <= toleranceSteps; step += 1) {
            const t = step / toleranceSteps;
            const mt = 1 - t;
            const px = mt * mt * x0 + 2 * mt * t * x1 + t * t * x2;
            const py = mt * mt * y0 + 2 * mt * t * y1 + t * t * y2;
            addPoint(px, py);
        }
    };

    while (index < tokens.length) {
        if (/[a-zA-Z]/.test(tokens[index])) command = tokens[index++];
        const relative = command === command.toLowerCase();
        const upper = command.toUpperCase();
        if (upper === 'Z') {
            if (current) current.push([startX, startY]);
            x = startX;
            y = startY;
            current = null;
            lastCommand = 'Z';
            continue;
        }
        if (upper === 'M') {
            const px = number() + (relative ? x : 0);
            const py = number() + (relative ? y : 0);
            startX = px;
            startY = py;
            current = null;
            addPoint(px, py);
            command = relative ? 'l' : 'L';
            lastCommand = 'M';
            continue;
        }
        if (upper === 'L') {
            addPoint(number() + (relative ? x : 0), number() + (relative ? y : 0));
            lastCommand = 'L';
            continue;
        }
        if (upper === 'H') {
            addPoint(number() + (relative ? x : 0), y);
            lastCommand = 'H';
            continue;
        }
        if (upper === 'V') {
            addPoint(x, number() + (relative ? y : 0));
            lastCommand = 'V';
            continue;
        }
        if (upper === 'C') {
            const x1 = number() + (relative ? x : 0);
            const y1 = number() + (relative ? y : 0);
            const x2 = number() + (relative ? x : 0);
            const y2 = number() + (relative ? y : 0);
            const x3 = number() + (relative ? x : 0);
            const y3 = number() + (relative ? y : 0);
            addCubic(x1, y1, x2, y2, x3, y3);
            lastControlX = x2;
            lastControlY = y2;
            lastCommand = 'C';
            continue;
        }
        if (upper === 'S') {
            const reflectedX = lastCommand === 'C' || lastCommand === 'S' ? 2 * x - lastControlX : x;
            const reflectedY = lastCommand === 'C' || lastCommand === 'S' ? 2 * y - lastControlY : y;
            const x2 = number() + (relative ? x : 0);
            const y2 = number() + (relative ? y : 0);
            const x3 = number() + (relative ? x : 0);
            const y3 = number() + (relative ? y : 0);
            addCubic(reflectedX, reflectedY, x2, y2, x3, y3);
            lastControlX = x2;
            lastControlY = y2;
            lastCommand = 'S';
            continue;
        }
        if (upper === 'Q') {
            const x1 = number() + (relative ? x : 0);
            const y1 = number() + (relative ? y : 0);
            const x2 = number() + (relative ? x : 0);
            const y2 = number() + (relative ? y : 0);
            addQuadratic(x1, y1, x2, y2);
            lastControlX = x1;
            lastControlY = y1;
            lastCommand = 'Q';
            continue;
        }
        if (upper === 'T') {
            const reflectedX = lastCommand === 'Q' || lastCommand === 'T' ? 2 * x - lastControlX : x;
            const reflectedY = lastCommand === 'Q' || lastCommand === 'T' ? 2 * y - lastControlY : y;
            const x2 = number() + (relative ? x : 0);
            const y2 = number() + (relative ? y : 0);
            addQuadratic(reflectedX, reflectedY, x2, y2);
            lastControlX = reflectedX;
            lastControlY = reflectedY;
            lastCommand = 'T';
            continue;
        }
        throw new Error(`unsupported path command ${command}: ${d.slice(0, 60)}`);
    }
    return subpaths.filter((points) => points.length >= 2);
}

function pointInSubpaths(subpaths, px, py, evenOdd) {
    let winding = 0;
    let crossings = 0;
    for (const points of subpaths) {
        for (let i = 0; i < points.length; i += 1) {
            const [x0, y0] = points[i];
            const [x1, y1] = points[(i + 1) % points.length];
            if ((y0 <= py && y1 > py) || (y1 <= py && y0 > py)) {
                const t = (py - y0) / (y1 - y0);
                if (x0 + t * (x1 - x0) > px) {
                    crossings += 1;
                    winding += y1 > y0 ? 1 : -1;
                }
            }
        }
    }
    return evenOdd ? crossings % 2 === 1 : winding !== 0;
}

function distanceToSegment(px, py, ax, ay, bx, by) {
    const dx = bx - ax;
    const dy = by - ay;
    const lengthSquared = dx * dx + dy * dy;
    let t = lengthSquared === 0 ? 0 : ((px - ax) * dx + (py - ay) * dy) / lengthSquared;
    t = Math.max(0, Math.min(1, t));
    const sx = ax + t * dx;
    const sy = ay + t * dy;
    return Math.hypot(px - sx, py - sy);
}

function distanceToOpenPolyline(points, px, py) {
    let best = Infinity;
    for (let i = 0; i + 1 < points.length; i += 1) {
        const [ax, ay] = points[i];
        const [bx, by] = points[i + 1];
        const distance = distanceToSegment(px, py, ax, ay, bx, by);
        if (distance < best) best = distance;
    }
    return best;
}

function parseSvg(text) {
    const width = Number(attribute(text, 'width'));
    const height = Number(attribute(text, 'height'));
    const masks = new Map();
    const maskRegex = /<mask\b[^>]*\bid="([^"]+)"[^>]*>([\s\S]*?)<\/mask>/g;
    for (let match = maskRegex.exec(text); match; match = maskRegex.exec(text)) {
        const maskPath = match[2].match(/<path\b([^>]*)\/?>/);
        if (maskPath) masks.set(match[1], flattenPath(attribute(maskPath[1], 'd')));
    }
    const paths = [];
    // Mask definitions are clipping geometry, not painted paths; scan only the
    // document body outside <mask> blocks.
    const painted = text.replace(/<mask\b[\s\S]*?<\/mask>/g, '');
    const pathRegex = /<path\b([^>]*?)\/?>/g;
    for (let match = pathRegex.exec(painted); match; match = pathRegex.exec(painted)) {
        const attrs = match[1];
        const maskReference = attribute(attrs, 'mask');
        const maskId = maskReference ? maskReference.match(/#([^)]+)/)?.[1] : null;
        const d = attribute(attrs, 'd');
        const stroke = attribute(attrs, 'stroke');
        const fill = attribute(attrs, 'fill');
        if (!stroke && (!fill || fill === 'none')) continue;
        const geometry = flattenPath(d);
        paths.push({
            geometry,
            mask: maskId ? masks.get(maskId) ?? null : null,
            stroke: stroke ? parseColor(stroke, [0, 0, 0]) : null,
            strokeWidth: Number(attribute(attrs, 'stroke-width') ?? '1'),
            lineCap: attribute(attrs, 'stroke-linecap') ?? 'butt',
            fill: !stroke && fill && fill !== 'none' ? parseColor(fill, [0, 0, 0]) : null,
            // Figma exported these glyphs by converting strokes to filled outlines.
            // The viewbox border contour is self-intersecting, so the even-odd rule
            // keeps it a 1px border instead of filling the whole canvas.
            evenOdd: attribute(attrs, 'fill-rule') !== 'nonzero',
        });
    }
    if (!paths.length) throw new Error(`no paths found in ${file}`);
    return { width, height, paths };
}

function render(svg, size) {
    const pixels = new Uint8ClampedArray(size * size * 4); // BGRA
    const scale = size / Math.max(svg.width, svg.height);
    const offsetX = (size - svg.width * scale) / 2;
    const offsetY = (size - svg.height * scale) / 2;
    const samples = supersample * supersample;
    for (const path of svg.paths) {
        const geometry = path.geometry.map((points) =>
            points.map(([px, py]) => [px * scale + offsetX, py * scale + offsetY]),
        );
        const mask = path.mask
            ? path.mask.map((points) => points.map(([px, py]) => [px * scale + offsetX, py * scale + offsetY]))
            : null;
        const color = path.fill ?? path.stroke ?? [0, 0, 0];
        const stroked = Boolean(path.stroke);
        const halfWidth = Math.max(0.35, (path.strokeWidth * scale) / 2);
        for (let y = 0; y < size; y += 1) {
            for (let x = 0; x < size; x += 1) {
                let covered = 0;
                for (let sy = 0; sy < supersample; sy += 1) {
                    for (let sx = 0; sx < supersample; sx += 1) {
                        const px = x + (sx + 0.5) / supersample;
                        const py = y + (sy + 0.5) / supersample;
                        if (mask && !pointInSubpaths(mask, px, py, false)) continue;
                        const inside = stroked
                            ? geometry.reduce((best, points) => Math.min(best, distanceToOpenPolyline(points, px, py)), Infinity) <= halfWidth
                            : pointInSubpaths(geometry, px, py, path.evenOdd);
                        if (inside) covered += 1;
                    }
                }
                if (!covered) continue;
                const alpha = (covered / samples) * 255;
                const offset = (y * size + x) * 4;
                const previous = pixels[offset + 3] / 255;
                const next = alpha / 255 + previous * (1 - alpha / 255);
                if (next <= 0) continue;
                for (let channel = 0; channel < 3; channel += 1) {
                    const existing = pixels[offset + channel];
                    const blended = (color[2 - channel] * (alpha / 255) + existing * previous * (1 - alpha / 255)) / next;
                    pixels[offset + channel] = Math.round(blended);
                }
                pixels[offset + 3] = Math.round(next * 255);
            }
        }
    }
    return pixels;
}

function buildBmpPayload(imageSize, bitmap) {
    const maskRowBytes = Math.ceil(imageSize / 32) * 4;
    const xorSize = imageSize * imageSize * 4;
    const andSize = maskRowBytes * imageSize;
    const headerSize = 40;
    const body = Buffer.alloc(headerSize + xorSize + andSize);
    body.writeUInt32LE(40, 0);
    body.writeInt32LE(imageSize, 4);
    body.writeInt32LE(imageSize * 2, 8);
    body.writeUInt16LE(1, 12);
    body.writeUInt16LE(32, 14);
    body.writeUInt32LE(0, 16);
    body.writeUInt32LE(xorSize + andSize, 20);
    for (let y = 0; y < imageSize; y += 1) {
        const sourceRow = imageSize - 1 - y;
        for (let x = 0; x < imageSize; x += 1) {
            const source = (sourceRow * imageSize + x) * 4;
            const target = headerSize + (y * imageSize + x) * 4;
            body[target] = bitmap[source];
            body[target + 1] = bitmap[source + 1];
            body[target + 2] = bitmap[source + 2];
            body[target + 3] = bitmap[source + 3];
        }
    }
    const maskBase = headerSize + xorSize;
    for (let y = 0; y < imageSize; y += 1) {
        const sourceRow = imageSize - 1 - y;
        for (let x = 0; x < imageSize; x += 1) {
            const alpha = bitmap[(sourceRow * imageSize + x) * 4 + 3];
            if (alpha < 128) {
                const offset = maskBase + y * maskRowBytes + (x >> 3);
                body[offset] |= 0x80 >> (x & 7);
            }
        }
    }
    return body;
}

if (!existsSync(outputDir)) mkdirSync(outputDir, { recursive: true });
const outputs = [
    { name: 'app' },
    { name: 'new' },
    { name: 'start' },
    // Figma captured 开始/继续 in its disabled (gray) state; the toolbar uses a
    // black variant when the command becomes available.
    { name: 'start-active', source: 'start', strokeOverride: 'black' },
    { name: 'pause' },
    { name: 'stop' },
    { name: 'delete' },
    { name: 'folder' },
    { name: 'settings' },
];
for (const spec of outputs) {
    const source = spec.source ?? spec.name;
    let text = readFileSync(join(sourceDir, `${source}.svg`), 'utf8');
    if (spec.strokeOverride) {
        text = text.replace(/#6[dD]6[dD]6[dD]/g, spec.strokeOverride);
    }
    const svg = parseSvg(text);
    const entries = sizes.map((size) => {
        const pixels = render(svg, size);
        const payload = buildBmpPayload(size, pixels);
        return { size, payload };
    });
    const total = 6 + entries.length * 16 + entries.reduce((sum, entry) => sum + entry.payload.length, 0);
    const out = Buffer.alloc(total);
    out.writeUInt16LE(0, 0);
    out.writeUInt16LE(1, 2);
    out.writeUInt16LE(entries.length, 4);
    let cursor = 6 + entries.length * 16;
    entries.forEach((entry, index) => {
        const base = 6 + index * 16;
        out.writeUInt8(entry.size >= 256 ? 0 : entry.size, base);
        out.writeUInt8(entry.size >= 256 ? 0 : entry.size, base + 1);
        out.writeUInt8(0, base + 2);
        out.writeUInt8(0, base + 3);
        out.writeUInt16LE(1, base + 4);
        out.writeUInt16LE(32, base + 6);
        out.writeUInt32LE(entry.payload.length, base + 8);
        out.writeUInt32LE(cursor, base + 12);
        entry.payload.copy(out, cursor);
        cursor += entry.payload.length;
    });
    writeFileSync(join(outputDir, `${spec.name}.ico`), out);
    console.log(`wrote ${spec.name}.ico (${sizes.join(', ')})`);
}
