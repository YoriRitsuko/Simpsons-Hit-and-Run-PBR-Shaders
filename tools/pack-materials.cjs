const fs = require("node:fs");
const path = require("node:path");
const { createHash } = require("node:crypto");
const { targaInfo } = require("./targa.cjs");

const pure3d = 0xff443350;
const materialChunk = 0x44524250; // PBRD
const maxPackageBytes = 64 * 1024 * 1024;
const textureKeys = new Map([
    ["basecolor", ".tga"], ["normalmap", ".tga"], ["orm", ".tga"], ["emissivemap", ".tga"],
    ["spheremap", ".tga"], ["environment", ".dds"], ["interiormap", ".tga"],
]);

function readMaterials(filename) {
    const sections = new Map();
    let active;
    for (const [index, raw] of fs.readFileSync(filename, "utf8").split(/\r?\n/).entries()) {
        const line = raw.trim();
        if (!line || line.startsWith(";") || line.startsWith("#")) continue;
        const match = /^\[([\x20-\x7e]+)\]$/.exec(line);
        if (match) {
            const name = match[1].trim().toLowerCase();
            if (!name || sections.has(name)) throw Error("Duplicate or empty material: " + line);
            active = new Map();
            sections.set(name, active);
            continue;
        }
        const field = /^([A-Za-z][A-Za-z0-9]*)\s*=\s*(.+)$/.exec(line);
        if (!active || !field) throw Error(`Expected a section or setting at line ${index + 1}: ${line}`);
        const key = field[1].toLowerCase();
        if (active.has(key)) throw Error("Duplicate material setting: " + field[1]);
        active.set(key, field[2].trim());
    }
    return sections;
}

function words(...values) {
    const result = Buffer.alloc(values.length * 4);
    values.forEach((value, index) => result.writeUInt32LE(value, index * 4));
    return result;
}

function text(value) {
    const bytes = Buffer.from(value, "utf8");
    if (!bytes.length || bytes.length >= 256 || bytes.includes(0)) throw Error("Invalid material text: " + value);
    const length = Buffer.alloc(2);
    length.writeUInt16LE(bytes.length);
    return Buffer.concat([length, bytes]);
}

function textureData(name, extension, data) {
    if (extension === ".tga") {
        try { targaInfo(data); }
        catch (error) { throw Error(`${name}. ${error.message}`); }
    } else if (data.length < 128 || data.toString("ascii", 0, 4) !== "DDS " || data.readUInt32LE(84) !== 113) {
        throw Error(name + " must be a mipmapped RGBA16F DDS from the environment prefilter tool");
    }
    return data;
}

function modelChunks(model) {
    if (!model) return [];
    if (model.length < 12 || model.readUInt32LE(0) !== pure3d || model.readUInt32LE(4) !== 12 ||
        model.readUInt32LE(8) !== model.length) throw Error("Expected an uncompressed little-endian Pure3D model");
    const chunks = [];
    for (let offset = 12; offset < model.length;) {
        if (offset + 12 > model.length) throw Error("Truncated Pure3D child header");
        const header = model.readUInt32LE(offset + 4), size = model.readUInt32LE(offset + 8);
        if (header < 12 || size < header || offset + size > model.length) throw Error("Invalid Pure3D child length");
        if (model.readUInt32LE(offset) !== materialChunk) chunks.push(model.subarray(offset, offset + size));
        offset += size;
    }
    return chunks;
}

function packMaterials(materials, readAsset, model) {
    if (!materials.size || materials.size > 256) throw Error("Use 1 to 256 material sections");
    const assets = new Map();
    const materialRecords = [];
    for (const [name, settings] of materials) {
        const mode = settings.get("mode")?.toLowerCase();
        if (!["pbr", "interior", "glass", "paint"].includes(mode))
            throw Error(`Choose Mode=PBR, Interior, Glass, or Paint in [${name}]`);
        if (mode === "interior" && (!settings.has("basecolor") || !settings.has("interiormap")))
            throw Error(`[${name}] needs BaseColor and InteriorMap`);
        if (settings.size > 32) throw Error(`Too many settings in [${name}]`);
        materialRecords.push(text(name), words(settings.size));
        for (const [key, value] of settings) {
            const extension = textureKeys.get(key);
            if (extension) {
                if (/[\\/:*?"<>|]/.test(value) || !value.toLowerCase().endsWith(extension))
                    throw Error(`${key} needs a bare ${extension} filename in [${name}]`);
                const lower = value.replace(/[A-Z]/g, letter => letter.toLowerCase());
                if (!assets.has(lower)) assets.set(lower, textureData(value, extension, readAsset(value)));
            }
            materialRecords.push(text(key), text(value));
        }
    }
    if (assets.size > 256) throw Error("Use 256 maps or fewer in a Pure3D package");
    const records = [words(assets.size, materials.size)];
    for (const [filename, bytes] of assets) records.push(text(filename), words(bytes.length), bytes);
    records.push(...materialRecords);
    const body = Buffer.concat(records);
    if (body.length + 20 > maxPackageBytes) throw Error("Split materials into packages smaller than 64 MiB");
    const id = createHash("sha256").update(body).digest().subarray(0, 16);
    const chunkLength = 12 + 4 + id.length + body.length;
    const children = modelChunks(model);
    const fileLength = 12 + chunkLength + children.reduce((sum, child) => sum + child.length, 0);
    return Buffer.concat([words(pure3d, 12, fileLength), words(materialChunk, chunkLength, chunkLength),
        words(1), id, body, ...children]);
}

function writePackage(output, bytes) {
    const temp = path.join(path.dirname(path.resolve(output)), `.pbr-${process.pid}-${Date.now()}.tmp`);
    try {
        fs.writeFileSync(temp, bytes, { flag: "wx" });
        fs.renameSync(temp, output);
    } finally {
        if (fs.existsSync(temp)) fs.unlinkSync(temp);
    }
}

module.exports = { readMaterials, packMaterials, writePackage };

if (require.main === module) {
    try {
        const [ini, output, model] = process.argv.slice(2);
        if (!ini || !output) throw Error("Usage: node pack-materials.cjs MATERIALS.ini OUTPUT.p3d [INPUT_MODEL.p3d]");
        const bytes = packMaterials(readMaterials(ini), name => fs.readFileSync(path.join(path.dirname(ini), name)),
            model && fs.readFileSync(model));
        writePackage(output, bytes);
        console.log(`Packed ${output} (${bytes.length} bytes)`);
    } catch (error) {
        console.error(error.message);
        process.exitCode = 1;
    }
}
