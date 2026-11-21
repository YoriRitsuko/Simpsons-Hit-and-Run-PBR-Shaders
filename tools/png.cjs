const fs = require("node:fs");
const zlib = require("node:zlib");

function paeth(a, b, c) {
    const p = a + b - c;
    const da = Math.abs(p - a), db = Math.abs(p - b), dc = Math.abs(p - c);
    return da <= db && da <= dc ? a : db <= dc ? b : c;
}

function readPng(filename) {
    const src = fs.readFileSync(filename);
    const width = src.readUInt32BE(16), height = src.readUInt32BE(20);
    const components = { 0: 1, 2: 3, 6: 4 }[src[25]];
    if (src.toString("ascii", 12, 16) !== "IHDR" || src[24] !== 8 || src[28] ||
        !components || !width || !height || width > 4096 || height > 4096)
        throw new Error("Expected an 8-bit grayscale, RGB or RGBA PNG: " + filename);
    const pieces = [];
    for (let cursor = 8; cursor + 12 <= src.length;) {
        const size = src.readUInt32BE(cursor);
        if (size > src.length - cursor - 12) throw new Error("Truncated PNG: " + filename);
        if (src.toString("ascii", cursor + 4, cursor + 8) === "IDAT")
            pieces.push(src.subarray(cursor + 8, cursor + 8 + size));
        cursor += 12 + size;
    }
    const stride = width * components;
    const scan = zlib.inflateSync(Buffer.concat(pieces), { maxOutputLength: (stride + 1) * height });
    if (scan.length !== (stride + 1) * height) throw new Error("Bad PNG scanlines: " + filename);
    const unfiltered = Buffer.alloc(stride * height);
    for (let y = 0; y < height; y++) {
        const filter = scan[y * (stride + 1)];
        if (filter > 4) throw new Error("Bad PNG filter: " + filename);
        for (let x = 0; x < stride; x++) {
            const offset = y * stride + x;
            const a = x >= components ? unfiltered[offset - components] : 0;
            const b = y ? unfiltered[offset - stride] : 0;
            const c = y && x >= components ? unfiltered[offset - stride - components] : 0;
            const prediction = [0, a, b, (a + b) >> 1, paeth(a, b, c)][filter];
            unfiltered[offset] = scan[y * (stride + 1) + 1 + x] + prediction;
        }
    }
    const pixels = Buffer.alloc(width * height * 4, 255);
    for (let pixel = 0; pixel < width * height; pixel++) {
        for (let c = 0; c < 3; c++)
            pixels[4 * pixel + c] = unfiltered[pixel * components + (components === 1 ? 0 : c)];
        if (components === 4) pixels[4 * pixel + 3] = unfiltered[4 * pixel + 3];
    }
    return { width, height, pixels };
}

module.exports = { readPng };
