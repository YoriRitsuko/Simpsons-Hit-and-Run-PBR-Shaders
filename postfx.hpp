#pragma once

#include <d3d9.h>

enum class ToneMapper { Aces13, KhronosNeutral };

struct PostSettings {
    bool linear = true;
    bool bloom = true;
    bool vignette = true;
    bool reflections = true;
    bool globalIllumination = true;
    bool staticIllumination = true;
    bool ambientOcclusion = true;
    bool occlusionOnly = false;
    bool depthOfField = true;
    bool foregroundBlur = false;
    bool histogramEqualization = true;
    unsigned reflectionSteps = 64;
    unsigned reflectionDistance = 64;
    unsigned indirectRays = 16;
    unsigned indirectDistance = 8;
    unsigned indirectStrength = 120;
    unsigned aoQuality = 1;
    unsigned aoRadius = 100;
    unsigned aoStrength = 100;
    unsigned exposure = 200;
    unsigned skyBrightness = 300;
    unsigned bloomStrength = 40;
    unsigned bloomStreakMix = 30;
    unsigned vignetteStrength = 65;
    unsigned focusDistance = 90;
    unsigned focusRange = 5;
    unsigned focusRadius = 12;
    unsigned histogramStrength = 35;
    ToneMapper toneMapper = ToneMapper::KhronosNeutral;
};

void InstallPostEffects(const PostSettings& settings);
void PostBeginScene(IDirect3DDevice9* device);
void PostEndScene(IDirect3DDevice9* device);
void PostReset(IDirect3DDevice9* device);
bool LinearWorld(IDirect3DDevice9* device);
bool TracksWorldCoverage(IDirect3DDevice9* device);
void CaptureWorldProjection(IDirect3DDevice9* device);
bool CapturingScreenMaterials(IDirect3DDevice9* device);
bool BindScreenMaterialOutputs(IDirect3DDevice9* device, const D3DMATRIX& projection);
void UnbindScreenMaterialOutputs(IDirect3DDevice9* device, bool drawn);
bool BeginStaticDepthDraw(IDirect3DDevice9* device);
void EndStaticDepthDraw(IDirect3DDevice9* device, bool drawn);
