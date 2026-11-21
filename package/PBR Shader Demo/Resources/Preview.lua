local path = "/GameDir/art/l1z1.p3d"
local previewPath = GetModPath() .. "/Resources/PBRPreview.p3d"
if IsWriting() or not Exists(path, true, false) or not Exists(previewPath, true, false) then return end

local level = ReadFile(path)
local preview = ReadFile(previewPath)
local header = string.pack("<I4I4", 0xff443350, 12)
if not level or not preview or level:sub(1, 8) ~= header or preview:sub(1, 8) ~= header or
   #level < 12 or #preview < 12 or string.unpack("<I4", level, 9) ~= #level or
   string.unpack("<I4", preview, 9) ~= #preview then return end

-- Put the authored mesh in the stock zone without shipping any of its geometry.
Output(header .. string.pack("<I4", #level + #preview - 12) .. level:sub(13) .. preview:sub(13))
