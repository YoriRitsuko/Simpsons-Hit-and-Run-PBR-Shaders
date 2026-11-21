#define WIN32_LEAN_AND_MEAN
#include "postfx.hpp"
#include "com_ptr.hpp"
#include "materials.hpp"

#include "AcesComposite.inc"
#include "NeutralComposite.inc"
#include "BloomPrefilter.inc"
#include "BloomDownsample.inc"
#include "BloomConvolution.inc"
#include "BloomStreak.inc"
#include "ScreenReflection.inc"
#include "ScreenIrradiance.inc"
#include "StaticIrradianceShape.inc"
#include "FilterIrradiance.inc"
#include "IndirectComposite.inc"
#include "HorizonOcclusion.inc"
#include "FilterOcclusion.inc"
#include "SkyBrightness.inc"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {

constexpr D3DFORMAT hdrFormat = D3DFMT_A16B16G16R16F;
constexpr auto depthFormat = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
constexpr unsigned lutSize = 64;
constexpr unsigned bloomStreakLevel = 1; // Quarter resolution.
PostSettings options;
IDirect3DDevice9* sceneDevice = nullptr;
unsigned staticDisplayDepth = 0;

struct Target {
    ComPtr<IDirect3DTexture9> texture;
    ComPtr<IDirect3DSurface9> surface;
    UINT width = 0, height = 0;

    bool Create(IDirect3DDevice9* device, UINT w, UINT h)
    {
        width = w;
        height = h;
        return SUCCEEDED(device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, hdrFormat,
                                               D3DPOOL_DEFAULT, texture.Put(), nullptr)) &&
               SUCCEEDED(texture.Get()->GetSurfaceLevel(0, surface.Put()));
    }

    void Release()
    {
        surface.Reset();
        texture.Reset();
    }

    void Swap(Target& other)
    {
        surface.Swap(other.surface);
        texture.Swap(other.texture);
        std::swap(width, other.width);
        std::swap(height, other.height);
    }
};

struct BloomLevel {
    Target image;
    Target scratch;
};

struct Frame {
    IDirect3DDevice9* device = nullptr;
    UINT width = 0, height = 0;
    ComPtr<IDirect3DTexture9> lut;
    ComPtr<IDirect3DPixelShader9> composite;
    ComPtr<IDirect3DPixelShader9> bloomPrefilter;
    ComPtr<IDirect3DPixelShader9> bloomDownsample;
    ComPtr<IDirect3DPixelShader9> bloomConvolution;
    ComPtr<IDirect3DPixelShader9> bloomStreakShader;
    ComPtr<IDirect3DPixelShader9> reflections;
    ComPtr<IDirect3DPixelShader9> irradianceShader;
    ComPtr<IDirect3DPixelShader9> staticShapeShader;
    ComPtr<IDirect3DPixelShader9> irradianceFilter;
    ComPtr<IDirect3DPixelShader9> indirectComposite;
    ComPtr<IDirect3DPixelShader9> horizonOcclusion;
    ComPtr<IDirect3DPixelShader9> filterOcclusion;
    ComPtr<IDirect3DPixelShader9> skyBrightness;
    Target world;
    Target processed;
    Target irradiance;
    Target irradianceScratch;
    Target staticShape;
    Target occlusion;
    Target occlusionScratch;
    std::array<BloomLevel, 4> bloom;
    Target bloomStreak;
    std::array<Target, 3> material;
    ComPtr<IDirect3DTexture9> readableDepth;
    ComPtr<IDirect3DSurface9> depth;
    ComPtr<IDirect3DTexture9> staticDepthTexture;
    ComPtr<IDirect3DSurface9> staticDepth;
    ComPtr<IDirect3DSurface9> backBuffer;
    ComPtr<IDirect3DSurface9> backDepth;
    bool ready = false;
    bool bloomReady = false;
    bool reflectionReady = false;
    bool indirectReady = false;
    bool staticDepthReady = false;
    bool aoReady = false;
    bool active = false;
    bool depthBound = false;
    bool materialTargetsCleared = false;
    bool aoProjectionKnown = false;
    bool hasReceivers = false;
    bool hasStaticReceivers = false;
    bool staticShapeResolved = false;
    D3DMATRIX projection{};
    D3DMATRIX aoProjection{};

    void ClearBloom()
    {
        for (auto& level : bloom) {
            level.image.Release();
            level.scratch.Release();
        }
        bloomStreak.Release();
        bloomPrefilter.Reset();
        bloomDownsample.Reset();
        bloomConvolution.Reset();
        bloomStreakShader.Reset();
        bloomReady = false;
    }

    void Clear()
    {
        backBuffer.Reset();
        backDepth.Reset();
        world.Release();
        processed.Release();
        irradiance.Release();
        irradianceScratch.Release();
        staticShape.Release();
        occlusion.Release();
        occlusionScratch.Release();
        ClearBloom();
        for (auto& target : material) target.Release();
        depth.Reset();
        readableDepth.Reset();
        staticDepth.Reset();
        staticDepthTexture.Reset();
        lut.Reset();
        composite.Reset();
        reflections.Reset();
        irradianceShader.Reset();
        staticShapeShader.Reset();
        irradianceFilter.Reset();
        indirectComposite.Reset();
        horizonOcclusion.Reset();
        filterOcclusion.Reset();
        skyBrightness.Reset();
        ready = reflectionReady = indirectReady = active = depthBound = hasReceivers = false;
        staticDepthReady = hasStaticReceivers = staticShapeResolved = false;
        aoReady = aoProjectionKnown = materialTargetsCleared = false;
        device = nullptr;
    }
} frame;

template<uintptr_t address>
void __fastcall DisplayStatic(void* entity, void*)
{
    using Display = void (__thiscall*)(void*);
    ++staticDisplayDepth;
    reinterpret_cast<Display>(address)(entity);
    --staticDisplayDepth;
}

bool InstallStaticDrawHooks()
{
    // StaticEntityDSG, InstStatEntityDSG and InstStatPhysDSG Display slots.
    // Dynamic and animated entities share the opaque queue with these classes.
    struct Slot { uintptr_t address, original; void* replacement; };
    const Slot slots[] = {
        {0x60c928, 0x4a5630, reinterpret_cast<void*>(&DisplayStatic<0x4a5630>)},
        {0x60cc00, 0x4a1020, reinterpret_cast<void*>(&DisplayStatic<0x4a1020>)},
        {0x60cb80, 0x4a1600, reinterpret_cast<void*>(&DisplayStatic<0x4a1600>)},
    };
    for (const auto& slot : slots) {
        if (*reinterpret_cast<const uintptr_t*>(slot.address) != slot.original) return false;
    }
    // All three slots share the same read-only data page.
    const auto start = reinterpret_cast<void*>(0x60c928);
    DWORD protection;
    if (!VirtualProtect(start, 0x60cc04 - 0x60c928, PAGE_READWRITE, &protection)) return false;
    for (const auto& slot : slots)
        InterlockedExchangePointer(reinterpret_cast<void**>(slot.address), slot.replacement);
    VirtualProtect(start, 0x60cc04 - 0x60c928, protection, &protection);
    return true;
}

bool LoadAcesLut(IDirect3DDevice9* device)
{
    HMODULE module;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&LoadAcesLut), &module)) return false;
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));
    constexpr UINT rowBytes = lutSize * 8;
    constexpr DWORD bytes = rowBytes * lutSize * lutSize;
    if (!resource || SizeofResource(module, resource) != bytes) return false;
    const auto* data = static_cast<const char*>(LockResource(LoadResource(module, resource)));
    if (!data) return false;

    constexpr UINT tiles = 8;
    constexpr UINT atlasSize = lutSize * tiles;
    ComPtr<IDirect3DTexture9> staging;
    if (FAILED(device->CreateTexture(atlasSize, atlasSize, 1, 0, hdrFormat,
                                     D3DPOOL_SYSTEMMEM, staging.Put(), nullptr))) return false;
    D3DLOCKED_RECT lock{};
    if (FAILED(staging.Get()->LockRect(0, &lock, nullptr, 0))) return false;
    for (UINT z = 0; z < lutSize; ++z) {
        for (UINT y = 0; y < lutSize; ++y) {
            auto* row = static_cast<char*>(lock.pBits) + (z / tiles * lutSize + y) * lock.Pitch +
                        (z % tiles) * rowBytes;
            memcpy(row, data + (z * lutSize + y) * rowBytes, rowBytes);
        }
    }
    staging.Get()->UnlockRect(0);
    return SUCCEEDED(device->CreateTexture(atlasSize, atlasSize, 1, 0, hdrFormat,
                                            D3DPOOL_DEFAULT, frame.lut.Put(), nullptr)) &&
           SUCCEEDED(device->UpdateTexture(staging.Get(), frame.lut.Get()));
}

bool CreateReadableDepth(IDirect3DDevice9* device, UINT width, UINT height)
{
    return SUCCEEDED(device->CreateTexture(width, height, 1, D3DUSAGE_DEPTHSTENCIL, depthFormat,
                                           D3DPOOL_DEFAULT, frame.readableDepth.Put(), nullptr)) &&
           SUCCEEDED(frame.readableDepth.Get()->GetSurfaceLevel(0, frame.depth.Put()));
}

bool CreateScreenData(IDirect3DDevice9* device, UINT width, UINT height)
{
    D3DCAPS9 caps{};
    if (FAILED(device->GetDeviceCaps(&caps)) || caps.NumSimultaneousRTs < 4 ||
        !(caps.PrimitiveMiscCaps & D3DPMISCCAPS_SEPARATEALPHABLEND) ||
        !frame.processed.Create(device, width, height)) return false;
    for (auto& target : frame.material) {
        if (!target.Create(device, width, height)) return false;
    }
    return true;
}

bool CreateAmbientOcclusion(IDirect3DDevice9* device, UINT width, UINT height)
{
    const auto halfWidth = std::max(1u, width / 2);
    const auto halfHeight = std::max(1u, height / 2);
    return frame.occlusion.Create(device, halfWidth, halfHeight) &&
           frame.occlusionScratch.Create(device, halfWidth, halfHeight) &&
           SUCCEEDED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(HorizonOcclusionCode),
                                               frame.horizonOcclusion.Put())) &&
           SUCCEEDED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(FilterOcclusionCode),
                                               frame.filterOcclusion.Put()));
}

bool CreateIndirectLighting(IDirect3DDevice9* device, UINT width, UINT height)
{
    const auto halfWidth = std::max(1u, width / 2);
    const auto halfHeight = std::max(1u, height / 2);
    return frame.irradiance.Create(device, halfWidth, halfHeight) &&
           frame.irradianceScratch.Create(device, halfWidth, halfHeight) &&
           SUCCEEDED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(ScreenIrradianceCode),
                                               frame.irradianceShader.Put())) &&
           SUCCEEDED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(FilterIrradianceCode),
                                               frame.irradianceFilter.Put())) &&
           SUCCEEDED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(IndirectCompositeCode),
                                               frame.indirectComposite.Put()));
}

bool CreateStaticCapture(IDirect3DDevice9* device, UINT width, UINT height)
{
    return SUCCEEDED(device->CreateTexture(width, height, 1, D3DUSAGE_DEPTHSTENCIL, depthFormat,
                                           D3DPOOL_DEFAULT, frame.staticDepthTexture.Put(), nullptr)) &&
           SUCCEEDED(frame.staticDepthTexture.Get()->GetSurfaceLevel(0, frame.staticDepth.Put())) &&
           frame.staticShape.Create(device, width, height) &&
           SUCCEEDED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(StaticIrradianceShapeCode),
                                               frame.staticShapeShader.Put()));
}

bool CreateBloom(IDirect3DDevice9* device, UINT width, UINT height)
{
    if (FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(BloomPrefilterCode),
                                        frame.bloomPrefilter.Put())) ||
        FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(BloomDownsampleCode),
                                        frame.bloomDownsample.Put())) ||
        FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(BloomConvolutionCode),
                                        frame.bloomConvolution.Put()))) return false;
    for (auto& level : frame.bloom) {
        width = (width + 1) / 2;
        height = (height + 1) / 2;
        if (!level.image.Create(device, width, height) || !level.scratch.Create(device, width, height)) return false;
    }
    if (options.bloomStreakMix != 0) {
        const auto& source = frame.bloom[bloomStreakLevel].image;
        if (!frame.bloomStreak.Create(device, source.width, source.height) ||
            FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(BloomStreakCode),
                                            frame.bloomStreakShader.Put()))) {
            frame.bloomStreak.Release();
            frame.bloomStreakShader.Reset();
            ShaderLog("Bloom streak resources are unavailable.");
        }
    }
    return true;
}

bool EnsureTargets(IDirect3DDevice9* device, const D3DSURFACE_DESC& desc)
{
    if (frame.device == device && frame.width == desc.Width && frame.height == desc.Height) return frame.ready;
    frame.Clear();
    frame.device = device;
    frame.width = desc.Width;
    frame.height = desc.Height;
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        ShaderLog("Linear HDR is unavailable with a multisampled backbuffer.");
        return false;
    }
    const bool aces = options.toneMapper == ToneMapper::Aces13;
    const auto* code = aces ? AcesCompositeCode : NeutralCompositeCode;
    if (!frame.world.Create(device, desc.Width, desc.Height) || (aces && !LoadAcesLut(device)) ||
        FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(code), frame.composite.Put()))) {
        ShaderLog("Could not create the HDR color target or tone-mapping resources.");
        return false;
    }
    frame.ready = true;
    if (options.bloom && options.bloomStrength != 0) {
        frame.bloomReady = CreateBloom(device, desc.Width, desc.Height);
        if (!frame.bloomReady) {
            frame.ClearBloom();
            ShaderLog("Convolution bloom resources are unavailable.");
        }
    }
    const bool indirect = options.globalIllumination && options.indirectStrength != 0;
    const bool ao = options.ambientOcclusion && (options.aoStrength || options.occlusionOnly);
    const bool screen = options.reflections || indirect;
    const bool depth = (screen || ao) && CreateReadableDepth(device, desc.Width, desc.Height);
    frame.aoReady = ao && depth && CreateAmbientOcclusion(device, desc.Width, desc.Height);
    if (screen && depth && CreateScreenData(device, desc.Width, desc.Height)) {
        frame.reflectionReady = options.reflections &&
            SUCCEEDED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(ScreenReflectionCode),
                                                frame.reflections.Put()));
        frame.indirectReady = indirect && CreateIndirectLighting(device, desc.Width, desc.Height);
        if (frame.indirectReady && options.staticIllumination && !CreateStaticCapture(device, desc.Width, desc.Height)) {
            frame.staticDepth.Reset();
            ShaderLog("Could not create the opaque scenery SSGI targets.");
        }
    }
    if (options.reflections && !frame.reflectionReady) ShaderLog("SSR depth or material targets are unavailable.");
    if (indirect && !frame.indirectReady) ShaderLog("SSGI depth or material targets are unavailable.");
    if (ao && !frame.aoReady) ShaderLog("Ambient occlusion depth or half-resolution targets are unavailable.");
    if (options.skyBrightness != 100 &&
        ((!frame.processed.surface.Get() && !frame.processed.Create(device, desc.Width, desc.Height)) ||
         FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD*>(SkyBrightnessCode), frame.skyBrightness.Put()))))
        ShaderLog("Could not create the sky brightness pass.");
    ShaderLog("Created linear HDR world target. Tone mapper: ", aces ? "ACES 1.3" : "Khronos PBR Neutral");
    return true;
}

void StartWorld()
{
    auto* device = sceneDevice;
    if (!device || frame.active) return;
    ComPtr<IDirect3DSurface9> target;
    ComPtr<IDirect3DSurface9> depth;
    D3DSURFACE_DESC desc{};
    D3DVIEWPORT9 viewport{};
    if (FAILED(device->GetRenderTarget(0, target.Put())) || FAILED(target.Get()->GetDesc(&desc)) ||
        FAILED(device->GetDepthStencilSurface(depth.Put())) || FAILED(device->GetViewport(&viewport)) ||
        !EnsureTargets(device, desc)) return;

    frame.backBuffer.Swap(target);
    frame.backDepth.Swap(depth);
    frame.hasReceivers = false;
    frame.hasStaticReceivers = false;
    frame.staticShapeResolved = false;
    frame.staticDepthReady = false;
    frame.depthBound = false;
    frame.aoProjectionKnown = false;
    frame.materialTargetsCleared = false;
    if (frame.reflectionReady || frame.indirectReady) {
        // Clear before binding the material targets to an opaque draw.
        device->SetDepthStencilSurface(nullptr);
        bool cleared = true;
        for (auto& target : frame.material) {
            cleared = cleared && SUCCEEDED(device->SetRenderTarget(0, target.surface.Get())) &&
                SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0));
        }
        if (frame.staticDepth.Get() && SUCCEEDED(device->SetDepthStencilSurface(frame.staticDepth.Get())))
            frame.staticDepthReady = SUCCEEDED(device->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1, 0));
        frame.materialTargetsCleared = cleared;
    }
    if (frame.materialTargetsCleared || frame.aoReady)
        frame.depthBound = SUCCEEDED(device->SetDepthStencilSurface(frame.depth.Get()));
    if (!frame.depthBound) device->SetDepthStencilSurface(frame.backDepth.Get());
    if (FAILED(device->SetRenderTarget(0, frame.world.surface.Get()))) {
        device->SetRenderTarget(0, frame.backBuffer.Get());
        device->SetDepthStencilSurface(frame.backDepth.Get());
        frame.backBuffer.Reset();
        frame.backDepth.Reset();
        return;
    }
    device->SetViewport(&viewport);
    frame.active = true;
}

void Sample(IDirect3DDevice9* device, DWORD slot, IDirect3DBaseTexture9* texture, DWORD filter)
{
    device->SetTexture(slot, texture);
    device->SetSamplerState(slot, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(slot, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(slot, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
    device->SetSamplerState(slot, D3DSAMP_MAGFILTER, filter);
    device->SetSamplerState(slot, D3DSAMP_MINFILTER, filter);
    device->SetSamplerState(slot, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(slot, D3DSAMP_SRGBTEXTURE, FALSE);
}

bool FullScreen(IDirect3DDevice9* device, IDirect3DPixelShader9* shader, UINT width, UINT height)
{
    const float right = float(width) - 0.5f;
    const float bottom = float(height) - 0.5f;
    const float vertices[4][6] = {
        {-0.5f, -0.5f, 0, 1, 0, 0}, {right, -0.5f, 0, 1, 1, 0},
        {-0.5f, bottom, 0, 1, 0, 1}, {right, bottom, 0, 1, 1, 1}
    };
    const D3DVIEWPORT9 viewport{0, 0, width, height, 0, 1};
    device->SetViewport(&viewport);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    return SUCCEEDED(device->SetVertexShader(nullptr)) &&
           SUCCEEDED(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1)) &&
           SUCCEEDED(device->SetPixelShader(shader)) &&
           SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(vertices[0])));
}

void ResolveSkyBrightness()
{
    if (!frame.active || !frame.skyBrightness.Get()) return;
    auto* device = frame.device;
    ComPtr<IDirect3DSurface9> target;
    D3DVIEWPORT9 view{};
    ComPtr<IDirect3DStateBlock9> saved;
    if (FAILED(device->GetRenderTarget(0, target.Put())) || target.Get() != frame.world.surface.Get() ||
        FAILED(device->GetViewport(&view)) ||
        FAILED(device->CreateStateBlock(D3DSBT_ALL, saved.Put())) || FAILED(saved.Get()->Capture()) ||
        FAILED(device->SetRenderTarget(0, frame.processed.surface.Get()))) return;

    const float width = float(frame.width);
    const float height = float(frame.height);
    const float area[] = {view.X / width, view.Y / height,
                          (view.X + view.Width) / width, (view.Y + view.Height) / height};
    const float gain[] = {options.skyBrightness * 0.01f, 0, 0, 0};
    device->SetPixelShaderConstantF(0, gain, 1);
    device->SetPixelShaderConstantF(9, area, 1);
    Sample(device, 0, frame.world.texture.Get(), D3DTEXF_POINT);
    // The sky is the only color drawn in this view so far. Keep the scene
    // depth surface bound and let the rest of the game render over the sky.
    frame.active = false;
    const bool drawn = FullScreen(device, frame.skyBrightness.Get(), frame.width, frame.height);
    frame.active = true;
    if (drawn) frame.world.Swap(frame.processed);
    else device->SetRenderTarget(0, target.Get());
    saved.Get()->Apply();
    device->SetViewport(&view);

    static bool logged = false;
    if (drawn && !logged) {
        const auto setting = std::to_string(options.skyBrightness) + "%.";
        ShaderLog("Applied linear sky brightness: ", setting.c_str());
        logged = true;
    }
}

void __fastcall EndSkyPhase(void* context, void*, bool enabled)
{
    using EnableDepth = void (__thiscall*)(void*, bool);
    auto** table = *static_cast<void***>(context);
    reinterpret_cast<EnableDepth>(table[0xe8 / sizeof(void*)])(context, enabled);
    ResolveSkyBrightness();
}

bool InstallSkyBrightnessHook()
{
    // WorldRenderLayer calls EnableZBuffer(true) right after the sky group.
    auto* site = reinterpret_cast<uint8_t*>(0x4aad0d);
    const uint8_t original[] = {0xff, 0x90, 0xe8, 0x00, 0x00, 0x00};
    if (memcmp(site, original, sizeof(original)) != 0) return false;
    uint8_t patch[] = {0xe8, 0, 0, 0, 0, 0x90};
    const auto displacement = uint32_t(reinterpret_cast<uintptr_t>(&EndSkyPhase) - reinterpret_cast<uintptr_t>(site + 5));
    memcpy(patch + 1, &displacement, sizeof(displacement));
    DWORD protection;
    if (!VirtualProtect(site, sizeof(patch), PAGE_EXECUTE_READWRITE, &protection)) return false;
    memcpy(site, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(patch));
    VirtualProtect(site, sizeof(patch), protection, &protection);
    return true;
}

bool DrawBloomPass(IDirect3DDevice9* device, IDirect3DPixelShader9* shader, const Target& source,
                   const Target& target, const std::array<float, 4>& parameters, DWORD filter = D3DTEXF_LINEAR)
{
    if (FAILED(device->SetRenderTarget(0, target.surface.Get())) ||
        FAILED(device->SetPixelShaderConstantF(0, parameters.data(), 1))) return false;
    Sample(device, 0, source.texture.Get(), filter);
    const bool drawn = FullScreen(device, shader, target.width, target.height);
    device->SetTexture(0, nullptr);
    return drawn;
}

struct BloomOutput {
    bool halo = false;
    bool streak = false;
};

std::array<float, 4> BloomReduction(const Target& source, const Target& target)
{
    return {float(source.width) / target.width, float(source.height) / target.height,
            1.0f / source.width, 1.0f / source.height};
}

BloomOutput RenderBloom(IDirect3DDevice9* device)
{
    const auto& first = frame.bloom.front().image;
    if (!DrawBloomPass(device, frame.bloomPrefilter.Get(), frame.world, first,
                       BloomReduction(frame.world, first), D3DTEXF_POINT)) return {};

    for (unsigned i = 1; i < frame.bloom.size(); ++i) {
        const auto& source = frame.bloom[i - 1].image;
        const auto& target = frame.bloom[i].image;
        if (!DrawBloomPass(device, frame.bloomDownsample.Get(), source, target,
                           BloomReduction(source, target))) return {};
    }

    // Give the streak a soft vertical profile so it slides between rows as
    // a highlight moves. Reuse the quarter-resolution halo's scratch target.
    const auto& streakLevel = frame.bloom[bloomStreakLevel];
    const bool streak = frame.bloomStreakShader.Get() &&
        DrawBloomPass(device, frame.bloomConvolution.Get(), streakLevel.image, streakLevel.scratch,
                      {0.0f, 1.0f / streakLevel.image.height}) &&
        DrawBloomPass(device, frame.bloomStreakShader.Get(), streakLevel.scratch, frame.bloomStreak,
                      {1.0f / streakLevel.image.width});
    for (const auto& level : frame.bloom) {
        if (!DrawBloomPass(device, frame.bloomConvolution.Get(), level.image, level.scratch,
                           {1.0f / level.image.width, 0.0f}) ||
            !DrawBloomPass(device, frame.bloomConvolution.Get(), level.scratch, level.image,
                           {0.0f, 1.0f / level.image.height})) return {};
    }
    static bool logged = false;
    if (!logged) {
        ShaderLog(streak ? "Rendered HDR convolution bloom with horizontal streaks." :
                           "Rendered HDR convolution bloom.");
        logged = true;
    }
    return {true, streak};
}

void SampleScreenData(IDirect3DDevice9* device, DWORD colorFilter)
{
    device->SetPixelShaderConstantF(1, &frame.projection.m[0][0], 4);
    Sample(device, 0, frame.world.texture.Get(), colorFilter);
    Sample(device, 1, frame.readableDepth.Get(), D3DTEXF_POINT);
    for (unsigned i = 0; i < frame.material.size(); ++i)
        Sample(device, i + 2, frame.material[i].texture.Get(), D3DTEXF_POINT);
}

bool ResolveScreenPass(IDirect3DDevice9* device, IDirect3DPixelShader9* shader, const float* parameters)
{
    // The former input becomes the output after each successful resolve.
    device->SetTexture(0, nullptr);
    if (FAILED(device->SetRenderTarget(0, frame.processed.surface.Get()))) return false;
    device->SetPixelShaderConstantF(0, parameters, 1);
    SampleScreenData(device, D3DTEXF_LINEAR);
    if (!FullScreen(device, shader, frame.width, frame.height)) return false;
    frame.world.Swap(frame.processed);
    return true;
}

void ResolveOpaqueReflections(IDirect3DDevice9* device)
{
    if (!frame.reflectionReady) return;
    const float parameters[4] = {1.0f / frame.width, 1.0f / frame.height,
                                 static_cast<float>(options.reflectionSteps),
                                 static_cast<float>(options.reflectionDistance)};
    if (ResolveScreenPass(device, frame.reflections.Get(), parameters)) {
        static bool logged = false;
        if (!logged) {
            const auto settings = std::to_string(options.reflectionSteps) + " steps, " +
                                  std::to_string(options.reflectionDistance) + " world units.";
            ShaderLog("Resolved opaque PBR SSR after the world draw: ", settings.c_str());
            logged = true;
        }
    }
}

std::array<float, 4> IrradianceParameters()
{
    return {1.0f / frame.irradiance.width, 1.0f / frame.irradiance.height,
            1.0f / frame.width, 1.0f / frame.height};
}

void ResolveStaticShape(IDirect3DDevice9* device)
{
    if (!frame.hasStaticReceivers || FAILED(device->SetRenderTarget(0, frame.staticShape.surface.Get()))) return;
    const auto parameters = IrradianceParameters();
    device->SetPixelShaderConstantF(0, parameters.data(), 1);
    SampleScreenData(device, D3DTEXF_POINT);
    Sample(device, 7, frame.staticDepthTexture.Get(), D3DTEXF_POINT);
    frame.staticShapeResolved = FullScreen(device, frame.staticShapeShader.Get(), frame.width, frame.height);
}

bool GatherIrradiance(IDirect3DDevice9* device)
{
    if (!frame.indirectReady) return false;
    ResolveStaticShape(device);
    if ((!frame.hasReceivers && !frame.staticShapeResolved) ||
        FAILED(device->SetRenderTarget(0, frame.irradiance.surface.Get()))) return false;
    const auto parameters = IrradianceParameters();
    const float controls[4] = {float(options.indirectRays), float(options.indirectDistance),
                               0.008f * options.indirectStrength, frame.staticShapeResolved ? 1.0f : 0.0f};
    device->SetPixelShaderConstantF(0, parameters.data(), 1);
    device->SetPixelShaderConstantF(5, controls, 1);
    SampleScreenData(device, D3DTEXF_POINT);
    Sample(device, 7, frame.staticShapeResolved ? frame.staticShape.texture.Get() : nullptr, D3DTEXF_POINT);
    if (!FullScreen(device, frame.irradianceShader.Get(), frame.irradiance.width, frame.irradiance.height)) return false;

    // Soften the individual ray hits while keeping depth and surface-normal
    // edges crisp. Both directions reuse the irradiance depth in alpha.
    for (const bool horizontal : {true, false}) {
        device->SetTexture(6, nullptr);
        if (FAILED(device->SetRenderTarget(0, frame.irradianceScratch.surface.Get()))) return false;
        const float filter[4] = {0, 0, horizontal ? parameters[0] : 0.0f, horizontal ? 0.0f : parameters[1]};
        device->SetPixelShaderConstantF(0, filter, 1);
        Sample(device, 6, frame.irradiance.texture.Get(), D3DTEXF_POINT);
        if (!FullScreen(device, frame.irradianceFilter.Get(), frame.irradiance.width, frame.irradiance.height))
            return false;
        frame.irradiance.Swap(frame.irradianceScratch);
    }
    return true;
}

void ResolveIndirectLighting(IDirect3DDevice9* device)
{
    const auto parameters = IrradianceParameters();
    Sample(device, 6, frame.irradiance.texture.Get(), D3DTEXF_POINT);
    if (ResolveScreenPass(device, frame.indirectComposite.Get(), parameters.data())) {
        static bool logged = false;
        if (!logged) {
            const auto settings = std::to_string(options.indirectRays) + " rays, " +
                                  std::to_string(options.indirectDistance) + " world units, " +
                                  std::to_string(options.indirectStrength) + "% strength.";
            ShaderLog("Composited filtered half-resolution SSGI: ", settings.c_str());
            logged = true;
        }
        static bool loggedStatic = false;
        if (frame.staticShapeResolved && !loggedStatic) {
            ShaderLog("Composited SSGI on visible opaque level scenery.");
            loggedStatic = true;
        }
    }
}

bool RenderAmbientOcclusion(IDirect3DDevice9* device)
{
    if (!frame.depthBound || !frame.aoProjectionKnown) return false;
    const auto width = frame.occlusion.width;
    const auto height = frame.occlusion.height;
    constexpr float slices[] = {2.0f, 4.0f, 6.0f};
    constexpr float steps[] = {4.0f, 6.0f, 8.0f};
    const float controls[] = {0.012f * options.aoRadius, 0.007f * options.aoStrength,
                              slices[options.aoQuality], steps[options.aoQuality]};
    const float grid[] = {1.0f / width, 1.0f / height, 1.0f / frame.width, 1.0f / frame.height};
    device->SetPixelShaderConstantF(1, &frame.aoProjection.m[0][0], 4);
    device->SetPixelShaderConstantF(6, controls, 1);
    device->SetPixelShaderConstantF(7, grid, 1);
    Sample(device, 9, frame.readableDepth.Get(), D3DTEXF_POINT);
    if (FAILED(device->SetRenderTarget(0, frame.occlusion.surface.Get())) ||
        !FullScreen(device, frame.horizonOcclusion.Get(), width, height)) return false;

    // Filter across neighboring samples on the same surface, first along
    // each axis at half resolution, then jointly upsample in the HDR composite.
    for (const bool horizontal : {true, false}) {
        device->SetTexture(8, nullptr);
        if (FAILED(device->SetRenderTarget(0, frame.occlusionScratch.surface.Get()))) return false;
        const float direction[] = {horizontal ? grid[0] : 0.0f, horizontal ? 0.0f : grid[1], 0, 0};
        device->SetPixelShaderConstantF(0, direction, 1);
        Sample(device, 8, frame.occlusion.texture.Get(), D3DTEXF_POINT);
        if (!FullScreen(device, frame.filterOcclusion.Get(), width, height)) return false;
        frame.occlusion.Swap(frame.occlusionScratch);
    }
    static bool logged = false;
    if (!logged) {
        const auto settings = std::to_string(unsigned(controls[2])) + " slices, " +
                              std::to_string(unsigned(controls[3])) + " steps per side, radius " +
                              std::to_string(options.aoRadius) + "%, strength " +
                              std::to_string(options.aoStrength) + "%.";
        ShaderLog(options.occlusionOnly ? "Rendered GTAO (occlusion-only view): " : "Rendered GTAO: ", settings.c_str());
        logged = true;
    }
    return true;
}

void FinishWorld()
{
    if (!frame.active) return;
    frame.active = false;
    auto* device = frame.device;
    ComPtr<IDirect3DStateBlock9> saved;
    D3DVIEWPORT9 viewport{};
    device->GetViewport(&viewport);
    const bool captured = SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, saved.Put())) &&
                          SUCCEEDED(saved.Get()->Capture());
    device->SetDepthStencilSurface(nullptr);
    // Gameplay draws characters and player vehicles late. Include their
    // color and depth, then composite on the configured visible receivers.
    if (captured && frame.depthBound && (frame.hasReceivers || frame.hasStaticReceivers)) {
        // Trace diffuse rays against the original HDR frame before SSR or
        // bloom contributes any display effects to the source radiance.
        const bool irradiance = GatherIrradiance(device);
        if (frame.hasReceivers) ResolveOpaqueReflections(device);
        if (irradiance) ResolveIndirectLighting(device);
    }
    const bool ao = captured && RenderAmbientOcclusion(device);
    const auto bloom = captured && frame.bloomReady && !(ao && options.occlusionOnly)
        ? RenderBloom(device) : BloomOutput{};
    device->SetRenderTarget(0, frame.backBuffer.Get());
    if (captured) {
        const float parameters[4] = {bloom.halo ? options.bloomStrength * 0.01f : 0.0f,
                                     bloom.streak ? options.bloomStreakMix * 0.01f : 0.0f, 0,
                                     options.exposure * 0.01f};
        device->SetPixelShaderConstantF(0, parameters, 1);
        const float ambientMode[4] = {ao ? (options.occlusionOnly ? 2.0f : 1.0f) : 0.0f, 0, 0, 0};
        device->SetPixelShaderConstantF(8, ambientMode, 1);
        const float diagonal = std::hypot(float(frame.width), float(frame.height));
        const float vignette[4] = {frame.width / diagonal, frame.height / diagonal,
                                   options.vignette ? options.vignetteStrength * 0.01f : 0.0f, 0};
        device->SetPixelShaderConstantF(10, vignette, 1);
        Sample(device, 0, frame.world.texture.Get(), D3DTEXF_POINT);
        if (ao) Sample(device, 8, frame.occlusion.texture.Get(), D3DTEXF_POINT);
        if (bloom.halo) {
            for (unsigned i = 0; i < frame.bloom.size(); ++i)
                Sample(device, i + 1, frame.bloom[i].image.texture.Get(), D3DTEXF_LINEAR);
        }
        if (bloom.streak) Sample(device, 10, frame.bloomStreak.texture.Get(), D3DTEXF_LINEAR);
        if (frame.lut.Get()) Sample(device, 5, frame.lut.Get(), D3DTEXF_LINEAR);
        const bool drawn = FullScreen(device, frame.composite.Get(), frame.width, frame.height);
        static bool logged = false;
        if (drawn && !logged) {
            ShaderLog("Composited HDR before the GUI using ",
                      options.toneMapper == ToneMapper::Aces13 ? "ACES 1.3." : "Khronos PBR Neutral.");
            const auto exposure = std::to_string(options.exposure) + "%.";
            ShaderLog("Applied HDR exposure: ", exposure.c_str());
            if (vignette[2] > 0) {
                const auto strength = std::to_string(options.vignetteStrength) + "%.";
                ShaderLog("Applied vignette strength: ", strength.c_str());
            }
            logged = true;
        }
        saved.Get()->Apply();
    }
    device->SetDepthStencilSurface(frame.backDepth.Get());
    device->SetViewport(&viewport);
    frame.backBuffer.Reset();
    frame.backDepth.Reset();
}

} // namespace

void __fastcall RunWorldLayer(void* layer, void*)
{
    using RenderLayer = void (__thiscall*)(void*);
    StartWorld();
    reinterpret_cast<RenderLayer>(0x4aac00)(layer);
    FinishWorld();
}

void InstallPostEffects(const PostSettings& settings)
{
    if (!settings.linear) return;
    options = settings;
    // WorldRenderLayer::Render runs inside the gameplay frame. The GUI has
    // a separate layer and gets the original backbuffer after this returns.
    auto** render = reinterpret_cast<void**>(0x60be44);
    DWORD protection;
    if (*render != reinterpret_cast<void*>(0x4aac00) ||
        !VirtualProtect(render, sizeof(*render), PAGE_READWRITE, &protection)) {
        ShaderLog("The retail world-layer dispatch was not available for HDR.");
        options.linear = false;
        return;
    }
    InterlockedExchangePointer(render, reinterpret_cast<void*>(&RunWorldLayer));
    VirtualProtect(render, sizeof(*render), protection, &protection);
    ShaderLog("Installed the retail HDR world-layer hook.");
    if (options.skyBrightness != 100 && !InstallSkyBrightnessHook()) {
        options.skyBrightness = 100;
        ShaderLog("The retail sky pass was unavailable for sky brightness.");
    }
    if (options.globalIllumination && options.indirectStrength && options.staticIllumination) {
        options.staticIllumination = InstallStaticDrawHooks();
        ShaderLog(options.staticIllumination ? "Installed static scenery display hooks for SSGI."
                                             : "Static scenery display hooks were unavailable for SSGI.");
    }
}

void PostBeginScene(IDirect3DDevice9* device)
{
    if (options.linear) sceneDevice = device;
}

void PostEndScene(IDirect3DDevice9* device)
{
    if (sceneDevice == device) sceneDevice = nullptr;
}

void PostReset(IDirect3DDevice9* device)
{
    PostEndScene(device);
    if (frame.device == device) frame.Clear();
}

bool LinearWorld(IDirect3DDevice9* device)
{
    return frame.active && device == frame.device;
}

bool CapturingScreenMaterials(IDirect3DDevice9* device)
{
    return TracksWorldCoverage(device) && frame.materialTargetsCleared;
}

bool TracksWorldCoverage(IDirect3DDevice9* device)
{
    return LinearWorld(device) && frame.depthBound;
}

void CaptureAmbientProjection(IDirect3DDevice9* device)
{
    if (!frame.aoReady || frame.aoProjectionKnown || !TracksWorldCoverage(device)) return;
    DWORD depth, writeDepth;
    if (SUCCEEDED(device->GetRenderState(D3DRS_ZENABLE, &depth)) && depth == D3DZB_TRUE &&
        SUCCEEDED(device->GetRenderState(D3DRS_ZWRITEENABLE, &writeDepth)) && writeDepth &&
        SUCCEEDED(device->GetTransform(D3DTS_PROJECTION, &frame.aoProjection)) && std::abs(frame.aoProjection._34) > 0.5f)
        frame.aoProjectionKnown = true;
}

bool BindScreenMaterialOutputs(IDirect3DDevice9* device, const D3DMATRIX& projection)
{
    if (!CapturingScreenMaterials(device) || std::abs(projection._34) < 0.5f) return false;
    for (unsigned i = 0; i < frame.material.size(); ++i) {
        if (FAILED(device->SetRenderTarget(i + 1, frame.material[i].surface.Get()))) {
            UnbindScreenMaterialOutputs(device, false);
            return false;
        }
        device->SetRenderState(static_cast<D3DRENDERSTATETYPE>(D3DRS_COLORWRITEENABLE1 + i), 15);
    }
    frame.projection = projection;
    return true;
}

void UnbindScreenMaterialOutputs(IDirect3DDevice9* device, bool drawn)
{
    for (unsigned i = 0; i < frame.material.size(); ++i) device->SetRenderTarget(i + 1, nullptr);
    frame.hasReceivers |= drawn;
}

namespace {
constexpr D3DRENDERSTATETYPE depthOnlyStates[] = {D3DRS_COLORWRITEENABLE, D3DRS_STENCILENABLE, D3DRS_ZFUNC};
constexpr DWORD depthOnlyValues[] = {0, FALSE, D3DCMP_LESSEQUAL};
DWORD beforeStaticDraw[std::size(depthOnlyStates)]{};
}

bool BeginStaticDepthDraw(IDirect3DDevice9* device)
{
    if (!staticDisplayDepth || !CapturingScreenMaterials(device) || !frame.staticDepthReady) return false;
    DWORD depth, writeDepth, blend;
    if (FAILED(device->GetRenderState(D3DRS_ZENABLE, &depth)) || depth != D3DZB_TRUE ||
        FAILED(device->GetRenderState(D3DRS_ZWRITEENABLE, &writeDepth)) || !writeDepth ||
        FAILED(device->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend)) || blend) return false;

    ComPtr<IDirect3DSurface9> sceneDepth;
    D3DMATRIX projection{};
    if (FAILED(device->GetDepthStencilSurface(sceneDepth.Put())) || sceneDepth.Get() != frame.depth.Get() ||
        FAILED(device->GetTransform(D3DTS_PROJECTION, &projection)) || std::abs(projection._34) < 0.5f) return false;
    for (unsigned i = 0; i < std::size(depthOnlyStates); ++i) {
        if (FAILED(device->GetRenderState(depthOnlyStates[i], &beforeStaticDraw[i]))) return false;
    }
    if (FAILED(device->SetDepthStencilSurface(frame.staticDepth.Get()))) return false;
    // Replay the original draw with no color writes. The stock alpha test
    // and culling still run, so holes in fences and leaves stay open.
    for (unsigned i = 0; i < std::size(depthOnlyStates); ++i) {
        if (FAILED(device->SetRenderState(depthOnlyStates[i], depthOnlyValues[i]))) {
            EndStaticDepthDraw(device, false);
            return false;
        }
    }
    frame.projection = projection;
    return true;
}

void EndStaticDepthDraw(IDirect3DDevice9* device, bool drawn)
{
    for (unsigned i = 0; i < std::size(depthOnlyStates); ++i)
        device->SetRenderState(depthOnlyStates[i], beforeStaticDraw[i]);
    device->SetDepthStencilSurface(frame.depth.Get());
    frame.hasStaticReceivers |= drawn;
}
