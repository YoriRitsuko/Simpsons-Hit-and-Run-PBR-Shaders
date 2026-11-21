local stockPath = "/GameDir/art/cars/nuctruck.p3d"
local remasterPath = GetModPath() .. "/Resources/NuTruckRemaster.p3d"
if IsWriting() or not Exists(stockPath, true, false) or not Exists(remasterPath, true, false) then return end

local rootHeader = string.pack("<I4I4", 0xff443350, 12)
local meshType = 0x10000
local shaderType = 0x11000
local meshNames = { nuctruckShape = true, wShape0 = true, windShape = true }
local mounts = {
    -- Match the wheel arch centers and tuck the wider tires under the outer lip.
    w0 = { x = 0.88, z = -1.52 },
    w1 = { x = -0.88, z = -1.52 },
}
local brakeMount = { lift = 0.11, setback = 0.16 }
local reverseMount = { lift = 0.045, setback = 0.16 }
for i = 1, 4 do
    -- Each lamp has two stock sprite layers with slightly different depths.
    mounts["brake" .. i] = brakeMount
    mounts["rev" .. i] = reverseMount
end

local function readName(bytes, headerSize, first)
    local name, nextField = string.unpack("<s1", bytes, first or 13)
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

local function fitTranslation(bytes, first, mount)
    local x, y, z, after = string.unpack("<fff", bytes, first)
    local translation = string.pack("<fff", mount.x or x, y + (mount.lift or 0), mount.z or (z - mount.setback))
    return bytes:sub(1, first - 1) .. translation .. bytes:sub(after)
end

local function fitSkeleton(skeleton)
    local shifted = {}
    local updated = rewriteChildren(skeleton, function(id, joint, headerSize)
        if id ~= 0x4501 then return joint end
        local name, fields = readName(joint, headerSize)
        local mount = mounts[name]
        if not mount then return joint end

        -- These mounts are root children. Their local matrices use truck coordinates.
        local translation = fields + 6 * 4 + 12 * 4
        assert(not shifted[name] and string.unpack("<i4", joint, fields) == 0 and translation + 15 == headerSize)
        shifted[name] = true
        return fitTranslation(joint, translation, mount)
    end)
    for name in pairs(mounts) do assert(shifted[name]) end
    return updated
end

local function fitPoseAnimation(animation)
    local shifted = {}
    local updated = rewriteChildren(animation, function(id, list)
        if id ~= 0x121002 then return list end
        return rewriteChildren(list, function(groupId, group, groupHeaderSize)
            if groupId ~= 0x121001 then return group end
            local name = readName(group, groupHeaderSize, 17)
            local mount = mounts[name]
            if not mount then return group end
            return rewriteChildren(group, function(channelId, channel, channelHeaderSize)
                if channelId ~= 0x121104 or channel:sub(17, 20) ~= "TRAN" then return channel end
                -- PTRN_nuctruck writes absolute translations, overriding the rest pose.
                local keyCount = string.unpack("<I4", channel, 21)
                assert(not shifted[name] and keyCount == 1 and channelHeaderSize == 38)
                shifted[name] = true
                return fitTranslation(channel, 27, mount)
            end)
        end)
    end)
    for name in pairs(mounts) do assert(shifted[name]) end
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

    local hasInteriorMaterial, hasSkeleton, hasAnimation, hasComposite = false, false, false, false
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
            bytes = fitSkeleton(bytes)
        elseif id == 0x121000 and readName(bytes, headerSize, 17) == "PTRN_nuctruck" then
            assert(not hasAnimation)
            hasAnimation = true
            bytes = fitPoseAnimation(bytes)
        elseif id == 0x4512 and readName(bytes, headerSize) == "nuctruck" then
            assert(not hasComposite)
            hasComposite = true
            bytes = detachBarrelGlow(bytes)
        end
        chunks[#chunks + 1] = bytes
    end)
    assert(not next(replacements) and hasInteriorMaterial and hasSkeleton and hasAnimation and hasComposite)
    local content = table.concat(chunks)
    return rootHeader .. string.pack("<I4", 12 + #content) .. content
end

local ok, model = pcall(buildReplacement)
if ok then Output(model) end
