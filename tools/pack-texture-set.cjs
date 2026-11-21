const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { packMaterials, writePackage } = require("./pack-materials.cjs");
const { encodeTga, readTga } = require("./targa.cjs");
const { prefilterEnvironment } = require("./prefilter-environment.cjs");

function packedOrm(ao, roughness, metallic) {
    const maps = [ao, roughness, metallic].map(file => file && readTga(fs.readFileSync(file)));
    const { width, height } = maps.find(Boolean);
    if (maps.some(map => map && (map.width !== width || map.height !== height)))
        throw Error("Use the same image size for AO, roughness, and metallic");
    const pixels = Buffer.alloc(width * height * 4, 255);
    const defaults = [255, 128, 0];
    for (let i = 0; i < pixels.length; i += 4) {
        for (let channel = 0; channel < 3; channel++)
            pixels[i + channel] = maps[channel] ? maps[channel].pixels[i] : defaults[channel];
    }
    return encodeTga({ width, height, pixels });
}

function environmentData(filename) {
    if (path.extname(filename).toLowerCase() === ".dds") return fs.readFileSync(filename);
    if (path.extname(filename).toLowerCase() !== ".hdr") throw Error("Choose a mirror-ball HDR or prefiltered DDS");
    const temp = fs.mkdtempSync(path.join(os.tmpdir(), "pbr-environment-"));
    const output = path.join(temp, "environment.dds");
    try {
        prefilterEnvironment(filename, output);
        return fs.readFileSync(output);
    } finally {
        if (fs.existsSync(output)) fs.unlinkSync(output);
        fs.rmdirSync(temp);
    }
}

function packTextureSet(inputs) {
    const name = inputs.shader?.trim();
    if (!name || !/^[\x20-\x7e]+$/.test(name)) throw Error("Enter the exact shader chunk name using ASCII text");
    if (!inputs.output || path.extname(inputs.output).toLowerCase() !== ".p3d")
        throw Error("Choose a .p3d output file");
    const mode = inputs.mode || "PBR";
    if (inputs.interior && mode.toLowerCase() !== "interior") throw Error("Choose Interior mode for a room map");
    const settings = new Map([["mode", mode]]);
    const files = new Map();
    for (const [field, key] of [["base", "basecolor"], ["normal", "normalmap"],
                                ["emissive", "emissivemap"], ["interior", "interiormap"]]) {
        if (!inputs[field]) continue;
        const filename = key + ".tga";
        settings.set(key, filename);
        files.set(filename, fs.readFileSync(inputs[field]));
    }
    const hasChannels = inputs.ao || inputs.roughness || inputs.metallic;
    if (inputs.orm && hasChannels) throw Error("Choose one ORM image or separate AO, roughness, and metallic maps");
    if (inputs.orm || hasChannels) {
        settings.set("orm", "orm.tga");
        files.set("orm.tga", inputs.orm ? fs.readFileSync(inputs.orm) : packedOrm(inputs.ao, inputs.roughness, inputs.metallic));
    }
    if (inputs.environment) {
        settings.set("environment", "environment.dds");
        files.set("environment.dds", environmentData(inputs.environment));
    }
    const bytes = packMaterials(new Map([[name.toLowerCase(), settings]]), filename => files.get(filename),
        inputs.model && fs.readFileSync(inputs.model));
    writePackage(inputs.output, bytes);
    return bytes.length;
}

module.exports = { packTextureSet };

if (require.main === module) {
    try {
        if (!process.argv[2]) throw Error("Usage: node pack-texture-set.cjs INPUTS.json");
        const inputs = JSON.parse(fs.readFileSync(process.argv[2], "utf8").replace(/^\uFEFF/, ""));
        const size = packTextureSet(inputs);
        console.log(`Wrote ${inputs.output} (${size} bytes)`);
    } catch (error) {
        console.error(error.message);
        process.exitCode = 1;
    }
}
