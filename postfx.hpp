#pragma once

#include <d3d9.h>

enum class ToneMapper { Aces13, KhronosNeutral };

struct PostSettings {
    bool linear = false;
    bool bloom = true;
    bool reflections = false;
    bool globalIllumination = false;
    bool staticIllumination = false;
    bool ambientOcclusion = false;
    bool occlusionOnly = false;
    unsigned reflectionSteps = 64;
    unsigned reflectionDistance = 64;
    unsigned indirectRays = 16;
    unsigned indirectDistance = 3;
    unsigned indirectStrength = 100;
    unsigned aoQuality = 1;
    unsigned aoRadius = 100;
    unsigned aoStrength = 100;
    unsigned exposure = 100;
    unsigned skyBrightness = 100;
    ToneMapper toneMapper = ToneMapper::Aces13;
};

void InstallPostEffects(const PostSettings& settings);
void PostBeginScene(IDirect3DDevice9* device);
void PostEndScene(IDirect3DDevice9* device);
void PostReset(IDirect3DDevice9* device);
bool LinearWorld(IDirect3DDevice9* device);
bool TracksWorldCoverage(IDirect3DDevice9* device);
void CaptureAmbientProjection(IDirect3DDevice9* device);
bool CapturingScreenMaterials(IDirect3DDevice9* device);
bool BindScreenMaterialOutputs(IDirect3DDevice9* device, const D3DMATRIX& projection);
void UnbindScreenMaterialOutputs(IDirect3DDevice9* device, bool drawn);
bool BeginStaticDepthDraw(IDirect3DDevice9* device);
void EndStaticDepthDraw(IDirect3DDevice9* device, bool drawn);
