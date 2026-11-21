#pragma once

#include <windows.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

enum class MaterialMode { Glass, Paint, Pbr };
enum class AlphaMode { Opaque, Mask, Blend };

struct Image {
    unsigned width = 0;
    unsigned height = 0;
    std::vector<uint32_t> pixels;
};

struct Material {
    MaterialMode mode = MaterialMode::Glass;
    std::array<float, 3> paint{1, 1, 1};
    float reflection = 0.55f;
    float normalStrength = 1.0f;
    unsigned maskSet = 0;
    bool useGameSphere = false;
    std::wstring normalMap;
    std::wstring sphereMap;
    AlphaMode alphaMode = AlphaMode::Opaque;
    float opacity = 1.0f;
    float alphaCutoff = 0.5f;
    float roughness = 0.5f;
    float metallic = 0.0f;
    float exposure = 1.0f;
    float directLightStrength = 1.0f;
    float environmentStrength = 1.0f;
    float emissiveStrength = 1.0f;
    std::wstring baseColor;
    std::wstring orm;
    std::wstring emissive;
    std::wstring environment;
};

struct EnvironmentMap {
    unsigned size = 0;
    unsigned levels = 0;
    std::vector<uint8_t> texels;
};

bool InitializeMaterials();
const Material* ActiveMaterial();
const std::wstring& MaterialDirectory();
Image ReadTarga(const std::wstring& filename);
EnvironmentMap ReadEnvironment(const std::wstring& filename);
void ShaderLog(const char* message, const char* material = "");
