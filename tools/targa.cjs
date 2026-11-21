function targaInfo(bytes) {
    if (bytes.length < 18 || bytes[1] !== 0 || bytes[2] !== 2 || ![24, 32].includes(bytes[16]))
        throw Error("Expected an uncompressed 24-bit or 32-bit true-color TGA");
    const width = bytes.readUInt16LE(12), height = bytes.readUInt16LE(14);
    const stride = bytes[16] / 8, start = 18 + bytes[0];
    if (!width || !height || width > 4096 || height > 4096 || bytes.length < start + width * height * stride)
        throw Error("Invalid TGA dimensions or pixels");
    return { width, height, stride, start };
}

function readTga(bytes) {
    const { width, height, stride, start } = targaInfo(bytes);
    const pixels = Buffer.alloc(width * height * 4);
    for (let y = 0; y < height; y++) {
        const row = bytes[17] & 0x20 ? y : height - 1 - y;
        for (let x = 0; x < width; x++) {
            const column = bytes[17] & 0x10 ? width - 1 - x : x;
            const source = start + (row * width + column) * stride;
            const dest = (y * width + x) * 4;
            pixels[dest] = bytes[source + 2];
            pixels[dest + 1] = bytes[source + 1];
            pixels[dest + 2] = bytes[source];
            pixels[dest + 3] = stride === 4 ? bytes[source + 3] : 255;
        }
    }
    return { width, height, pixels };
}

function encodeTga({ width, height, pixels }) {
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
    return Buffer.concat([header, bgr]);
}

module.exports = { targaInfo, readTga, encodeTga };
