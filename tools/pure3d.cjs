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

module.exports = { uints, floats, pstring, chunk, shader };
