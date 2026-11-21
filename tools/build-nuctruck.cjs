const fs = require("node:fs");
const path = require("node:path");
const { readPng } = require("./png.cjs");
const { encodeTga } = require("./targa.cjs");
const { uints, floats, pstring, chunk, shader } = require("./pure3d.cjs");
const { packMaterials, writePackage } = require("./pack-materials.cjs");

const sourceMaterial = "nuctruck_vAll_m";
const interiorMaterial = "char_swatches_m";
const bodyMaterial = "PBRNuTruckBody_m";
const glassMaterial = "PBRNuTruckGlass_m";
const colorTexture = "PBRNuTruckBase.png";
const shapes = [
    ["nuctruckShape", new Map([[sourceMaterial, bodyMaterial], [interiorMaterial, interiorMaterial]])],
    ["wShape0", new Map([[sourceMaterial, bodyMaterial]])],
    ["windShape", new Map([[sourceMaterial, glassMaterial]])],
];

function triangulate(indices, points, filename) {
    if (indices.length === 3) return [indices[0], indices[2], indices[1]];

    // Project onto the plane that retains the most area, then clip ears.
    // Some of the body panels are concave, so a triangle fan can cross an edge.
    const normal = [0, 0, 0];
    for (let i = 0; i < indices.length; i++) {
        const [x, y, z] = points[indices[i]];
        const [nx, ny, nz] = points[indices[(i + 1) % indices.length]];
        normal[0] += y * nz - z * ny;
        normal[1] += z * nx - x * nz;
        normal[2] += x * ny - y * nx;
    }
    const axis = normal.map(Math.abs).indexOf(Math.max(...normal.map(Math.abs)));
    const u = (axis + 1) % 3, v = (axis + 2) % 3;
    const winding = Math.sign(normal[axis]);
    const area = (a, b, c) => {
        const p = points[a], q = points[b], r = points[c];
        return winding * ((q[u] - p[u]) * (r[v] - p[v]) - (q[v] - p[v]) * (r[u] - p[u]));
    };

    const pending = indices.slice();
    const triangles = [];
    const epsilon = 1e-12;
    while (pending.length > 3) {
        const ear = pending.findIndex((b, i) => {
            const a = pending[(i + pending.length - 1) % pending.length];
            const c = pending[(i + 1) % pending.length];
            return area(a, b, c) > epsilon && !pending.some(p =>
                p !== a && p !== b && p !== c &&
                area(a, b, p) >= -epsilon && area(b, c, p) >= -epsilon && area(c, a, p) >= -epsilon);
        });
        if (ear === -1) throw Error("Could not triangulate a face in " + filename);
        const a = pending[(ear + pending.length - 1) % pending.length];
        const c = pending[(ear + 1) % pending.length];
        triangles.push(a, c, pending[ear]);
        pending.splice(ear, 1);
    }
    triangles.push(pending[0], pending[2], pending[1]);
    return triangles;
}

function readObj(filename, assignments) {
    const positions = [], normals = [], uvs = [];
    const groups = new Map();
    let selected;
    for (const line of fs.readFileSync(filename, "utf8").split(/\r?\n/)) {
        const [tag, ...fields] = line.trim().split(/\s+/);
        if (tag === "v" || tag === "vn") {
            const [x, y, z] = fields.map(Number);
            if (![x, y, z].every(Number.isFinite)) throw Error("Invalid vertex or normal in " + filename);
            // The Blender export points down -Z. The stock truck's front is +Z.
            (tag === "v" ? positions : normals).push([x, y, -z]);
        } else if (tag === "vt") {
            const [u, v] = fields.map(Number);
            if (![u, v].every(Number.isFinite)) throw Error("Invalid UV in " + filename);
            uvs.push([u, 1 - v]);
        } else if (tag === "usemtl") {
            const name = assignments.get(fields[0]);
            if (!name) throw Error("Unexpected material " + fields[0] + " in " + filename);
            if (!groups.has(name))
                groups.set(name, { points: [], normals: [], uvs: [], indices: [], vertices: new Map() });
            selected = groups.get(name);
        } else if (tag === "f") {
            if (!selected || fields.length < 3) throw Error("Expected faces with materials in " + filename);
            const indices = fields.map(key => {
                if (selected.vertices.has(key)) return selected.vertices.get(key);
                const [p, t, n] = key.split("/").map(Number);
                if (!positions[p - 1] || !uvs[t - 1] || !normals[n - 1])
                    throw Error("The OBJ needs positive position, UV, and normal indices: " + filename);
                const index = selected.points.length;
                selected.vertices.set(key, index);
                selected.points.push(positions[p - 1]);
                selected.normals.push(normals[n - 1]);
                selected.uvs.push(uvs[t - 1]);
                return index;
            });
            // Reflecting Z also reverses face winding.
            selected.indices.push(...triangulate(indices, selected.points, filename));
        }
    }
    for (const name of assignments.values()) {
        if (!groups.get(name)?.indices.length) throw Error("Missing faces for " + name + " in " + filename);
    }
    return groups;
}

function mesh(name, groups) {
    const primitives = [];
    const bounds = { min: [Infinity, Infinity, Infinity], max: [-Infinity, -Infinity, -Infinity] };
    for (const [material, { points, normals, uvs, indices }] of groups) {
        if (points.length > 65535) throw Error("Too many vertices in " + material);
        for (const point of points) {
            for (let axis = 0; axis < 3; axis++) {
                bounds.min[axis] = Math.min(bounds.min[axis], point[axis]);
                bounds.max[axis] = Math.max(bounds.max[axis], point[axis]);
            }
        }
        // Triangle list, with a normal and one UV set per vertex.
        primitives.push(chunk(0x10002, Buffer.concat([uints(0), pstring(material),
            uints(0, 0x2011, points.length, indices.length, 0)]), [
            chunk(0x10005, Buffer.concat([uints(points.length), floats(points.flat())])),
            chunk(0x10006, Buffer.concat([uints(normals.length), floats(normals.flat())])),
            chunk(0x10007, Buffer.concat([uints(uvs.length, 0), floats(uvs.flat())])),
            chunk(0x1000a, uints(indices.length, ...indices)),
        ]));
    }
    const center = bounds.min.map((min, axis) => (min + bounds.max[axis]) * 0.5);
    let radius = 0;
    for (const group of groups.values()) {
        for (const [x, y, z] of group.points)
            radius = Math.max(radius, Math.hypot(x - center[0], y - center[1], z - center[2]));
    }
    return chunk(0x10000, Buffer.concat([pstring(name), uints(0, primitives.length)]), [
        ...primitives,
        chunk(0x10003, floats([...bounds.min, ...bounds.max])),
        chunk(0x10004, floats([...center, radius])),
    ]);
}

function pngTexture(filename, width, height, png) {
    const name = pstring(filename);
    return chunk(0x19000, Buffer.concat([name, uints(14000, width, height, 32, 8, 1, 0, 0, 0)]), [
        chunk(0x19001, Buffer.concat([name, uints(14000, width, height, 32, 0, 1, 1)]), [
            chunk(0x19002, Buffer.concat([uints(png.length), png])),
        ]),
    ]);
}

function build(input, output) {
    const texture = channel => path.join(input, "NuTruck_Textures", "NuTruck_low_nuctruck_vAll_m_" + channel + ".png");
    const base = readPng(texture("BaseColor"));
    const normal = readPng(texture("Normal"));
    const emission = readPng(texture("Emissive"));
    const roughness = readPng(texture("Roughness"));
    const metallic = readPng(texture("Metallic"));
    for (const map of [normal, emission, roughness, metallic]) {
        if (map.width !== base.width || map.height !== base.height) throw Error("The truck maps must be the same size");
    }
    const pixels = Buffer.alloc(base.pixels.length, 255);
    for (let i = 0; i < pixels.length; i += 4) {
        // A separate AO bake was not supplied. White gives unoccluded ORM.
        pixels[i + 1] = roughness.pixels[i];
        pixels[i + 2] = metallic.pixels[i];
    }
    const assets = new Map([
        ["nuctruck-base.tga", encodeTga(base)],
        ["nuctruck-normal.tga", encodeTga(normal)],
        ["nuctruck-emission.tga", encodeTga(emission)],
        ["nuctruck-orm.tga", encodeTga({ width: base.width, height: base.height, pixels })],
    ]);
    const maps = [
        ["mode", "PBR"], ["basecolor", "nuctruck-base.tga"], ["normalmap", "nuctruck-normal.tga"],
        ["emissivemap", "nuctruck-emission.tga"], ["orm", "nuctruck-orm.tga"],
    ];
    const materials = new Map([
        [bodyMaterial.toLowerCase(), new Map(maps)],
        [glassMaterial.toLowerCase(), new Map([...maps, ["alphamode", "Blend"]])],
    ]);
    const children = [
        pngTexture(colorTexture, base.width, base.height, fs.readFileSync(texture("BaseColor"))),
        shader(bodyMaterial, false, colorTexture),
        shader(glassMaterial, true, colorTexture, true),
        ...shapes.map(([name, assignments]) => mesh(name, readObj(path.join(input, name + ".obj"), assignments))),
    ];
    const model = chunk(0xff443350, Buffer.alloc(0), children);
    const packed = packMaterials(materials, name => assets.get(name), model);
    writePackage(output, packed);
    console.log(`Packed NucTruck meshes and materials (${packed.length} bytes)`);
}

try {
    const [input, output] = process.argv.slice(2);
    if (!input || !output) throw Error("Usage: node tools/build-nuctruck.cjs EXTRACTED_ZIP OUTPUT.p3d");
    build(input, output);
} catch (error) {
    console.error(error.message);
    process.exitCode = 1;
}
