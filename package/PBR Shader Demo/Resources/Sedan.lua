local path = "/GameDir/art/cars/famil_v.p3d"
if IsWriting() or not Exists(path, true, false) then return end
local car = ReadFile(path)
if not car or #car ~= 181860 or string.unpack("<I4", car) ~= 0xff443350 then return end

local function chunk(id, header, payload)
    return string.pack("<I4I4I4", id, 12 + #header, 12 + #header + #payload) .. header .. payload
end

local changed = 0
local function rewrite(first, limit, depth)
    assert(depth <= 24 and first + 12 <= limit)
    local id, headerSize, size = string.unpack("<I4I4I4", car, first)
    local stop = first + size
    assert(headerSize >= 12 and size >= headerSize and stop <= limit)
    local header = car:sub(first + 12, first + headerSize - 1)
    local edited = false
    local children = {}
    local cursor = first + headerSize
    while cursor < stop do
        local child, nextChunk, different = rewrite(cursor, stop, depth + 1)
        children[#children + 1] = child
        cursor = nextChunk
        edited = edited or different
    end

    if id == 0x11000 then
        local name, afterName = string.unpack("<s1", header)
        local shader, afterShader = string.unpack("<s1", header, afterName + 4)
        if name:match("^[^%z]*") == "Windsheild_m" and shader:match("^[^%z]*") == "simple" then
            local alpha, needs, mask, count = string.unpack("<I4I4I4I4", header, afterShader)
            assert(alpha == 1 and count == #children)
            header = header:sub(1, afterName + 3) .. string.pack("<s1I4I4I4I4",
                "spheremap\0\0\0", alpha, needs, mask, count + 2)
            children[#children + 1] = chunk(0x11002, "REFL" .. string.pack("<s1", "EnvMap.bmp\0\0"), "")
            children[#children + 1] = chunk(0x11005, "ENVB" .. string.pack("<I4", 0xff000000), "")
            changed = changed + 1
            edited = true
        end
    end

    if not edited then return car:sub(first, stop - 1), stop, false end
    return chunk(id, header, table.concat(children)), stop, true
end

local ok, result, stop = pcall(rewrite, 1, #car + 1, 0)
if ok and stop == #car + 1 and changed == 1 then Output(result) end
