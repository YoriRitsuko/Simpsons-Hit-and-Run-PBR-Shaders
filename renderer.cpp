#define WIN32_LEAN_AND_MEAN
#include <d3d9.h>
#include <delayimp.h>

#include "materials.hpp"
#include "com_ptr.hpp"
#include "postfx.hpp"
#include "GlassVertex.inc"
#include "PaintVertex.inc"
#include "GlassPixel.inc"
#include "PaintPixel.inc"
#include "PbrVertex.inc"
#include "PbrPixel.inc"
#include "PbrReflectionPixel.inc"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

static_assert(sizeof(void*) == 4);

namespace {

struct MethodPatch {
    unsigned slot;
    void* hook;
    void** original;
};

template<class T>
MethodPatch Patch(unsigned slot, T hook, T& original)
{
    return {slot, reinterpret_cast<void*>(hook), reinterpret_cast<void**>(&original)};
}

bool HookMethods(void** table, const MethodPatch* patches, unsigned count)
{
    DWORD protection;
    const unsigned base = patches[0].slot;
    const unsigned length = patches[count - 1].slot + 1 - base;
    if (!VirtualProtect(table + base, length * sizeof(void*), PAGE_READWRITE, &protection)) return false;
    for (unsigned index = 0; index < count; ++index) {
        const auto& patch = patches[index];
        *patch.original = table[patch.slot];
        InterlockedExchangePointer(table + patch.slot, patch.hook);
    }
    VirtualProtect(table + base, length * sizeof(void*), protection, &protection);
    return true;
}

uint32_t Colour(float red, float green, float blue)
{
    const auto byte = [](float value) { return uint32_t(std::clamp(value, 0.0f, 1.0f) * 255); };
    return 0xff000000 | (byte(red) << 16) | (byte(green) << 8) | byte(blue);
}

Image DefaultSphere()
{
    constexpr unsigned size = 128;
    Image image{size, size, std::vector<uint32_t>(size * size)};
    for (unsigned y = 0; y < size; ++y) {
        for (unsigned x = 0; x < size; ++x) {
            const float u = float(x) / (size - 1);
            const float v = float(y) / (size - 1);
            const float sky = std::clamp((0.62f - v) * 3.0f, 0.0f, 1.0f);
            const float horizon = 0.50f * std::exp(-std::pow((v - 0.47f) * 13.0f, 2.0f));
            const float glint = 0.65f * std::exp(-std::pow((u - 0.33f) * 6.0f, 2.0f) -
                                                  std::pow((v - 0.27f) * 16.0f, 2.0f));
            image.pixels[y * size + x] = Colour(0.05f + 0.20f * sky + horizon + glint,
                0.07f + 0.37f * sky + horizon + glint, 0.09f + 0.56f * sky + horizon + glint);
        }
    }
    return image;
}

Image GlassSky()
{
    constexpr unsigned width = 256, height = 128;
    constexpr float turn = 6.2831853f;
    constexpr float clouds[][4] = {
        {0.01f, 0.35f, 0.08f, 0.029f}, {0.05f, 0.33f, 0.036f, 0.039f},
        {0.14f, 0.25f, 0.055f, 0.035f}, {0.19f, 0.27f, 0.048f, 0.020f},
        {0.31f, 0.39f, 0.095f, 0.027f}, {0.34f, 0.37f, 0.042f, 0.033f},
        {0.45f, 0.18f, 0.062f, 0.025f}, {0.50f, 0.17f, 0.033f, 0.032f},
        {0.58f, 0.32f, 0.075f, 0.031f}, {0.62f, 0.30f, 0.035f, 0.036f},
        {0.73f, 0.39f, 0.095f, 0.028f}, {0.78f, 0.37f, 0.043f, 0.040f},
        {0.88f, 0.27f, 0.052f, 0.029f}, {0.92f, 0.29f, 0.062f, 0.020f},
        {0.00f, 0.22f, 0.050f, 0.034f}, {0.06f, 0.23f, 0.045f, 0.020f},
        {0.22f, 0.15f, 0.070f, 0.040f}, {0.28f, 0.16f, 0.049f, 0.024f},
        {0.35f, 0.26f, 0.073f, 0.036f}, {0.39f, 0.24f, 0.056f, 0.046f},
        {0.50f, 0.35f, 0.061f, 0.028f}, {0.52f, 0.33f, 0.035f, 0.036f},
        {0.64f, 0.18f, 0.055f, 0.039f}, {0.68f, 0.19f, 0.047f, 0.029f},
        {0.76f, 0.26f, 0.053f, 0.042f}, {0.82f, 0.25f, 0.039f, 0.034f},
        {0.93f, 0.16f, 0.057f, 0.035f}, {0.96f, 0.15f, 0.030f, 0.023f}
    };
    Image image{width, height, std::vector<uint32_t>(width * height)};
    for (unsigned y = 0; y < height; ++y) {
        const float v = (float(y) + 0.5f) / height;
        const float haze = std::clamp(v * 1.7f, 0.0f, 0.85f);
        for (unsigned x = 0; x < width; ++x) {
            const float u = (float(x) + 0.5f) / width;
            const float skyline = 0.48f - 0.016f * std::sin(u * turn * 5) -
                                  0.011f * std::sin(u * turn * 13 + 0.5f);
            float cloud = 0;
            for (const auto& lobe : clouds) {
                const float offset = std::abs(u - lobe[0]);
                const float dx = std::min(offset, 1.0f - offset) / lobe[2];
                const float dy = (v - lobe[1]) / lobe[3];
                cloud = std::max(cloud, std::clamp((1.12f - dx * dx - dy * dy) * 1.4f, 0.0f, 1.0f));
            }
            float red = 0.12f + 0.36f * haze + cloud * 0.60f;
            float green = 0.33f + 0.26f * haze + cloud * 0.46f;
            float blue = 0.64f + 0.12f * haze + cloud * 0.28f;
            const float land = std::clamp((v - skyline) * height, 0.0f, 1.0f);
            red += (0.13f - red) * land;
            green += (0.22f - green) * land;
            blue += (0.19f - blue) * land;
            image.pixels[y * width + x] = Colour(red, green, blue);
        }
    }
    return image;
}

IDirect3DTexture9* Upload(IDirect3DDevice9* device, const Image& image)
{
    if (!image.width) return nullptr;
    ComPtr<IDirect3DTexture9> staging;
    if (FAILED(device->CreateTexture(image.width, image.height, 1, 0, D3DFMT_A8R8G8B8,
                                     D3DPOOL_SYSTEMMEM, staging.Put(), nullptr))) return nullptr;
    D3DLOCKED_RECT lock{};
    if (FAILED(staging.Get()->LockRect(0, &lock, nullptr, 0))) return nullptr;
    for (unsigned y = 0; y < image.height; ++y) {
        memcpy(static_cast<char*>(lock.pBits) + y * lock.Pitch,
               image.pixels.data() + y * image.width, image.width * 4);
    }
    staging.Get()->UnlockRect(0);

    IDirect3DTexture9* texture = nullptr;
    if (FAILED(device->CreateTexture(image.width, image.height, 1, 0, D3DFMT_A8R8G8B8,
                                     D3DPOOL_DEFAULT, &texture, nullptr))) return nullptr;
    if (SUCCEEDED(device->UpdateTexture(staging.Get(), texture))) return texture;
    texture->Release();
    return nullptr;
}

IDirect3DTexture9* UploadEnvironment(IDirect3DDevice9* device, const EnvironmentMap& env)
{
    if (!env.size) return nullptr;
    ComPtr<IDirect3DTexture9> staging;
    if (FAILED(device->CreateTexture(env.size, env.size, env.levels, 0, D3DFMT_A16B16G16R16F,
                                     D3DPOOL_SYSTEMMEM, staging.Put(), nullptr))) return nullptr;
    size_t offset = 0;
    for (unsigned level = 0; level < env.levels; ++level) {
        const unsigned side = env.size >> level;
        const unsigned rowBytes = side * 8;
        D3DLOCKED_RECT lock{};
        if (FAILED(staging.Get()->LockRect(level, &lock, nullptr, 0))) return nullptr;
        for (unsigned row = 0; row < side; ++row) {
            memcpy(static_cast<char*>(lock.pBits) + row * lock.Pitch,
                   env.texels.data() + offset, rowBytes);
            offset += rowBytes;
        }
        staging.Get()->UnlockRect(level);
    }
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(device->CreateTexture(env.size, env.size, env.levels, 0, D3DFMT_A16B16G16R16F,
                                     D3DPOOL_DEFAULT, &texture, nullptr))) return nullptr;
    if (SUCCEEDED(device->UpdateTexture(staging.Get(), texture))) return texture;
    texture->Release();
    return nullptr;
}

enum class BuiltinTexture { Sphere, GlassSky, FlatNormal, White, Black };

struct DeviceResources {
    IDirect3DDevice9* owner = nullptr;
    ComPtr<IDirect3DVertexShader9> glassVertex;
    ComPtr<IDirect3DVertexShader9> paintVertex;
    ComPtr<IDirect3DPixelShader9> glassPixel;
    ComPtr<IDirect3DPixelShader9> paintPixel;
    ComPtr<IDirect3DVertexShader9> pbrVertex;
    ComPtr<IDirect3DPixelShader9> pbrPixel;
    ComPtr<IDirect3DPixelShader9> reflectionPixel;
    std::unordered_map<std::wstring, IDirect3DTexture9*> textures;
    bool loggedGlass = false;
    bool loggedGameSphere = false;
    bool loggedPaint = false;
    bool loggedPbrLights = false;
    bool loggedPbrOpaque = false;
    bool loggedPbrBlend = false;
    bool loggedGameAlbedo = false;
    bool loggedLayout = false;

    void Clear()
    {
        for (auto& entry : textures) {
            if (entry.second) entry.second->Release();
        }
        textures.clear();
        glassVertex.Reset();
        paintVertex.Reset();
        glassPixel.Reset();
        paintPixel.Reset();
        pbrVertex.Reset();
        pbrPixel.Reset();
        reflectionPixel.Reset();
        owner = nullptr;
    }

    IDirect3DTexture9* Texture(IDirect3DDevice9* device, const std::wstring& name, BuiltinTexture fallback)
    {
        constexpr const wchar_t* names[] = {L":sphere", L":glass", L":flat", L":white", L":black"};
        const std::wstring key = name.empty() ? names[static_cast<unsigned>(fallback)] : name;
        const auto found = textures.find(key);
        if (found != textures.end()) return found->second;
        Image image;
        if (!name.empty()) image = ReadTarga(name);
        else {
            switch (fallback) {
                case BuiltinTexture::Sphere: image = DefaultSphere(); break;
                case BuiltinTexture::GlassSky: image = GlassSky(); break;
                case BuiltinTexture::FlatNormal: image = {1, 1, {0xff8080ff}}; break;
                case BuiltinTexture::White: image = {1, 1, {0xffffffff}}; break;
                case BuiltinTexture::Black: image = {1, 1, {0xff000000}}; break;
            }
        }
        auto* texture = Upload(device, image);
        textures.emplace(key, texture);
        if (!texture) ShaderLog("Could not load a configured texture. The stock draw was retained where needed.");
        return texture;
    }

    IDirect3DTexture9* Environment(IDirect3DDevice9* device, const std::wstring& name)
    {
        if (name.empty()) return Texture(device, L"", BuiltinTexture::Sphere);
        const auto found = textures.find(name);
        if (found != textures.end()) return found->second;
        auto* texture = UploadEnvironment(device, ReadEnvironment(name));
        textures.emplace(name, texture);
        if (!texture) ShaderLog("Could not upload the HDR environment texture.");
        return texture;
    }
} resources;

D3DMATRIX Multiply(const D3DMATRIX& left, const D3DMATRIX& right)
{
    D3DMATRIX result{};
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned column = 0; column < 4; ++column) {
            for (unsigned term = 0; term < 4; ++term) {
                result.m[row][column] += left.m[row][term] * right.m[term][column];
            }
        }
    }
    return result;
}

D3DMATRIX SceneView(const D3DMATRIX& fallback)
{
    // The retail PDDI puts model-view in D3DTS_WORLD and leaves D3DTS_VIEW
    // at identity. Pure3D keeps its camera matrix separately in tContext.
    const auto* context = *reinterpret_cast<const uint8_t* const*>(0x65ef70);
    if (!context) return fallback;
    const auto inventory = *reinterpret_cast<void* const*>(context);
    const auto renderContext = *reinterpret_cast<void* const*>(context + 0xec);
    if (inventory != *reinterpret_cast<void* const*>(0x65ef60) ||
        renderContext != *reinterpret_cast<void* const*>(0x65ef5c)) return fallback;
    D3DMATRIX camera;
    memcpy(&camera, context + 0x14, sizeof(camera));
    return camera;
}

constexpr unsigned PbrLightLimit = 8;

struct PbrSettings {
    bool allSceneLights = false;
    unsigned directPercent = 200;
    unsigned environmentPercent = 70;
    PostSettings post;
};

PbrSettings pbrSettings;

void ReadPbrSettings()
{
    const auto hacks = GetModuleHandleW(L"Hacks.dll");
    if (!hacks) return;

    // Lucas 1.27.1 exports its settings lookup by ordinal. Use the default
    // controls if a different build changes the ordinal or value layout.
    const auto* base = reinterpret_cast<const uint8_t*>(hacks);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (pe->Signature != IMAGE_NT_SIGNATURE || pe->FileHeader.TimeDateStamp != 0x69d15f0b ||
        pe->OptionalHeader.SizeOfImage != 0xe8c000) return;

    using FindSetting = const uint8_t* (__cdecl*)(const wchar_t*, const wchar_t*);
    const auto address = GetProcAddress(hacks, MAKEINTRESOURCEA(2560));
    if (reinterpret_cast<const uint8_t*>(address) != base + 0xcbcd0) return;
    const auto find = reinterpret_cast<FindSetting>(reinterpret_cast<uintptr_t>(address));
    const auto read = [find](const wchar_t* name, unsigned fallback, unsigned maximum) {
        const auto* setting = find(L"VehicleShaders", name);
        if (!setting) return fallback;
        // Both multiple-choice indices and integer fields store the value here.
        uint32_t value;
        memcpy(&value, setting + 0x188, sizeof(value));
        return value <= maximum ? value : fallback;
    };
    pbrSettings.allSceneLights = read(L"DirectionalLights", pbrSettings.allSceneLights, 1) == 1;
    pbrSettings.directPercent = read(L"DirectLightPercent", pbrSettings.directPercent, 800);
    pbrSettings.environmentPercent = read(L"EnvironmentPercent", pbrSettings.environmentPercent, 800);
    const auto tick = [find](const wchar_t* name, bool fallback) {
        const auto* setting = find(L"VehicleShaders", name);
        return setting ? setting[0x188] != 0 : fallback;
    };
    auto& post = pbrSettings.post;
    post.linear = tick(L"LinearHDR", post.linear);
    post.bloom = tick(L"HDRBloom", post.bloom);
    post.vignette = tick(L"HDRVignette", post.vignette);
    post.reflections = tick(L"OpaqueSSR", post.reflections);
    post.globalIllumination = tick(L"SSGI", post.globalIllumination);
    post.staticIllumination = tick(L"SSGIStaticWorld", post.staticIllumination);
    post.ambientOcclusion = tick(L"AmbientOcclusion", post.ambientOcclusion);
    post.depthOfField = tick(L"DepthOfField", post.depthOfField);
    post.histogramEqualization = tick(L"HistogramEqualization", post.histogramEqualization);
    post.occlusionOnly = read(L"AOView", post.occlusionOnly, 1) == 1;
    post.toneMapper = static_cast<ToneMapper>(read(L"ToneMapper", static_cast<unsigned>(post.toneMapper), 1));
    post.reflectionSteps = std::max(16u, read(L"SSRSteps", post.reflectionSteps, 128));
    post.reflectionDistance = std::max(8u, read(L"SSRDistance", post.reflectionDistance, 160));
    post.indirectRays = std::max(6u, read(L"SSGIRays", post.indirectRays, 32));
    post.indirectDistance = std::max(1u, read(L"SSGIDistance", post.indirectDistance, 12));
    post.indirectStrength = read(L"SSGIStrength", post.indirectStrength, 200);
    post.aoQuality = read(L"AOQuality", post.aoQuality, 2);
    post.aoRadius = std::max(25u, read(L"AORadius", post.aoRadius, 250));
    post.aoStrength = read(L"AOStrength", post.aoStrength, 200);
    post.exposure = std::max(25u, read(L"HDRExposure", post.exposure, 400));
    post.skyBrightness = std::max(25u, read(L"SkyBrightness", post.skyBrightness, 400));
    post.bloomStrength = read(L"HDRBloomStrength", post.bloomStrength, 100);
    post.bloomStreakMix = read(L"HDRBloomStreakMix", post.bloomStreakMix, 100);
    post.vignetteStrength = read(L"HDRVignetteStrength", post.vignetteStrength, 100);
    post.focusDistance = std::max(1u, read(L"DOFFocusDistance", post.focusDistance, 100));
    post.focusRadius = std::max(2u, read(L"DOFBlurRadius", post.focusRadius, 24));
    post.histogramStrength = read(L"HistogramStrength", post.histogramStrength, 100);
}

struct PbrLights {
    float directions[PbrLightLimit][4]{};
    float colors[PbrLightLimit][4]{};
    unsigned count = 0;
    bool fallback = false;
};

PbrLights ReadPbrLights(IDirect3DDevice9* device, const D3DMATRIX& camera, float strength)
{
    PbrLights lights;
    // Fixed-function PDDI colors were used with cosine diffuse. Convert to
    // radiance so the PBR shader's Lambert 1/pi keeps the expected energy.
    constexpr float pi = 3.14159265f;
    const float gain = pi * strength;
    float strongest = 0;
    for (DWORD index = 0; index < PbrLightLimit; ++index) {
        BOOL active = FALSE;
        D3DLIGHT9 native{};
        if (FAILED(device->GetLightEnable(index, &active)) || !active ||
            FAILED(device->GetLight(index, &native)) || native.Type != D3DLIGHT_DIRECTIONAL) continue;
        const auto& ray = native.Direction;
        const float length = std::sqrt(ray.x*ray.x + ray.y*ray.y + ray.z*ray.z);
        const auto& diffuse = native.Diffuse;
        const float luminance = 0.2126f * diffuse.r + 0.7152f * diffuse.g + 0.0722f * diffuse.b;
        if (length < 0.0001f || luminance <= 0) continue;
        if (!pbrSettings.allSceneLights) {
            if (luminance <= strongest) continue;
            strongest = luminance;
            lights.count = 0;
        }
        auto& direction = lights.directions[lights.count];
        auto& color = lights.colors[lights.count++];
        // PDDI already submitted the ray in view space. Reverse it to get
        // the direction from the surface toward the light.
        direction[0] = -ray.x / length;
        direction[1] = -ray.y / length;
        direction[2] = -ray.z / length;
        color[0] = gain * diffuse.r;
        color[1] = gain * diffuse.g;
        color[2] = gain * diffuse.b;
    }
    if (lights.count) return lights;

    // Keep the fallback in world space as the player moves the camera.
    constexpr float key[] = {-0.44f, 0.72f, 0.54f};
    const float length = std::sqrt(key[0]*key[0] + key[1]*key[1] + key[2]*key[2]);
    for (unsigned axis = 0; axis < 3; ++axis) {
        lights.directions[0][axis] = (key[0] * camera.m[0][axis] +
                                      key[1] * camera.m[1][axis] +
                                      key[2] * camera.m[2][axis]) / length;
        lights.colors[0][axis] = strength;
    }
    lights.count = 1;
    lights.fallback = true;
    return lights;
}

bool BuildDeclaration(IDirect3DDevice9* device, const Material& material,
                      ComPtr<IDirect3DVertexDeclaration9>& output)
{
    ComPtr<IDirect3DVertexDeclaration9> original;
    if (FAILED(device->GetVertexDeclaration(original.Put())) || !original.Get()) return false;
    std::array<D3DVERTEXELEMENT9, MAXD3DDECLLENGTH + 1> elements{};
    UINT size = static_cast<UINT>(elements.size());
    if (FAILED(original.Get()->GetDeclaration(elements.data(), &size))) return false;
    std::array<D3DVERTEXELEMENT9, 5> required{};
    unsigned count = 0, fields = 0;
    const bool paint = material.mode == MaterialMode::Paint;
    for (unsigned index = 0; index < size && elements[index].Stream != 0xff; ++index) {
        auto element = elements[index];
        if (element.Method != D3DDECLMETHOD_DEFAULT || element.Usage == D3DDECLUSAGE_POSITIONT ||
            element.Usage == D3DDECLUSAGE_BLENDWEIGHT || element.Usage == D3DDECLUSAGE_BLENDINDICES) return false;

        unsigned field = 0;
        if (element.UsageIndex == 0 && element.Usage == D3DDECLUSAGE_POSITION &&
            (element.Type == D3DDECLTYPE_FLOAT3 || element.Type == D3DDECLTYPE_FLOAT4)) field = 1;
        if (element.UsageIndex == 0 && element.Usage == D3DDECLUSAGE_NORMAL) field = 2;
        if (element.UsageIndex == 0 && element.Usage == D3DDECLUSAGE_TEXCOORD) field = 4;
        if (paint && element.UsageIndex == material.maskSet && element.Usage == D3DDECLUSAGE_COLOR &&
            (element.Type == D3DDECLTYPE_D3DCOLOR || element.Type == D3DDECLTYPE_FLOAT4 ||
             element.Type == D3DDECLTYPE_UBYTE4N)) {
            element.UsageIndex = 0;
            field = 8;
        }
        if (field && !(fields & field)) {
            required[count++] = element;
            fields |= field;
        }
    }
    if (fields != (paint ? 15u : 7u)) {
        if (!resources.loggedLayout) {
            resources.loggedLayout = true;
            ShaderLog("Material had no compatible position, normal, UV or paint mask stream.");
        }
        return false;
    }
    required[count] = D3DDECL_END();
    return SUCCEEDED(device->CreateVertexDeclaration(required.data(), output.Put()));
}

void ReadGameSphere(IDirect3DDevice9* device, const Material& material,
                    ComPtr<IDirect3DBaseTexture9>& texture)
{
    DWORD stage1 = D3DTOP_DISABLE;
    if (material.useGameSphere &&
        SUCCEEDED(device->GetTextureStageState(1, D3DTSS_COLOROP, &stage1)) &&
        stage1 != D3DTOP_DISABLE) device->GetTexture(1, texture.Put());
    if (texture.Get() && texture.Get()->GetType() != D3DRTYPE_TEXTURE) texture.Reset();
}

float DecodeSrgb(float value)
{
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

DWORD DecodeColor(DWORD color)
{
    DWORD result = color & 0xff000000;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        const float channel = float((color >> shift) & 255) / 255;
        result |= DWORD(DecodeSrgb(channel) * 255 + 0.5f) << shift;
    }
    return result;
}

class LinearStockState {
public:
    explicit LinearStockState(IDirect3DDevice9* device) : device(device)
    {
        for (DWORD i = 0; i < samplerModes.size(); ++i) {
            if (SUCCEEDED(device->GetSamplerState(i, D3DSAMP_SRGBTEXTURE, &samplerModes[i])))
                device->SetSamplerState(i, D3DSAMP_SRGBTEXTURE, TRUE);
        }
        for (auto& value : states) {
            value.saved = SUCCEEDED(device->GetRenderState(value.state, &value.original));
            if (value.saved) device->SetRenderState(value.state, value.state == D3DRS_SRGBWRITEENABLE
                                                    ? FALSE : DecodeColor(value.original));
        }
        DWORD lighting = FALSE;
        if (SUCCEEDED(device->GetRenderState(D3DRS_LIGHTING, &lighting)) && lighting &&
            SUCCEEDED(device->GetMaterial(&material))) {
            auto linear = material;
            for (auto* color : {&linear.Diffuse, &linear.Ambient, &linear.Emissive, &linear.Specular}) {
                color->r = DecodeSrgb(color->r);
                color->g = DecodeSrgb(color->g);
                color->b = DecodeSrgb(color->b);
            }
            savedMaterial = SUCCEEDED(device->SetMaterial(&linear));
        }
    }

    ~LinearStockState()
    {
        if (savedMaterial) device->SetMaterial(&material);
        for (const auto& value : states) {
            if (value.saved) device->SetRenderState(value.state, value.original);
        }
        for (DWORD i = 0; i < samplerModes.size(); ++i) {
            if (samplerModes[i] != invalid) device->SetSamplerState(i, D3DSAMP_SRGBTEXTURE, samplerModes[i]);
        }
    }

private:
    struct State {
        D3DRENDERSTATETYPE state;
        DWORD original = 0;
        bool saved = false;
    };
    static constexpr DWORD invalid = ~DWORD(0);
    IDirect3DDevice9* device;
    std::array<DWORD, 4> samplerModes{invalid, invalid, invalid, invalid};
    State states[3]{{D3DRS_SRGBWRITEENABLE}, {D3DRS_FOGCOLOR}, {D3DRS_AMBIENT}};
    D3DMATERIAL9 material{};
    bool savedMaterial = false;
};

class ReflectorCoverage {
public:
    explicit ReflectorCoverage(IDirect3DDevice9* device) : device(device)
    {
        if (!TracksWorldCoverage(device)) return;
        DWORD blend, source, destination, operation;
        if (FAILED(device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend)) || !blend ||
            FAILED(device->GetRenderState(D3DRS_COLORWRITEENABLE, &writeMask)) ||
            !(writeMask & D3DCOLORWRITEENABLE_ALPHA) ||
            FAILED(device->GetRenderState(D3DRS_SRCBLEND, &source)) ||
            FAILED(device->GetRenderState(D3DRS_DESTBLEND, &destination)) ||
            FAILED(device->GetRenderState(D3DRS_BLENDOP, &operation))) return;

        if (operation != D3DBLENDOP_ADD || destination != D3DBLEND_INVSRCALPHA ||
            (source != D3DBLEND_ONE && source != D3DBLEND_SRCALPHA)) {
            // Glows, shadows, and other blend modes leave the coverage alone.
            changedMask = SUCCEEDED(device->SetRenderState(D3DRS_COLORWRITEENABLE,
                                        writeMask & ~D3DCOLORWRITEENABLE_ALPHA));
            return;
        }

        for (unsigned i = 0; i < states.size(); ++i) {
            if (FAILED(device->GetRenderState(states[i], &original[i]))) return;
        }
        changedBlend = true;
        // Opaque receivers start their alpha at one. Each layer of normal
        // alpha blending leaves only the uncovered fraction of the SSR.
        constexpr std::array<DWORD, 4> coverage{TRUE, D3DBLEND_ZERO, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD};
        for (unsigned i = 0; i < states.size(); ++i) device->SetRenderState(states[i], coverage[i]);
    }

    ~ReflectorCoverage()
    {
        if (changedMask) device->SetRenderState(D3DRS_COLORWRITEENABLE, writeMask);
        if (changedBlend) {
            for (unsigned i = 0; i < states.size(); ++i) device->SetRenderState(states[i], original[i]);
        }
    }

private:
    static constexpr std::array<D3DRENDERSTATETYPE, 4> states{
        D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA};
    IDirect3DDevice9* device;
    std::array<DWORD, 4> original{};
    DWORD writeMask = 0;
    bool changedMask = false;
    bool changedBlend = false;
};

class EffectState {
public:
    bool ApplyGlassBase(IDirect3DDevice9* device, const Material& material)
    {
        ComPtr<IDirect3DBaseTexture9> sphere;
        ReadGameSphere(device, material, sphere);
        ComPtr<IDirect3DBaseTexture9> base;
        device->GetTexture(0, base.Put());
        if (!sphere.Get() || !base.Get() || !Save(device)) return false;

        // The fixed-function spheremap combiner can feed EnvMap's alpha into
        // blending. Draw the glass texture by itself before adding the reflection.
        return SUCCEEDED(device->SetPixelShader(nullptr)) &&
               SUCCEEDED(device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE)) &&
               SUCCEEDED(device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE)) &&
               SUCCEEDED(device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE)) &&
               SUCCEEDED(device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1)) &&
               SUCCEEDED(device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE)) &&
               SUCCEEDED(device->SetTextureStageState(0, D3DTSS_RESULTARG, D3DTA_CURRENT)) &&
               SUCCEEDED(device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE)) &&
               SUCCEEDED(device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE)) &&
               SUCCEEDED(device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE)) &&
               SUCCEEDED(device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA)) &&
               SUCCEEDED(device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA)) &&
               SUCCEEDED(device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD)) &&
               SUCCEEDED(device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE)) &&
               SUCCEEDED(device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE));
    }

    bool Apply(IDirect3DDevice9* device, const Material& material)
    {
        const bool glass = material.mode == MaterialMode::Glass;
        DWORD alphaBlend = FALSE;
        DWORD blend = D3DVBF_DISABLE;
        if (FAILED(device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend)) ||
            bool(alphaBlend) != glass ||
            FAILED(device->GetRenderState(D3DRS_VERTEXBLEND, &blend)) || blend != D3DVBF_DISABLE) return false;

        if (resources.owner != device) {
            resources.Clear();
            resources.owner = device;
        }
        auto& vertex = glass ? resources.glassVertex : resources.paintVertex;
        auto& pixel = glass ? resources.glassPixel : resources.paintPixel;
        const auto* vertexCode = reinterpret_cast<const DWORD*>(glass ? GlassVertexCode : PaintVertexCode);
        const auto* pixelCode = reinterpret_cast<const DWORD*>(glass ? GlassPixelCode : PaintPixelCode);
        if (!vertex.Get() && FAILED(device->CreateVertexShader(vertexCode, vertex.Put()))) return false;
        if (!pixel.Get() && FAILED(device->CreatePixelShader(pixelCode, pixel.Put()))) return false;

        ComPtr<IDirect3DVertexDeclaration9> declaration;
        if (!BuildDeclaration(device, material, declaration)) return false;
        ComPtr<IDirect3DBaseTexture9> albedo;
        ComPtr<IDirect3DBaseTexture9> gameSphere;
        device->GetTexture(0, albedo.Put());
        if (glass) ReadGameSphere(device, material, gameSphere);
        const bool worldSky = glass && !gameSphere.Get() && material.sphereMap.empty();
        auto* sphere = gameSphere.Get() ? gameSphere.Get() :
                       resources.Texture(device, material.sphereMap,
                                         glass ? BuiltinTexture::GlassSky : BuiltinTexture::Sphere);
        auto* bump = glass ? nullptr : resources.Texture(device, material.normalMap, BuiltinTexture::FlatNormal);
        if (!sphere || sphere->GetType() != D3DRTYPE_TEXTURE ||
            (!glass && (!albedo.Get() || !bump))) return false;

        D3DMATRIX world{}, view{}, projection{};
        if (FAILED(device->GetTransform(D3DTS_WORLD, &world)) ||
            FAILED(device->GetTransform(D3DTS_VIEW, &view)) ||
            FAILED(device->GetTransform(D3DTS_PROJECTION, &projection))) return false;
        const auto worldView = Multiply(world, view);
        const auto clip = Multiply(worldView, projection);
        D3DMATERIAL9 stockMaterial{};
        const float opacity = SUCCEEDED(device->GetMaterial(&stockMaterial)) ? stockMaterial.Diffuse.a : 1;
        float light[4] = {-0.44f, 0.72f, -0.54f, material.normalStrength};
        for (DWORD index = 0; !glass && index < 8; ++index) {
            BOOL active = FALSE;
            D3DLIGHT9 native{};
            if (FAILED(device->GetLightEnable(index, &active)) || !active ||
                FAILED(device->GetLight(index, &native)) || native.Type != D3DLIGHT_DIRECTIONAL) continue;
            for (unsigned axis = 0; axis < 3; ++axis) {
                light[axis] = -(native.Direction.x * view.m[0][axis] +
                                native.Direction.y * view.m[1][axis] +
                                native.Direction.z * view.m[2][axis]);
            }
            const float length = std::sqrt(light[0]*light[0] + light[1]*light[1] + light[2]*light[2]);
            if (length > 0.0001f) for (unsigned axis = 0; axis < 3; ++axis) light[axis] /= length;
            break;
        }
        const float attributes[4] = {material.paint[0], material.paint[1], material.paint[2],
                                     material.reflection};
        DWORD alphaTest = FALSE;
        if (glass) device->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
        const float parameters[4] = {albedo.Get() ? 1.0f : 0.0f, opacity,
                                     gameSphere.Get() ? 2.0f : worldSky ? 1.0f : 0.0f,
                                     alphaTest ? 1.0f : 0.0f};
        if (!Save(device)) return false;
        if (FAILED(device->SetVertexDeclaration(declaration.Get())) ||
            FAILED(device->SetVertexShader(vertex.Get())) ||
            FAILED(device->SetPixelShader(pixel.Get())) ||
            FAILED(device->SetVertexShaderConstantF(0, &clip.m[0][0], 4)) ||
            FAILED(device->SetVertexShaderConstantF(4, &worldView.m[0][0], 4)) ||
            FAILED(device->SetPixelShaderConstantF(0, attributes, 1)) ||
            FAILED(device->SetPixelShaderConstantF(1, light, 1)) ||
            FAILED(device->SetPixelShaderConstantF(2, parameters, 1)) ||
            FAILED(device->SetTexture(2, sphere))) return false;

        if (gameSphere.Get() && !resources.loggedGameSphere) {
            resources.loggedGameSphere = true;
            ShaderLog("Using the game's bound spheremap for glass.");
        }
        SetSampling(device, 2, D3DTADDRESS_CLAMP);
        if (glass) {
            if (worldSky) {
                if (!SetViewRotation(device, SceneView(view))) return false;
                device->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
                device->SetSamplerState(2, D3DSAMP_SRGBTEXTURE, FALSE);
            }
            DWORD colorWrite = 0;
            device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
            device->SetRenderState(D3DRS_COLORWRITEENABLE, colorWrite & 7);
            device->SetRenderState(D3DRS_SRCBLEND, alphaTest ? D3DBLEND_ONE : D3DBLEND_SRCALPHA);
            device->SetRenderState(D3DRS_DESTBLEND, alphaTest ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
            device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
            device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
            device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            DWORD compare = 0;
            if (SUCCEEDED(device->GetRenderState(D3DRS_ZFUNC, &compare)) && compare == D3DCMP_LESS) {
                device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            }
        } else {
            if (FAILED(device->SetTexture(1, bump))) return false;
            SetSampling(device, 1, D3DTADDRESS_WRAP);
            device->SetSamplerState(1, D3DSAMP_SRGBTEXTURE, FALSE);
        }
        const bool linear = LinearWorld(device);
        const float colorSpace[4] = {linear ? 1.0f : 0.0f, 0, 0, 0};
        device->SetPixelShaderConstantF(25, colorSpace, 1);
        if (linear) {
            device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, TRUE);
            device->SetSamplerState(2, D3DSAMP_SRGBTEXTURE, TRUE);
            device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        }
        return true;
    }

    bool ApplyPbr(IDirect3DDevice9* device, const Material& material)
    {
        DWORD blend;
        if (FAILED(device->GetRenderState(D3DRS_VERTEXBLEND, &blend)) || blend != D3DVBF_DISABLE) return false;
        if (resources.owner != device) {
            resources.Clear();
            resources.owner = device;
        }
        if (!resources.pbrVertex.Get() && FAILED(device->CreateVertexShader(
                reinterpret_cast<const DWORD*>(PbrVertexCode), resources.pbrVertex.Put()))) return false;
        if (!resources.pbrPixel.Get() && FAILED(device->CreatePixelShader(
                reinterpret_cast<const DWORD*>(PbrPixelCode), resources.pbrPixel.Put()))) return false;

        ComPtr<IDirect3DVertexDeclaration9> declaration;
        if (!BuildDeclaration(device, material, declaration)) return false;
        ComPtr<IDirect3DBaseTexture9> gameAlbedo;
        if (material.baseColor.empty()) device->GetTexture(0, gameAlbedo.Put());
        auto* albedo = gameAlbedo.Get() ? gameAlbedo.Get() :
                       resources.Texture(device, material.baseColor, BuiltinTexture::White);
        auto* normal = resources.Texture(device, material.normalMap, BuiltinTexture::FlatNormal);
        auto* environment = resources.Environment(device, material.environment);
        auto* orm = resources.Texture(device, material.orm, BuiltinTexture::White);
        auto* emission = resources.Texture(device, material.emissive, BuiltinTexture::Black);
        if (!albedo || !normal || !environment || !orm || !emission) return false;
        D3DSURFACE_DESC environmentDesc{};
        if (FAILED(environment->GetLevelDesc(0, &environmentDesc))) return false;

        D3DMATRIX world{}, view{}, projection{};
        if (FAILED(device->GetTransform(D3DTS_WORLD, &world)) ||
            FAILED(device->GetTransform(D3DTS_VIEW, &view)) ||
            FAILED(device->GetTransform(D3DTS_PROJECTION, &projection))) return false;
        const auto worldView = Multiply(world, view);
        const auto clip = Multiply(worldView, projection);
        const auto camera = SceneView(view);
        const float directStrength = material.directLightStrength * pbrSettings.directPercent / 100.0f;
        const float environmentStrength = material.environmentStrength * pbrSettings.environmentPercent / 100.0f;
        const auto lights = ReadPbrLights(device, camera, directStrength);
        const bool linear = LinearWorld(device);
        const float normalScale[4] = {0, 0, 0, material.normalStrength};
        const float lighting[4] = {float(environment->GetLevelCount() - 1), float(lights.count),
                                   environmentStrength, linear ? 1.0f : 0.0f};
        const float sampling[2][4] = {
            {linear ? 1.0f : 0.0f, linear && material.environment.empty() ? 1.0f : 0.0f, 0, 0},
            {float(environmentDesc.Width), float(environmentDesc.Height), 0, 0},
        };
        const float surface[4] = {material.opacity, material.alphaCutoff,
                                  static_cast<float>(material.alphaMode), material.orm.empty() ? 0.0f : 1.0f};
        const float options[4] = {material.roughness, material.metallic, material.exposure,
                                  material.emissiveStrength};
        if (!Save(device) ||
            FAILED(device->SetVertexDeclaration(declaration.Get())) ||
            FAILED(device->SetVertexShader(resources.pbrVertex.Get())) ||
            FAILED(device->SetPixelShader(resources.pbrPixel.Get())) ||
            FAILED(device->SetVertexShaderConstantF(0, &clip.m[0][0], 4)) ||
            FAILED(device->SetVertexShaderConstantF(4, &worldView.m[0][0], 4)) ||
            FAILED(device->SetPixelShaderConstantF(1, normalScale, 1)) || !SetViewRotation(device, camera) ||
            FAILED(device->SetPixelShaderConstantF(6, surface, 1)) ||
            FAILED(device->SetPixelShaderConstantF(7, options, 1)) ||
            FAILED(device->SetPixelShaderConstantF(8, lighting, 1)) ||
            FAILED(device->SetPixelShaderConstantF(9, &lights.directions[0][0], PbrLightLimit)) ||
            FAILED(device->SetPixelShaderConstantF(17, &lights.colors[0][0], PbrLightLimit)) ||
            FAILED(device->SetPixelShaderConstantF(25, &sampling[0][0], 2))) return false;

        if (!resources.loggedPbrLights) {
            resources.loggedPbrLights = true;
            if (lights.fallback) ShaderLog("PBR uses a fallback world-space key light.");
            else ShaderLog("PBR directional lights submitted: ", std::to_string(lights.count).c_str());
        }

        IDirect3DBaseTexture9* textures[] = {albedo, normal, environment, orm, emission};
        for (unsigned slot = 0; slot < std::size(textures); ++slot) {
            if (FAILED(device->SetTexture(slot, textures[slot])) ||
                FAILED(device->SetSamplerState(slot, D3DSAMP_SRGBTEXTURE, FALSE))) return false;
            SetSampling(device, slot, slot == 2 ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
        }
        if (environment->GetLevelCount() > 1) device->SetSamplerState(2, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        const bool transparent = material.alphaMode == AlphaMode::Blend;
        if (transparent &&
            (FAILED(device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE)) ||
             FAILED(device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA)) ||
             FAILED(device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA)) ||
             FAILED(device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD)) ||
             FAILED(device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE)))) return false;
        if (FAILED(device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE)) ||
            FAILED(device->SetRenderState(D3DRS_ALPHABLENDENABLE, transparent)) ||
            FAILED(device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE))) return false;
        if (linear && !transparent && BindScreenMaterialOutputs(device, projection)) {
            if ((!resources.reflectionPixel.Get() && FAILED(device->CreatePixelShader(
                    reinterpret_cast<const DWORD*>(PbrReflectionPixelCode), resources.reflectionPixel.Put()))) ||
                FAILED(device->SetPixelShader(resources.reflectionPixel.Get()))) {
                UnbindScreenMaterialOutputs(device, false);
            } else {
                reflectionDevice = device;
                DWORD writeMask;
                if (SUCCEEDED(device->GetRenderState(D3DRS_COLORWRITEENABLE, &writeMask)))
                    device->SetRenderState(D3DRS_COLORWRITEENABLE, writeMask | D3DCOLORWRITEENABLE_ALPHA);
            }
        }
        return true;
    }

    void Complete(HRESULT result)
    {
        if (!reflectionDevice) return;
        UnbindScreenMaterialOutputs(reflectionDevice, SUCCEEDED(result));
        reflectionDevice = nullptr;
    }

    void Restore()
    {
        Complete(D3DERR_INVALIDCALL);
        if (saved.Get()) saved.Get()->Apply();
        saved.Reset();
    }

    ~EffectState() { Restore(); }

private:
    static bool SetViewRotation(IDirect3DDevice9* device, const D3DMATRIX& view)
    {
        // Transpose the camera rotation to take a view-space ray into the world.
        const float worldFromView[3][4] = {
            {view._11, view._21, view._31, 0},
            {view._12, view._22, view._32, 0},
            {view._13, view._23, view._33, 0}
        };
        return SUCCEEDED(device->SetPixelShaderConstantF(3, &worldFromView[0][0], 3));
    }

    bool Save(IDirect3DDevice9* device)
    {
        if (FAILED(device->CreateStateBlock(D3DSBT_ALL, saved.Put())) ||
            FAILED(saved.Get()->Capture())) {
            saved.Reset();
            return false;
        }
        return true;
    }

    static void SetSampling(IDirect3DDevice9* device, DWORD sampler, DWORD address)
    {
        device->SetSamplerState(sampler, D3DSAMP_ADDRESSU, address);
        device->SetSamplerState(sampler, D3DSAMP_ADDRESSV, address);
        device->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    }

    ComPtr<IDirect3DStateBlock9> saved;
    IDirect3DDevice9* reflectionDevice = nullptr;
};

template<class Draw>
HRESULT Render(IDirect3DDevice9* device, Draw draw)
{
    const auto pass = [&](bool captureStatic = true) {
        CaptureWorldProjection(device);
        ReflectorCoverage coverage(device);
        const HRESULT result = draw();
        if (captureStatic && SUCCEEDED(result) && BeginStaticDepthDraw(device)) {
            EndStaticDepthDraw(device, SUCCEEDED(draw()));
        }
        return result;
    };
    const auto stock = [&] {
        if (!LinearWorld(device)) return draw();
        LinearStockState linear(device);
        return pass();
    };
    const Material* material = ActiveMaterial();
    if (!material) return stock();
    const bool glass = material->mode == MaterialMode::Glass;
    EffectState effect;
    HRESULT original = D3D_OK;
    if (glass) {
        const bool separateBase = effect.ApplyGlassBase(device, *material);
        if (!separateBase) effect.Restore();
        original = stock();
        effect.Restore();
        if (FAILED(original) || material->reflection == 0) return original;
    }
    const bool pbr = material->mode == MaterialMode::Pbr;
    if (!(pbr ? effect.ApplyPbr(device, *material) : effect.Apply(device, *material))) {
        effect.Restore();
        return glass ? original : stock();
    }
    const HRESULT rendered = pass(!pbr);
    effect.Complete(rendered);
    effect.Restore();
    if (FAILED(rendered)) return glass ? original : stock();
    if (pbr) {
        if (material->baseColor.empty() && !resources.loggedGameAlbedo) {
            resources.loggedGameAlbedo = true;
            ShaderLog("Rendered PBR with the stock material texture binding.");
        }
        const bool transparent = material->alphaMode == AlphaMode::Blend;
        bool& logged = transparent ? resources.loggedPbrBlend : resources.loggedPbrOpaque;
        if (!logged) {
            logged = true;
            ShaderLog(transparent ? "Rendered translucent PBR material." : "Rendered opaque PBR material.");
        }
        return rendered;
    }
    bool& logged = glass ? resources.loggedGlass : resources.loggedPaint;
    if (!logged) {
        logged = true;
        ShaderLog(glass ? "Rendered transparent sphere reflection." : "Rendered normal-mapped paint with vertex masks.");
    }
    return rendered;
}

struct DrawMethods {
    void** table = nullptr;
    using ResetCall = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    using Scene = HRESULT (WINAPI*)(IDirect3DDevice9*);
    using ClearCall = HRESULT (WINAPI*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
    using Primitive = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
    using Indexed = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    using PrimitiveUP = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
    using IndexedUP = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT,
                                       const void*, D3DFORMAT, const void*, UINT);
    ResetCall resetCall = nullptr;
    Scene beginScene = nullptr;
    Scene endScene = nullptr;
    ClearCall clear = nullptr;
    Primitive primitive = nullptr;
    Indexed indexed = nullptr;
    PrimitiveUP primitiveUP = nullptr;
    IndexedUP indexedUP = nullptr;
};
DrawMethods devices[2];

template<class Interface>
void** Vtable(Interface* object) { return *reinterpret_cast<void***>(object); }

DrawMethods& DeviceMethods(IDirect3DDevice9* device)
{
    return devices[devices[0].table == Vtable(device) ? 0 : 1];
}

HRESULT WINAPI Reset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* p)
{
    PostReset(d);
    if (resources.owner == d) resources.Clear();
    return DeviceMethods(d).resetCall(d, p);
}

HRESULT WINAPI BeginScene(IDirect3DDevice9* d)
{
    const auto result = DeviceMethods(d).beginScene(d);
    if (SUCCEEDED(result)) PostBeginScene(d);
    return result;
}

HRESULT WINAPI EndScene(IDirect3DDevice9* d)
{
    PostEndScene(d);
    return DeviceMethods(d).endScene(d);
}

HRESULT WINAPI Clear(IDirect3DDevice9* d, DWORD count, const D3DRECT* rects, DWORD flags,
                     D3DCOLOR color, float depth, DWORD stencil)
{
    if ((flags & D3DCLEAR_TARGET) && LinearWorld(d)) color = DecodeColor(color);
    return DeviceMethods(d).clear(d, count, rects, flags, color, depth, stencil);
}

HRESULT WINAPI DrawPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, UINT first, UINT count)
{
    return Render(d, [&] { return DeviceMethods(d).primitive(d, type, first, count); });
}

HRESULT WINAPI DrawIndexed(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, INT base, UINT minimum,
                           UINT vertices, UINT start, UINT count)
{
    return Render(d, [&] { return DeviceMethods(d).indexed(d, type, base, minimum, vertices, start, count); });
}

HRESULT WINAPI DrawPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, UINT count,
                               const void* vertices, UINT stride)
{
    return Render(d, [&] { return DeviceMethods(d).primitiveUP(d, type, count, vertices, stride); });
}

HRESULT WINAPI DrawIndexedUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE type, UINT minimum, UINT vertices,
                             UINT count, const void* indices, D3DFORMAT format, const void* data, UINT stride)
{
    return Render(d, [&] { return DeviceMethods(d).indexedUP(d, type, minimum, vertices, count,
                                                            indices, format, data, stride); });
}

void InstallDevice(IDirect3DDevice9* device)
{
    if (!device) return;
    auto* table = Vtable(device);
    for (auto& methods : devices) {
        if (methods.table == table) return;
        if (methods.table) continue;
        const MethodPatch patches[] = {
            Patch(16, Reset, methods.resetCall),
            Patch(41, BeginScene, methods.beginScene),
            Patch(42, EndScene, methods.endScene),
            Patch(43, Clear, methods.clear),
            Patch(81, DrawPrimitive, methods.primitive),
            Patch(82, DrawIndexed, methods.indexed),
            Patch(83, DrawPrimitiveUP, methods.primitiveUP),
            Patch(84, DrawIndexedUP, methods.indexedUP)
        };
        methods.table = table;
        if (HookMethods(table, patches, std::size(patches))) {
            ShaderLog("Connected to the Direct3D 9 draw calls.");
        } else {
            methods.table = nullptr;
        }
        return;
    }
}

struct FactoryMethods {
    using Create = HRESULT (WINAPI*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                     D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
    void** table = nullptr;
    Create create = nullptr;
};
FactoryMethods factories[2];

FactoryMethods& Factory(IDirect3D9* object)
{
    return factories[factories[0].table == Vtable(object) ? 0 : 1];
}

HRESULT WINAPI CreateDevice(IDirect3D9* factory, UINT adapter, D3DDEVTYPE type, HWND window,
                            DWORD flags, D3DPRESENT_PARAMETERS* p, IDirect3DDevice9** device)
{
    const HRESULT result = Factory(factory).create(factory, adapter, type, window, flags, p, device);
    if (SUCCEEDED(result) && device) InstallDevice(*device);
    return result;
}

void InstallFactory(IDirect3D9* object)
{
    if (!object) return;
    auto* table = Vtable(object);
    for (auto& methods : factories) {
        if (methods.table == table) return;
        if (methods.table) continue;
        const auto patch = Patch(16, CreateDevice, methods.create);
        methods.table = table;
        if (!HookMethods(table, &patch, 1)) methods.table = nullptr;
        return;
    }
}

using Direct3DFactory = IDirect3D9* (WINAPI*)(UINT);
Direct3DFactory createFactory;
void** factorySlot;

IDirect3D9* WINAPI CreateFactory(UINT sdkVersion)
{
    auto* object = createFactory(sdkVersion);
    // The delay loader may replace the slot with the resolved address.
    void* entry = *factorySlot;
    if (entry && entry != reinterpret_cast<void*>(&CreateFactory)) {
        DWORD protection;
        if (VirtualProtect(factorySlot, sizeof(*factorySlot), PAGE_READWRITE, &protection)) {
            createFactory = reinterpret_cast<Direct3DFactory>(entry);
            InterlockedExchangePointer(factorySlot, reinterpret_cast<void*>(&CreateFactory));
            DWORD ignored;
            VirtualProtect(factorySlot, sizeof(*factorySlot), protection, &ignored);
        }
    }
    InstallFactory(object);
    return object;
}

// Lucas's Direct3D 9 hack owns the factory call. Follow that one import so
// the Windows loader and any existing graphics wrapper keep doing their jobs.
void** LauncherFactoryImport()
{
    auto* image = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"Hacks.dll"));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < static_cast<LONG>(sizeof(*dos)) ||
        dos->e_lfanew > 0x1000) return nullptr;
    const auto* header = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
    if (header->Signature != IMAGE_NT_SIGNATURE ||
        header->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return nullptr;
    const auto imageSize = header->OptionalHeader.SizeOfImage;
    const auto inImage = [imageSize](DWORD rva, size_t length) {
        return rva && rva < imageSize && length <= imageSize - rva;
    };
    const auto& imports = header->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!inImage(imports.VirtualAddress, imports.Size)) return nullptr;
    const auto* descriptors = reinterpret_cast<const ImgDelayDescr*>(image + imports.VirtualAddress);
    constexpr char library[] = "d3d9.dll";
    constexpr char function[] = "Direct3DCreate9";
    for (unsigned i = 0; i < imports.Size / sizeof(*descriptors) && descriptors[i].rvaDLLName; ++i) {
        const auto& entry = descriptors[i];
        if (!(entry.grAttrs & dlattrRva) || !inImage(entry.rvaDLLName, sizeof(library)) ||
            _strnicmp(reinterpret_cast<const char*>(image + entry.rvaDLLName), library, sizeof(library))) continue;
        const auto lookup = entry.rvaINT;
        const auto address = entry.rvaIAT;
        if (!inImage(lookup, sizeof(IMAGE_THUNK_DATA32)) ||
            !inImage(address, sizeof(IMAGE_THUNK_DATA32))) return nullptr;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA32*>(image + lookup);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA32*>(image + address);
        const auto count = (imageSize - std::max(lookup, address)) / sizeof(*names);
        for (unsigned j = 0; j < count && names[j].u1.AddressOfData; ++j) {
            const auto name = names[j].u1.AddressOfData;
            if (!IMAGE_SNAP_BY_ORDINAL32(name) && inImage(name, sizeof(WORD) + sizeof(function)) &&
                !memcmp(image + name + sizeof(WORD), function, sizeof(function))) {
                return reinterpret_cast<void**>(&slots[j].u1.Function);
            }
        }
        return nullptr;
    }
    return nullptr;
}

enum class HackEvent : unsigned { InstallHacks = 1 };
} // namespace

extern "C" __declspec(dllexport) unsigned __cdecl HackEntryPoint(HackEvent event, void*)
{
    if (event != HackEvent::InstallHacks || createFactory) return 0;
    auto** slot = LauncherFactoryImport();
    DWORD protection;
    if (!slot || !*slot || !VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) return 0;
    HMODULE pinned;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&HackEntryPoint), &pinned) && InitializeMaterials()) {
        ReadPbrSettings();
        InstallPostEffects(pbrSettings.post);
        const auto settings = std::to_string(pbrSettings.directPercent) + "% direct, " +
                              std::to_string(pbrSettings.environmentPercent) + "% environment.";
        ShaderLog("PBR lighting strengths: ", settings.c_str());
        createFactory = reinterpret_cast<Direct3DFactory>(*slot);
        factorySlot = slot;
        InterlockedExchangePointer(slot, reinterpret_cast<void*>(&CreateFactory));
        ShaderLog("Connected to the launcher's Direct3D 9 factory.");
    }
    DWORD ignored;
    VirtualProtect(slot, sizeof(*slot), protection, &ignored);
    return 0;
}
