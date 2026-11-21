#include "materials.hpp"

#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <unordered_map>

namespace {

std::wstring directory;
wchar_t logPath[MAX_PATH]{};
std::unordered_map<std::string, Material> configured;
std::unordered_map<void*, const Material*> materials;
SRWLOCK materialLock = SRWLOCK_INIT;
const Material transparentSphere = [] {
    Material material;
    material.useGameSphere = true;
    return material;
}();

thread_local const Material* active = nullptr;
thread_local const Material* preStream = nullptr;
using BeginPrims = void* (__thiscall*)(void*, void*, int, unsigned, int, unsigned);
using EndPrims = void (__thiscall*)(void*, void*);
using DrawPrimBuffer = void (__thiscall*)(void*, void*, void*);
BeginPrims beginPrims;
EndPrims endPrims;
DrawPrimBuffer drawPrimBuffer;
bool renderHookInstalled;

std::string Lower(std::string text)
{
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return text;
}

std::wstring IniValue(const std::wstring& ini, const wchar_t* section,
                     const wchar_t* key, const wchar_t* fallback)
{
    wchar_t value[MAX_PATH];
    GetPrivateProfileStringW(section, key, fallback, value, MAX_PATH, ini.c_str());
    return value;
}

float ParseFloat(const std::wstring& text, float fallback, float maximum)
{
    wchar_t* end;
    const float value = std::wcstof(text.c_str(), &end);
    if (end == text.c_str() || *end || !std::isfinite(value)) return fallback;
    return std::clamp(value, 0.0f, maximum);
}

bool SafeTextureFilename(const std::wstring& name, const wchar_t* extension)
{
    if (name.empty() || name.size() >= 200 ||
        name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) return false;
    const auto dot = name.find_last_of(L'.');
    return dot != std::wstring::npos && _wcsicmp(name.c_str() + dot, extension) == 0;
}

std::wstring TextureSetting(const std::wstring& ini, const wchar_t* section,
                            const wchar_t* key, const wchar_t* extension = L".tga")
{
    auto name = IniValue(ini, section, key, L"");
    return SafeTextureFilename(name, extension) ? name : std::wstring{};
}

void LoadPbr(Material& material, const std::wstring& ini, const wchar_t* section)
{
    material.baseColor = TextureSetting(ini, section, L"BaseColor");
    material.orm = TextureSetting(ini, section, L"ORM");
    material.emissive = TextureSetting(ini, section, L"EmissiveMap");
    material.environment = TextureSetting(ini, section, L"Environment", L".dds");
    const auto alpha = IniValue(ini, section, L"AlphaMode", L"Opaque");
    if (!_wcsicmp(alpha.c_str(), L"Mask")) material.alphaMode = AlphaMode::Mask;
    else if (!_wcsicmp(alpha.c_str(), L"Blend")) material.alphaMode = AlphaMode::Blend;
    material.opacity = ParseFloat(IniValue(ini, section, L"Opacity", L""), 1.0f, 1.0f);
    material.alphaCutoff = ParseFloat(IniValue(ini, section, L"AlphaCutoff", L""), 0.5f, 1.0f);
    material.roughness = ParseFloat(IniValue(ini, section, L"Roughness", L""), 0.5f, 1.0f);
    material.metallic = ParseFloat(IniValue(ini, section, L"Metallic", L""), 0.0f, 1.0f);
    material.exposure = ParseFloat(IniValue(ini, section, L"Exposure", L""), 1.0f, 8.0f);
    material.directLightStrength = ParseFloat(IniValue(ini, section, L"DirectLightStrength", L""), 1.0f, 8.0f);
    material.environmentStrength = ParseFloat(IniValue(ini, section, L"EnvironmentStrength", L""), 1.0f, 8.0f);
    material.emissiveStrength = ParseFloat(IniValue(ini, section, L"EmissiveStrength", L""), 1.0f, 16.0f);
}

void LoadInterior(Material& material, const std::wstring& ini, const wchar_t* section)
{
    material.alphaMode = AlphaMode::Opaque;
    material.interiorMap = TextureSetting(ini, section, L"InteriorMap");
    material.interiorDepth = std::max(0.05f, ParseFloat(IniValue(ini, section, L"InteriorDepth", L""), 3.0f, 30.0f));
    material.interiorTiles = {
        std::max(1.0f, std::round(ParseFloat(IniValue(ini, section, L"InteriorTilesU", L""), 1.0f, 32.0f))),
        std::max(1.0f, std::round(ParseFloat(IniValue(ini, section, L"InteriorTilesV", L""), 1.0f, 32.0f))),
    };
    material.interiorBrightness = ParseFloat(IniValue(ini, section, L"InteriorBrightness", L""), 1.0f, 8.0f);
    material.glassRoughness = ParseFloat(IniValue(ini, section, L"GlassRoughness", L""), 0.08f, 1.0f);
    material.glassReflectance = ParseFloat(IniValue(ini, section, L"GlassReflectance", L""), 0.06f, 1.0f);
}

void LoadConfig()
{
    const std::wstring ini = directory + L"VehicleShaders.ini";
    wchar_t sections[8192];
    const DWORD count = GetPrivateProfileSectionNamesW(sections, 8192, ini.c_str());
    if (!count || count >= 8190) return;

    for (const wchar_t* section = sections; *section; section += wcslen(section) + 1) {
        char name[256];
        if (!WideCharToMultiByte(CP_ACP, 0, section, -1, name, sizeof(name), nullptr, nullptr)) continue;
        const auto mode = IniValue(ini, section, L"Mode", L"");
        Material material;
        if (!_wcsicmp(mode.c_str(), L"Paint")) material.mode = MaterialMode::Paint;
        else if (!_wcsicmp(mode.c_str(), L"PBR")) material.mode = MaterialMode::Pbr;
        else if (!_wcsicmp(mode.c_str(), L"Interior")) material.mode = MaterialMode::Interior;
        else if (_wcsicmp(mode.c_str(), L"Glass")) continue;

        material.normalMap = TextureSetting(ini, section, L"NormalMap");
        material.sphereMap = TextureSetting(ini, section, L"SphereMap");
        material.useGameSphere = material.mode == MaterialMode::Glass && material.sphereMap.empty();
        const auto color = IniValue(ini, section, L"PaintColour", L"FFFFFF");
        if (color.size() == 6 && color.find_first_not_of(L"0123456789ABCDEFabcdef") == std::wstring::npos) {
            const auto rgb = std::wcstoul(color.c_str(), nullptr, 16);
            for (unsigned index = 0; index < 3; ++index) {
                material.paint[index] = static_cast<float>((rgb >> (16 - 8 * index)) & 255) / 255.0f;
            }
        }
        material.reflection = ParseFloat(IniValue(ini, section, L"Reflection", L""), 0.55f, 2.0f);
        material.normalStrength = ParseFloat(IniValue(ini, section, L"NormalStrength", L""), 1.0f, 4.0f);
        material.maskSet = IniValue(ini, section, L"MaskSet", L"0") == L"1" ? 1 : 0;
        if (material.mode == MaterialMode::Pbr || material.mode == MaterialMode::Interior)
            LoadPbr(material, ini, section);
        if (material.mode == MaterialMode::Interior) {
            LoadInterior(material, ini, section);
            if (material.baseColor.empty() || material.interiorMap.empty()) {
                ShaderLog("Interior materials need BaseColor and InteriorMap: ", name);
                continue;
            }
        }
        configured.emplace(Lower(name), std::move(material));
    }
}

bool DemoRequested()
{
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return false;
    bool requested = false;
    for (int index = 1; index < count; ++index) {
        if (!_wcsicmp(arguments[index], L"VEHICLE_SHADERS_DEMO")) {
            requested = true;
            break;
        }
    }
    LocalFree(arguments);
    return requested;
}

void LoadDemo()
{
    if (!DemoRequested()) return;

    Material glass;
    glass.reflection = 1.15f;
    glass.useGameSphere = true;
    configured.insert_or_assign("windsheild_m", glass);

    const auto ini = directory + L"VehicleShaders.ini";
    const auto addPreview = [&ini](const char* name, const wchar_t* stem, AlphaMode alpha) {
        const std::wstring prefix = stem;
        Material pbr;
        pbr.mode = MaterialMode::Pbr;
        pbr.baseColor = prefix + L"-base.tga";
        pbr.orm = prefix + L"-orm.tga";
        pbr.normalMap = prefix + L"-normal.tga";
        pbr.emissive = prefix + L"-emission.tga";
        pbr.environment = L"preview-environment.dds";
        pbr.alphaMode = alpha;
        const std::wstring section(name, name + strlen(name));
        pbr.emissiveStrength = ParseFloat(IniValue(ini, section.c_str(), L"EmissiveStrength", L""), 1.0f, 16.0f);
        configured.try_emplace(name, std::move(pbr));
    };
    addPreview("pbrpreviewbody_m", L"preview-body", AlphaMode::Opaque);
    addPreview("pbrpreviewglass_m", L"preview-glass", AlphaMode::Blend);

    Material bouncePanel;
    bouncePanel.mode = MaterialMode::Pbr;
    bouncePanel.roughness = 0.95f;
    bouncePanel.directLightStrength = 0.58f;
    bouncePanel.environmentStrength = 0.42f;
    configured.try_emplace("pbrbouncepanel_m", bouncePanel);

    Material bounceEmitter;
    bounceEmitter.mode = MaterialMode::Pbr;
    // The test light contributes emission with zero diffuse reflectance.
    bounceEmitter.metallic = 1.0f;
    bounceEmitter.directLightStrength = 0.0f;
    bounceEmitter.environmentStrength = 0.0f;
    bounceEmitter.emissive = L"bounce-emission.tga";
    bounceEmitter.emissiveStrength = ParseFloat(IniValue(ini, L"PBRBounceEmitter_m", L"EmissiveStrength", L""),
                                               4.0f, 16.0f);
    configured.try_emplace("pbrbounceemitter_m", std::move(bounceEmitter));

    Material window;
    window.mode = MaterialMode::Interior;
    window.baseColor = L"interior-preview-facade.tga";
    window.interiorMap = L"interior-preview-room.tga";
    window.interiorDepth = 2.2f;
    window.interiorBrightness = 0.55f;
    window.glassReflectance = 0.09f;
    window.emissiveStrength = 2.0f;
    configured.try_emplace("pbrinteriorwindow_m", std::move(window));

    // These names are unique to the sedan's painted panels. Keep the game
    // texture binding so the normal-to-damaged texture swaps still work.
    Material sedanPaint;
    sedanPaint.mode = MaterialMode::Pbr;
    sedanPaint.roughness = 0.24f;
    for (const auto* name : {"famil_vbacknorm_m", "famil_vdoordnorm_m", "famil_vdoorpnorm_m",
                            "famil_vhoodnorm_m", "famil_vside_m"}) {
        configured.try_emplace(name, sedanPaint);
    }
    ShaderLog("Level 1 glass and PBR demo enabled.");
}

const Material* Lookup(void* shader)
{
    AcquireSRWLockShared(&materialLock);
    const auto found = materials.find(shader);
    const auto* material = found == materials.end() ? nullptr : found->second;
    ReleaseSRWLockShared(&materialLock);
    return material;
}

void* __fastcall WrapBegin(void* context, void*, void* shader, int primitive,
                           unsigned format, int count, unsigned pass)
{
    preStream = active;
    active = Lookup(shader);
    return beginPrims(context, shader, primitive, format, count, pass);
}

void __fastcall WrapEnd(void* context, void*, void* stream)
{
    endPrims(context, stream);
    active = preStream;
}

void __fastcall WrapBuffered(void* context, void*, void* shader, void* buffer)
{
    const Material* previous = active;
    active = Lookup(shader);
    drawPrimBuffer(context, shader, buffer);
    active = previous;
}

void InstallRenderHook()
{
    if (renderHookInstalled) return;
    auto* context = *reinterpret_cast<void***>(0x65ef5c);
    const auto native = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"pddidx8r.dll"));
    if (!context || !native || reinterpret_cast<uintptr_t>(*context) != native + 0x4e774) return;

    auto& methods = *reinterpret_cast<void***>(context);
    constexpr unsigned start = 30;
    DWORD protection;
    if (!VirtualProtect(methods + start, sizeof(void*) * 3, PAGE_READWRITE, &protection)) return;
    beginPrims = reinterpret_cast<BeginPrims>(methods[start]);
    endPrims = reinterpret_cast<EndPrims>(methods[start + 1]);
    drawPrimBuffer = reinterpret_cast<DrawPrimBuffer>(methods[start + 2]);
    methods[start] = reinterpret_cast<void*>(WrapBegin);
    methods[start + 1] = reinterpret_cast<void*>(WrapEnd);
    methods[start + 2] = reinterpret_cast<void*>(WrapBuffered);
    VirtualProtect(methods + start, sizeof(void*) * 3, protection, &protection);
    renderHookInstalled = true;
    ShaderLog("Connected to the Pure3D draw context.");
}

bool SupportedImage()
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
    if (reinterpret_cast<uintptr_t>(dos) != 0x400000 || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
        reinterpret_cast<const uint8_t*>(dos) + dos->e_lfanew);
    return pe->Signature == IMAGE_NT_SIGNATURE && pe->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
        pe->FileHeader.TimeDateStamp == 0x21544c66 && pe->OptionalHeader.SizeOfImage == 0x2d4000;
}

} // namespace

extern "C" void MaterialLoadedHook();
extern "C" void MaterialDestroyedHook();

extern "C" void RecordMaterial(void* material, const char* name, const char* type)
{
    static_assert(sizeof(void*) == 4);
    auto* shader = *reinterpret_cast<void**>(static_cast<uint8_t*>(material) + 0x14);
    if (!shader) return;
    const auto match = configured.find(Lower(name));
    if (match != configured.end()) {
        const Material& selected = match->second;
        if (selected.mode == MaterialMode::Paint && (_stricmp(type, "simple") &&
            _stricmp(type, "spheremap") && _stricmp(type, "environment"))) return;
        AcquireSRWLockExclusive(&materialLock);
        materials[shader] = &selected;
        InstallRenderHook();
        ReleaseSRWLockExclusive(&materialLock);
        ShaderLog("Selected material ", name);
    } else if (!_stricmp(type, "spheremap") &&
               *(static_cast<uint8_t*>(material) + 0x10) != 0) {
        AcquireSRWLockExclusive(&materialLock);
        materials[shader] = &transparentSphere;
        InstallRenderHook();
        ReleaseSRWLockExclusive(&materialLock);
        ShaderLog("Selected transparent spheremap ", name);
    } else {
        return;
    }
}

extern "C" void ForgetMaterial(void* material)
{
    auto* shader = *reinterpret_cast<void**>(static_cast<uint8_t*>(material) + 0x14);
    AcquireSRWLockExclusive(&materialLock);
    materials.erase(shader);
    ReleaseSRWLockExclusive(&materialLock);
}

bool InitializeMaterials()
{
    if (!SupportedImage()) return false;
    wchar_t path[MAX_PATH];
    const DWORD size = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!size || size >= MAX_PATH) return false;
    const wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    directory.assign(path, static_cast<size_t>(slash - path + 1));
    directory += L"VehicleShaders\\";
    const DWORD logLength = GetEnvironmentVariableW(L"SHAR_SHADER_LOG", logPath, MAX_PATH);
    if (logLength >= MAX_PATH) logPath[0] = 0;

    struct Patch {
        uintptr_t address;
        const char* expected;
        unsigned size;
        uint8_t opcode;
        void (*replacement)();
    };
    const Patch patches[] = {
        // Lucas uses the instructions immediately after this call to apply
        // its own material overrides. Keep that continuation intact.
        {0x57206a, "\xe8\x21\xd4\xff\xff", 5, 0xe8, MaterialLoadedHook},
        {0x57195d, "\xc7\x06\x90\x87\x5f\x00", 6, 0xe9, MaterialDestroyedHook}
    };
    for (const auto& patch : patches) {
        if (memcmp(reinterpret_cast<void*>(patch.address), patch.expected, patch.size)) {
            ShaderLog("Material hooks are inactive because the retail code differs.");
            return false;
        }
    }
    LoadConfig();
    configured.try_emplace("windsheild_m");
    LoadDemo();

    DWORD protection[2];
    unsigned writable = 0;
    for (const auto& patch : patches) {
        if (!VirtualProtect(reinterpret_cast<void*>(patch.address), patch.size,
                            PAGE_EXECUTE_READWRITE, &protection[writable])) break;
        ++writable;
    }
    const bool installed = writable == 2;
    if (installed) {
        for (const auto& patch : patches) {
            auto* where = reinterpret_cast<unsigned char*>(patch.address);
            const uint32_t rel = reinterpret_cast<uintptr_t>(patch.replacement) - patch.address - 5;
            where[0] = patch.opcode;
            memcpy(where + 1, &rel, sizeof(rel));
            memset(where + 5, 0x90, patch.size - 5);
            FlushInstructionCache(GetCurrentProcess(), where, patch.size);
        }
        ShaderLog("Installed retail Pure3D material hooks.");
    }
    while (writable) {
        const auto& patch = patches[--writable];
        DWORD ignored;
        VirtualProtect(reinterpret_cast<void*>(patch.address), patch.size, protection[writable], &ignored);
    }
    return installed;
}

const Material* ActiveMaterial() { return active; }
const std::wstring& MaterialDirectory() { return directory; }

void ShaderLog(const char* message, const char* material)
{
    if (!logPath[0]) return;
    const std::string line = std::string("[PBR Shaders] ") + message + material + "\r\n";
    const HANDLE file = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        CloseHandle(file);
    }
}

namespace {
std::vector<uint8_t> ReadTextureFile(const std::wstring& filename)
{
    const auto path = MaterialDirectory() + filename;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    const DWORD length = GetFileSize(file, nullptr);
    if (length < 18 || length > 64u * 1024 * 1024) {
        CloseHandle(file);
        return {};
    }
    std::vector<uint8_t> buffer(length);
    DWORD read;
    const bool success = ReadFile(file, buffer.data(), length, &read, nullptr) && read == length;
    CloseHandle(file);
    return success ? std::move(buffer) : std::vector<uint8_t>{};
}

std::vector<uint8_t> ReadInteriorPreviewTexture(const std::wstring& filename)
{
    unsigned id;
    if (!_wcsicmp(filename.c_str(), L"interior-preview-facade.tga")) id = 102;
    else if (!_wcsicmp(filename.c_str(), L"interior-preview-room.tga")) id = 103;
    else return {};

    HMODULE module;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ReadInteriorPreviewTexture), &module)) return {};
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!resource) return {};
    const auto* data = static_cast<const uint8_t*>(LockResource(LoadResource(module, resource)));
    return data ? std::vector<uint8_t>(data, data + SizeofResource(module, resource)) : std::vector<uint8_t>{};
}
}

Image ReadTarga(const std::wstring& filename)
{
    Image image;
    auto buffer = ReadTextureFile(filename);
    if (buffer.empty()) buffer = ReadInteriorPreviewTexture(filename);
    const auto length = buffer.size();
    if (length < 18 || buffer[1] || buffer[2] != 2) return image;
    const unsigned width = buffer[12] | (buffer[13] << 8);
    const unsigned height = buffer[14] | (buffer[15] << 8);
    const unsigned stride = buffer[16] / 8;
    const unsigned offset = 18 + buffer[0];
    if (!width || !height || width > 4096 || height > 4096 ||
        (buffer[16] != 24 && buffer[16] != 32) ||
        offset + width * height * stride > length) return image;
    image.width = width;
    image.height = height;
    image.pixels.resize(width * height);
    for (unsigned y = 0; y < height; ++y) {
        const unsigned sourceY = buffer[17] & 0x20 ? y : height - 1 - y;
        for (unsigned x = 0; x < width; ++x) {
            const unsigned sourceX = buffer[17] & 0x10 ? width - 1 - x : x;
            const auto* pixel = &buffer[offset + (sourceY * width + sourceX) * stride];
            image.pixels[y * width + x] = pixel[0] | (uint32_t(pixel[1]) << 8) |
                (uint32_t(pixel[2]) << 16) | (uint32_t(stride == 4 ? pixel[3] : 255) << 24);
        }
    }
    return image;
}

EnvironmentMap ReadEnvironment(const std::wstring& filename)
{
    auto data = ReadTextureFile(filename);
    if (data.size() < 128) return {};
    const auto word = [&data](unsigned offset) {
        uint32_t value;
        memcpy(&value, data.data() + offset, sizeof(value));
        return value;
    };
    // DDS, legacy FourCC A16B16G16R16F, one 2D mirror-ball map.
    constexpr uint32_t rgba16f = 113;
    const auto size = word(16);
    const auto levels = word(28);
    if (word(0) != 0x20534444 || word(4) != 124 || word(76) != 32 ||
        !(word(80) & 4) || word(84) != rgba16f || word(112) ||
        !size || size != word(12) || size > 1024 || (size & (size - 1)) ||
        !levels || levels > 11 || !(size >> (levels - 1))) return {};
    size_t used = 128;
    for (unsigned level = 0; level < levels; ++level) {
        const auto side = size >> level;
        used += 8u * side * side;
    }
    if (used != data.size()) return {};
    data.erase(data.begin(), data.begin() + 128);
    return {size, levels, std::move(data)};
}
