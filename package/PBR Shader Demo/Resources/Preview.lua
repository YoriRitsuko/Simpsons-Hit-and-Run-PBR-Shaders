local path = "/GameDir/art/l1z1.p3d"
if IsWriting() or not Exists(path, true, false) then return end

local header = string.pack("<I4I4", 0xff443350, 12)
local function payload(filename)
    if not Exists(filename, true, false) then return end
    local data = ReadFile(filename)
    if data and #data >= 12 and data:sub(1, 8) == header and string.unpack("<I4", data, 9) == #data then
        return data:sub(13)
    end
end

local level = payload(path)
if not level then return end
local resources = GetModPath() .. "/Resources/"
local preview = payload(resources .. "PBRPreview.p3d") or ""
local interior = payload(resources .. "PBRInterior.p3d") or ""
if #preview + #interior == 0 then return end

-- Append the authored props to the player's stock Level 1 zone.
Output(header .. string.pack("<I4", 12 + #level + #preview + #interior) .. level .. preview .. interior)
