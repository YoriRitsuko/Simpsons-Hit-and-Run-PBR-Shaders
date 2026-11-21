const fs = require("node:fs");

function readRadiance(filename) {
    const data = fs.readFileSync(filename);
    const headerEnd = data.indexOf("\n\n");
    const sizeEnd = data.indexOf("\n", headerEnd + 2);
    const size = /^-Y (\d+) \+X (\d+)$/.exec(data.toString("ascii", headerEnd + 2, sizeEnd));
    if (!size || !data.subarray(0, headerEnd).includes("FORMAT=32-bit_rle_rgbe"))
        throw new Error("Expected a Radiance RGBE image in -Y +X order");
    const [, height, width] = size.map(Number);
    if (width !== height || width < 8 || width > 1024 || (width & (width - 1)))
        throw new Error("The mirror-ball image must be square and power of two");

    const rgb = new Float32Array(width * height * 3);
    const row = new Uint8Array(width * 4);
    let offset = sizeEnd + 1;
    for (let y = 0; y < height; y++) {
        if (data[offset] !== 2 || data[offset + 1] !== 2 || data.readUInt16BE(offset + 2) !== width)
            throw new Error("Expected RGBE scanline compression");
        offset += 4;
        for (let channel = 0; channel < 4; channel++) {
            for (let x = 0; x < width;) {
                const code = data[offset++];
                const count = code > 128 ? code - 128 : code;
                if (!count || x + count > width) throw new Error("Bad RGBE run");
                if (code > 128) {
                    if (offset >= data.length) throw new Error("Truncated RGBE run");
                    row.fill(data[offset++], channel * width + x, channel * width + x + count);
                } else {
                    if (offset + count > data.length) throw new Error("Truncated RGBE scanline");
                    row.set(data.subarray(offset, offset + count), channel * width + x);
                    offset += count;
                }
                x += count;
            }
        }
        for (let x = 0; x < width; x++) {
            const exponent = row[3 * width + x];
            const scale = exponent ? 2 ** (exponent - 136) : 0;
            for (let c = 0; c < 3; c++) rgb[3 * (y * width + x) + c] = row[c * width + x] * scale;
        }
    }
    return { width, rgb };
}

function half(value) {
    if (value < 2 ** -14) return Math.round(value * 2 ** 24);
    const exponent = Math.floor(Math.log2(Math.min(value, 65504)));
    return Math.min(0x7bff, ((exponent + 15) << 10) +
        Math.round((value / 2 ** exponent - 1) * 1024));
}

function dot(a, b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

function unit(v) {
    const length = Math.hypot(...v);
    return v.map(x => x / length);
}

function ballRay(u, v) {
    let x = 2 * u - 1;
    let y = 1 - 2 * v;
    const radius = Math.hypot(x, y);
    if (radius > 0.999) {
        x *= 0.999 / radius;
        y *= 0.999 / radius;
    }
    const z = Math.sqrt(1 - x * x - y * y);
    return [2 * x * z, 2 * y * z, 1 - 2 * z * z];
}

function addSample(target, src, ray, weight) {
    const { width, rgb } = src;
    const scale = 0.5 / Math.max(Math.hypot(ray[0], ray[1], ray[2] - 1), 0.000001);
    const x = Math.max(0, Math.min(width - 1, (0.5 + scale * ray[0]) * width - 0.5));
    const y = Math.max(0, Math.min(width - 1, (0.5 - scale * ray[1]) * width - 0.5));
    const ix = Math.floor(x), iy = Math.floor(y);
    const jx = Math.min(ix + 1, width - 1), jy = Math.min(iy + 1, width - 1);
    const dx = x - ix, dy = y - iy;
    for (let c = 0; c < 3; c++) {
        const top = rgb[(iy * width + ix) * 3 + c] * (1 - dx) + rgb[(iy * width + jx) * 3 + c] * dx;
        const bottom = rgb[(jy * width + ix) * 3 + c] * (1 - dx) + rgb[(jy * width + jx) * 3 + c] * dx;
        target[c] += (top * (1 - dy) + bottom * dy) * weight;
    }
}

function radicalInverse(index) {
    let value = 0;
    for (let place = 0.5; index; place *= 0.5, index >>>= 1) value += (index & 1) * place;
    return value;
}

function ggxDirections(roughness) {
    const count = 96;
    const alpha = roughness ** 2;
    return Array.from({ length: count }, (_, index) => {
        const phi = index * (2 * Math.PI / count);
        const pick = radicalInverse(index);
        const cosTheta = Math.sqrt((1 - pick) / (1 + (alpha * alpha - 1) * pick));
        const sinTheta = Math.sqrt(1 - cosTheta * cosTheta);
        return [2 * sinTheta * cosTheta * Math.cos(phi),
                2 * sinTheta * cosTheta * Math.sin(phi), 2 * cosTheta * cosTheta - 1];
    }).filter(ray => ray[2] > 0);
}

function makeMip(source, size, roughness) {
    const bytes = Buffer.alloc(size * size * 8);
    const directions = roughness ? ggxDirections(roughness) : [];
    let write = 0;
    for (let y = 0; y < size; y++) {
        for (let x = 0; x < size; x++) {
            const ray = ballRay((x + 0.5) / size, (y + 0.5) / size);
            const sum = [0, 0, 0];
            let weight = 1;
            if (!roughness) addSample(sum, source, ray, 1);
            else {
                const tangent = unit(Math.abs(ray[1]) < 0.999 ? [ray[2], 0, -ray[0]] : [0, -ray[2], ray[1]]);
                const bitangent = [ray[1]*tangent[2] - ray[2]*tangent[1],
                    ray[2]*tangent[0] - ray[0]*tangent[2], ray[0]*tangent[1] - ray[1]*tangent[0]];
                weight = 0;
                for (const local of directions) {
                    const direction = ray.map((value, axis) =>
                        tangent[axis] * local[0] + bitangent[axis] * local[1] + value * local[2]);
                    const cosine = dot(ray, direction);
                    addSample(sum, source, direction, cosine);
                    weight += cosine;
                }
            }
            for (const channel of sum) {
                bytes.writeUInt16LE(half(channel / weight), write);
                write += 2;
            }
            bytes.writeUInt16LE(0x3c00, write);
            write += 2;
        }
    }
    return bytes;
}

function prefilterEnvironment(input, output) {
    const source = readRadiance(input);
    const levels = Math.log2(source.width) + 1;
    const header = Buffer.alloc(128);
    header.write("DDS ");
    for (const [offset, value] of [[4, 124], [8, 0x2100f], [12, source.width],
        [16, source.width], [20, source.width * 8], [28, levels], [76, 32], [80, 4],
        [84, 113], [108, 0x401008]]) header.writeUInt32LE(value, offset);
    const mips = [header];
    for (let level = 0; level < levels; level++) {
        mips.push(makeMip(source, source.width >> level, level / (levels - 1)));
    }
    fs.writeFileSync(output, Buffer.concat(mips));
}

module.exports = { prefilterEnvironment };
