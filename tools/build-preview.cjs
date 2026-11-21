const fs = require("node:fs");
const path = require("node:path");
const zlib = require("node:zlib");
const { prefilterEnvironment } = require("./prefilter-environment.cjs");

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

function writeTga(filename, { width, height, pixels }) {
    const header = Buffer.alloc(18);
    header[2] = 2;
    header.writeUInt16LE(width, 12);
    header.writeUInt16LE(height, 14);
    header[16] = 32;
    header[17] = 0x28;
    const bgr = Buffer.from(pixels);
    for (let i = 0; i < bgr.length; i += 4) {
        bgr[i] = pixels[i + 2];
        bgr[i + 2] = pixels[i];
    }
    fs.writeFileSync(filename, Buffer.concat([header, bgr]));
}

function packTextures(input, output, material, prefix) {
    const texture = map => path.join(input, "MaterialPreviewMesh_" + material + "_" + map + ".png");
    for (const [map, suffix] of [["BaseColor", "base"], ["Normal", "normal"], ["Emissive", "emission"]]) {
        writeTga(path.join(output, prefix + "-" + suffix + ".tga"), readPng(texture(map)));
    }
    const rough = readPng(texture("Roughness"));
    const metal = readPng(texture("Metallic"));
    const aoFile = texture("AmbientOcclusion");
    const ao = fs.existsSync(aoFile) ? readPng(aoFile) : null;
    for (const map of [metal, ao]) {
        if (map && (map.width !== rough.width || map.height !== rough.height))
            throw new Error("ORM source maps must have matching sizes");
    }
    const pixels = Buffer.alloc(rough.pixels.length, 255);
    for (let i = 0; i < pixels.length; i += 4) {
        pixels[i] = ao ? ao.pixels[i] : 255;
        pixels[i + 1] = rough.pixels[i];
        pixels[i + 2] = metal.pixels[i];
    }
    writeTga(path.join(output, prefix + "-orm.tga"), { width: rough.width, height: rough.height, pixels });
}

function uints(...values) {
    const result = Buffer.alloc(values.length * 4);
    values.forEach((value, index) => result.writeUInt32LE(value, 4 * index));
    return result;
}

function floats(values) {
    const result = Buffer.alloc(values.length * 4);
    values.forEach((value, index) => result.writeFloatLE(value, 4 * index));
    return result;
}

function pstring(value) {
    const raw = Buffer.from(value, "ascii");
    const length = Math.ceil(raw.length / 4) * 4;
    const data = Buffer.alloc(1 + length);
    data[0] = length;
    raw.copy(data, 1);
    return data;
}

function chunk(id, data, children = []) {
    const header = data.length + 12;
    return Buffer.concat([uints(id, header, header + children.reduce((size, child) => size + child.length, 0)),
        data, ...children]);
}

function shader(name, translucent) {
    const texture = chunk(0x11002, Buffer.concat([Buffer.from("TEX\0"), pstring("PBRPreviewWhite.png")]));
    const params = [["LIT\0", 1], ["BLMD", translucent ? 1 : 0], ["2SID", 0], ["ATST", 0]]
        .map(([key, value]) => chunk(0x11003, Buffer.concat([Buffer.from(key), uints(value)])));
    return chunk(0x11000, Buffer.concat([pstring(name), uints(0), pstring("simple"),
        uints(Number(translucent), 0x11, 0xffffffff, params.length + 1)]), [texture, ...params]);
}

function whiteTexture() {
    // A one-pixel stock fallback keeps UV0 in the game's draw stream.
    const pngChunk = (type, data) => {
        const body = Buffer.concat([Buffer.from(type), data]);
        const length = Buffer.alloc(4), crc = Buffer.alloc(4);
        length.writeUInt32BE(data.length);
        crc.writeUInt32BE(zlib.crc32(body));
        return Buffer.concat([length, body, crc]);
    };
    const header = Buffer.from([0,0,0,1, 0,0,0,1, 8,2,0,0,0]);
    const img = Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]), pngChunk("IHDR", header),
        pngChunk("IDAT", zlib.deflateSync(Buffer.from([0,255,255,255]))), pngChunk("IEND", Buffer.alloc(0))]);
    const name = pstring("PBRPreviewWhite.png");
    return chunk(0x19000, Buffer.concat([name, uints(14000, 1, 1, 24, 0, 1, 1, 0, 0)]), [
        chunk(0x19001, Buffer.concat([name, uints(14000, 1, 1, 24, 0, 0, 1)]), [
            chunk(0x19002, Buffer.concat([uints(img.length), img]))
        ])
    ]);
}

function readObj(filename) {
    const positions = [], normals = [], uvs = [];
    const meshes = new Map();
    let active;
    for (const rawLine of fs.readFileSync(filename, "utf8").split(/\r?\n/)) {
        const [kind, ...fields] = rawLine.trim().split(/\s+/);
        if (kind === "v") {
            const [x, y, z] = fields.map(Number);
            // The test piece sits on the grass by the Simpsons house.
            positions.push([x + 224, y + 3.46, z - 177.8]);
        } else if (kind === "vn") normals.push(fields.map(Number));
        else if (kind === "vt") uvs.push([Number(fields[0]), 1 - Number(fields[1])]);
        else if (kind === "usemtl") {
            if (!meshes.has(fields[0])) meshes.set(fields[0], { points: [], normals: [], uvs: [], indices: [], keys: new Map() });
            active = meshes.get(fields[0]);
        } else if (kind === "f") {
            if (!active || fields.length < 3) throw new Error("Expected a material on each OBJ face");
            const polygon = fields.map(key => {
                if (active.keys.has(key)) return active.keys.get(key);
                const [p, t, n] = key.split("/").map(Number);
                if (!positions[p - 1] || !normals[n - 1] || !uvs[t - 1])
                    throw new Error("The preview OBJ needs positive position/UV/normal indices");
                const vertex = active.points.length;
                active.keys.set(key, vertex);
                active.points.push(positions[p - 1]);
                active.normals.push(normals[n - 1]);
                active.uvs.push(uvs[t - 1]);
                return vertex;
            });
            for (let i = 1; i < polygon.length - 1; i++)
                active.indices.push(polygon[0], polygon[i], polygon[i + 1]);
        }
    }
    return meshes;
}

function staticEntity(mesh, shaderName, name, translucent) {
    const { points, normals, uvs, indices } = mesh;
    if (!points.length || points.length > 65535) throw new Error("Unexpected preview mesh size");
    const min = [0,1,2].map(axis => Math.min(...points.map(point => point[axis])));
    const max = [0,1,2].map(axis => Math.max(...points.map(point => point[axis])));
    const center = min.map((value, axis) => (value + max[axis]) * 0.5);
    const radius = Math.max(...points.map(point => Math.hypot(...point.map((value, axis) => value - center[axis]))));
    const group = chunk(0x10002, Buffer.concat([uints(0), pstring(shaderName),
        uints(0, 0x2011, points.length, indices.length, 0)]), [
        chunk(0x10005, Buffer.concat([uints(points.length), floats(points.flat())])),
        chunk(0x10006, Buffer.concat([uints(normals.length), floats(normals.flat())])),
        chunk(0x10007, Buffer.concat([uints(uvs.length, 0), floats(uvs.flat())])),
        chunk(0x1000a, uints(indices.length, ...indices))
    ]);
    const geometry = chunk(0x10000, Buffer.concat([pstring(name + "Mesh"), uints(0, 1)]), [
        group, chunk(0x10003, floats([...min, ...max])), chunk(0x10004, floats([...center, radius]))
    ]);
    return chunk(0x03f00000, Buffer.concat([pstring(name), uints(0, Number(translucent))]), [geometry]);
}

function panelBox(low, high) {
    const mesh = { points: [], normals: [], uvs: [], indices: [] };
    const corners = [[0, 0], [1, 0], [1, 1], [0, 1]];
    for (let axis = 0; axis < 3; axis++) {
        for (const side of [0, 1]) {
            const u = (axis + 1) % 3;
            const v = (axis + 2) % 3;
            const normal = [0, 0, 0];
            normal[axis] = side ? 1 : -1;
            const first = mesh.points.length;
            for (const [s, t] of corners) {
                const point = [...low];
                point[axis] = side ? high[axis] : low[axis];
                point[u] = s ? high[u] : low[u];
                point[v] = t ? high[v] : low[v];
                mesh.points.push(point);
                mesh.normals.push(normal);
                mesh.uvs.push([s, t]);
            }
            const triangles = side ? [0, 1, 2, 0, 2, 3] : [0, 2, 1, 0, 3, 2];
            mesh.indices.push(...triangles.map(index => first + index));
        }
    }
    return mesh;
}

function bounceDemo(chunks, textureDir) {
    // A horizontal receiver and a vertical emitter form a corner beside the
    // chrome preview. View the open side to see the bounce fall off across it.
    const panels = [
        ["PBRBouncePanel", [225.98, 3.54, -179.24], [229.25, 3.87, -176.92]],
        ["PBRBounceEmitter", [225.86, 3.87, -179.24], [226.02, 5.32, -176.92]]
    ];
    for (const [name, low, high] of panels) {
        const shaderName = name + "_m";
        chunks.push(shader(shaderName, false), staticEntity(panelBox(low, high), shaderName, name, false));
    }
    writeTga(path.join(textureDir, "bounce-emission.tga"), {
        width: 1, height: 1, pixels: Buffer.from([30, 158, 255, 255])
    });
}

const [input, output, overrideTextures] = process.argv.slice(2);
if (!input || !output) throw new Error("Usage: node build-preview.cjs EXTRACTED_MATERIAL_PREVIEW OUTPUT_FOLDER [TEXTURE_FOLDER]");
const sourceTextures = overrideTextures || path.join(input, "MaterialPreviewMesh_Textures");
const textureDir = path.join(output, "VehicleShaders");
fs.mkdirSync(textureDir, { recursive: true });
const meshes = readObj(path.join(input, "MaterialPreviewMesh.obj"));
const parts = [
    ["MaterialPreviewMesh_MAT", "preview-body", "PBRPreviewBody", false],
    ["MaterialPreviewMesh_GLASS_MAT", "preview-glass", "PBRPreviewGlass", true]
];
const chunks = [whiteTexture()];
for (const [material, prefix, name, translucent] of parts) {
    const mesh = meshes.get(material);
    if (!mesh) throw new Error("Missing OBJ material " + material);
    packTextures(sourceTextures, textureDir, material, prefix);
    const shaderName = name + "_m";
    chunks.push(shader(shaderName, translucent), staticEntity(mesh, shaderName, name, translucent));
}
bounceDemo(chunks, textureDir);
prefilterEnvironment(path.join(input, "Panorama_MirrorBall_Spheremap.hdr"),
                     path.join(textureDir, "preview-environment.dds"));
fs.writeFileSync(path.join(output, "PBRPreview.p3d"), chunk(0xff443350, Buffer.alloc(0), chunks));
