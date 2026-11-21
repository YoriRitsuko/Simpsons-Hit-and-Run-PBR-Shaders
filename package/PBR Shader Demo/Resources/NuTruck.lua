local stockPath = "/GameDir/art/cars/nuctruck.p3d"
local remasterPath = GetModPath() .. "/Resources/NuTruckRemaster.p3d"
if IsWriting() or not Exists(stockPath, true, false) or not Exists(remasterPath, true, false) then return end

local rootHeader = string.pack("<I4I4", 0xff443350, 12)
local meshType = 0x10000
local shaderType = 0x11000
local meshNames = { nuctruckShape = true, wShape0 = true, windShape = true }

local function readName(bytes, headerSize)
    local name, nextField = string.unpack("<s1", bytes, 13)
    assert(nextField <= headerSize + 1)
    return name:match("^[^%z]*"), nextField
end

local function visitChunks(data, first, visit)
    while first <= #data do
        local id, headerSize, size = string.unpack("<I4I4I4", data, first)
        local last = first + size - 1
        assert(headerSize >= 12 and size >= headerSize and last <= #data)
        visit(id, data:sub(first, last), headerSize)
        first = last + 1
    end
end

local function visitFile(path, visit)
    local data = ReadFile(path)
    assert(data and data:sub(1, 8) == rootHeader and string.unpack("<I4", data, 9) == #data)
    visitChunks(data, 13, visit)
end

local function rewriteChildren(bytes, edit, countChildren)
    local id, headerSize = string.unpack("<I4I4", bytes)
    local children = {}
    visitChunks(bytes, headerSize + 1, function(childId, child, childHeaderSize)
        children[#children + 1] = edit(childId, child, childHeaderSize)
    end)
    local header = bytes:sub(13, headerSize)
    if countChildren then
        assert(#header == 4)
        header = string.pack("<I4", #children)
    end
    local content = table.concat(children)
    return string.pack("<I4I4I4", id, headerSize, headerSize + #content) .. header .. content
end

local function alignRearAxle(skeleton)
    local shifted = {}
    local updated = rewriteChildren(skeleton, function(id, joint, headerSize)
        if id ~= 0x4501 then return joint end
        local name, fields = readName(joint, headerSize)
        if name ~= "w0" and name ~= "w1" then return joint end

        -- Both wheel pivots are root children. Keep the tire and suspension axis.
        local z = fields + 6 * 4 + 14 * 4
        assert(not shifted[name] and string.unpack("<i4", joint, fields) == 0 and z + 7 == headerSize)
        shifted[name] = true
        return joint:sub(1, z - 1) .. string.pack("<f", -1.52) .. joint:sub(z + 4)
    end)
    assert(shifted.w0 and shifted.w1)
    return updated
end

local function detachBarrelGlow(composite)
    local detached = false
    local updated = rewriteChildren(composite, function(id, list)
        if id ~= 0x4514 then return list end
        return rewriteChildren(list, function(propId, prop, propHeaderSize)
            if propId == 0x4516 and readName(prop, propHeaderSize) == "nucGlowGroupShape" then
                -- The stock animation can still resolve the glow asset in the file.
                assert(not detached)
                detached = true
                return nil
            end
            return prop
        end, true)
    end)
    assert(detached)
    return updated
end

local function buildReplacement()
    local replacements, chunks = {}, {}
    visitFile(remasterPath, function(id, bytes, headerSize)
        if id == meshType then
            local name = readName(bytes, headerSize)
            assert(meshNames[name] and not replacements[name])
            replacements[name] = bytes
        else
            assert(id == 0x44524250 or id == 0x19000 or id == shaderType)
            chunks[#chunks + 1] = bytes
        end
    end)
    for name in pairs(meshNames) do assert(replacements[name]) end

    local hasInteriorMaterial, hasSkeleton, hasComposite = false, false, false
    visitFile(stockPath, function(id, bytes, headerSize)
        if id == meshType then
            local name = readName(bytes, headerSize)
            if meshNames[name] then
                assert(replacements[name])
                bytes = replacements[name]
                replacements[name] = nil
            end
        elseif id == shaderType and readName(bytes, headerSize) == "char_swatches_m" then
            hasInteriorMaterial = true
        elseif id == 0x4500 and readName(bytes, headerSize) == "nuctruck" then
            assert(not hasSkeleton)
            hasSkeleton = true
            bytes = alignRearAxle(bytes)
        elseif id == 0x4512 and readName(bytes, headerSize) == "nuctruck" then
            assert(not hasComposite)
            hasComposite = true
            bytes = detachBarrelGlow(bytes)
        end
        chunks[#chunks + 1] = bytes
    end)
    assert(not next(replacements) and hasInteriorMaterial and hasSkeleton and hasComposite)
    local content = table.concat(chunks)
    return rootHeader .. string.pack("<I4", 12 + #content) .. content
end

local ok, model = pcall(buildReplacement)
if ok then Output(model) end
