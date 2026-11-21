const fs = require("node:fs");
const path = require("node:path");
const zlib = require("node:zlib");
const { prefilterEnvironment } = require("./prefilter-environment.cjs");
const { readMaterials, packMaterials, writePackage } = require("./pack-materials.cjs");
const { encodeTga } = require("./targa.cjs");

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

function writeTga(filename, image) {
    fs.writeFileSync(filename, encodeTga(image));
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

function shader(name, translucent, textureName = "PBRPreviewWhite.png", twoSided = false) {
    const texture = chunk(0x11002, Buffer.concat([Buffer.from("TEX\0"), pstring(textureName)]));
    const params = [["LIT\0", 1], ["BLMD", translucent ? 1 : 0], ["2SID", Number(twoSided)], ["ATST", 0]]
        .map(([key, value]) => chunk(0x11003, Buffer.concat([Buffer.from(key), uints(value)])));
    return chunk(0x11000, Buffer.concat([pstring(name), uints(0), pstring("simple"),
        uints(Number(translucent), 0x11, 0xffffffff, params.length + 1)]), [texture, ...params]);
}

function whiteTexture(textureName = "PBRPreviewWhite.png") {
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
    const name = pstring(textureName);
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

function drawTexture(width, height, sample) {
    const pixels = Buffer.alloc(width * height * 4);
    for (let y = 0; y < height; y++) {
        for (let x = 0; x < width; x++)
            pixels.set(sample((x + 0.5) / width, (y + 0.5) / height), (y * width + x) * 4);
    }
    return { width, height, pixels };
}

function facadeTexel(u, v) {
    if (u < 0.045 || u > 0.955 || v < 0.07 || v > 0.93) return [99, 60, 46, 0];
    if (u < 0.085 || u > 0.915 || v < 0.12 || v > 0.87) return [195, 131, 79, 0];
    if (Math.abs(u - 0.5) < 0.016 || Math.abs(v - 0.50) < 0.028) return [119, 71, 55, 0];
    return [68, 50, 40, 255];
}

function roomTexel(axis, hit) {
    const [x, y, z] = hit;
    if (axis === 1) {
        if (y < 0.5)
            return Math.abs(x - 0.5) < 0.20 && Math.abs(z - 0.55) < 0.13 ? [255, 227, 165, 255] : [169, 150, 111, 0];
        const tile = (Math.floor(x * 8) + Math.floor(z * 8)) % 2;
        return tile ? [148, 97, 66, 0] : [109, 70, 53, 0];
    }
    if (y > 0.91) return [77, 46, 39, 0];
    if (y > 0.88 || y < 0.07) return [168, 116, 76, 0];
    if (axis === 2 && z > 0.5) {
        if (x > 0.20 && x < 0.79 && y > 0.22 && y < 0.65) {
            if (x < 0.24 || x > 0.75 || y < 0.26 || y > 0.61) return [102, 62, 48, 0];
            if (Math.abs(x - 0.50) < 0.014) return [141, 90, 58, 0];
            return y < 0.39 ? [120, 168, 175, 45] : [212, 147, 79, 66];
        }
        return [114, 145, 123, 0];
    }
    if (axis === 0) {
        if (z > 0.25 && z < 0.68 && y > 0.50 && y < 0.77) {
            if (y > 0.72 || y < 0.56) return [110, 62, 41, 0];
            return Math.floor(z * 22) % 2 ? [155, 86, 68, 0] : [199, 133, 74, 0];
        }
        return x < 0.5 ? [162, 118, 87, 0] : [179, 141, 93, 0];
    }
    return [131, 104, 78, 0];
}

function interiorDemo(output) {
    const size = [2.6, 1.6, 2.2];
    const center = [221.3, 4.34, -177.7];
    const tangent = [Math.SQRT1_2, 0, -Math.SQRT1_2];
    const normal = [Math.SQRT1_2, 0, Math.SQRT1_2];
    const uvs = [[0, 0], [0, 1], [1, 1], [1, 0]];
    const mesh = {
        points: uvs.map(([u, v]) => center.map((origin, axis) => origin + tangent[axis] * (u - 0.5) * size[0]
            + (axis === 1 ? (0.5 - v) * size[1] : 0))),
        normals: uvs.map(() => normal),
        uvs,
        indices: [0, 1, 2, 0, 2, 3],
    };
    const textureName = "InteriorPreviewWhite.png";
    const material = "PBRInteriorWindow_m";
    const chunks = [whiteTexture(textureName), shader(material, false, textureName, true),
        staticEntity(mesh, material, "PBRInteriorWindow", false)];
    fs.mkdirSync(output, { recursive: true });
    fs.writeFileSync(path.join(output, "PBRInterior.p3d"), chunk(0xff443350, Buffer.alloc(0), chunks));
    writeTga(path.join(output, "interior-preview-facade.tga"), drawTexture(256, 128, facadeTexel));
    writeTga(path.join(output, "interior-preview-room.tga"), drawTexture(256, 256, (u, v) => {
        const px = (u - 0.5) * 2;
        const py = (v - 0.5) * 2;
        const r2 = Math.min(px * px + py * py, 1);
        const scale = 2 * Math.sqrt(1 - r2);
        // Invert SphereUV, looking into a room with the back wall at the map center.
        const ray = [px * scale / size[0], py * scale / size[1], (1 - 2 * r2) / size[2]];
        const axis = ray.reduce((best, value, i) => Math.abs(value) > Math.abs(ray[best]) ? i : best, 0);
        const hit = ray.map(component => 0.5 + 0.5 * component / Math.abs(ray[axis]));
        return roomTexel(axis, hit);
    }));
}

const [input, output, overrideTextures] = process.argv.slice(2);
if (!input || !output) throw new Error("Usage: node build-preview.cjs EXTRACTED_MATERIAL_PREVIEW|--interior|--pack-demo OUTPUT_FOLDER [TEXTURE_FOLDER]");

function packDemo(textureDir) {
    fs.mkdirSync(output, { recursive: true });
    const targets = [["demo-materials.ini", "PBRMaterials.p3d"], ["demo-sedan.ini", "PBRSedanMaterials.p3d"]];
    for (const [config, filename] of targets) {
        const materials = readMaterials(path.join(__dirname, config));
        const packed = packMaterials(materials, name => {
            const map = path.join(textureDir, name);
            return fs.readFileSync(fs.existsSync(map) ? map : path.join(__dirname, "..", "resources", name));
        });
        writePackage(path.join(output, filename), packed);
    }
}

if (input === "--interior") {
    interiorDemo(output);
    return;
}
if (input === "--pack-demo") {
    if (!overrideTextures) throw new Error("--pack-demo expects the demo's source maps folder");
    packDemo(overrideTextures);
    return;
}
const sourceTextures = overrideTextures || path.join(input, "MaterialPreviewMesh_Textures");
const textureDir = path.join(output, "AuthoringMaps");
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
packDemo(textureDir);
