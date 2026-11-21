sampler2D sceneTexture : register(s0);
sampler2D secondTexture : register(s1);
sampler2D thirdTexture : register(s2);
sampler2D fourthTexture : register(s3);
sampler2D fifthTexture : register(s4);
sampler2D acesTexture : register(s5);
sampler2D irradianceTexture : register(s6);
sampler2D staticTexture : register(s7);
sampler2D occlusionTexture : register(s8);
sampler2D occlusionDepth : register(s9);

float4 passParameters : register(c0);
row_major float4x4 projection : register(c1);
float4 indirectControls : register(c5);
float4 occlusionControls : register(c6);
float4 occlusionGrid : register(c7);
float4 occlusionMode : register(c8);
float4 skyView : register(c9);

float AmbientVisibility(float2 uv);

float4 SkyBrightness(float2 uv : TEXCOORD0) : COLOR0
{
    float4 sky = tex2D(sceneTexture, uv);
    if (all(uv >= skyView.xy) && all(uv < skyView.zw)) sky.rgb *= passParameters.x;
    return sky;
}

float3 BloomSample(float2 uv)
{
    float3 color = tex2D(sceneTexture, uv).rgb;
    float brightness = max(color.r, max(color.g, color.b));
    float knee = saturate(brightness - 0.5);
    float contribution = max(brightness - 1.0, knee * knee * 0.5);
    return color * (contribution / max(brightness, 0.0001));
}

float4 BloomDownsample(float2 uv : TEXCOORD0) : COLOR0
{
    if (passParameters.z != 0) return float4(BloomSample(uv), 1);
    float2 pixel = passParameters.xy;
    float4 centre = tex2D(sceneTexture, uv);
    float4 near = tex2D(sceneTexture, uv + pixel) + tex2D(sceneTexture, uv - pixel) +
                  tex2D(sceneTexture, uv + pixel * float2(1, -1)) +
                  tex2D(sceneTexture, uv + pixel * float2(-1, 1));
    float4 wide = tex2D(sceneTexture, uv + float2(2 * pixel.x, 0)) +
                  tex2D(sceneTexture, uv - float2(2 * pixel.x, 0)) +
                  tex2D(sceneTexture, uv + float2(0, 2 * pixel.y)) +
                  tex2D(sceneTexture, uv - float2(0, 2 * pixel.y));
    return centre * 0.25 + near * 0.125 + wide * 0.0625;
}

float3 CompositeHdr(float2 uv)
{
    float4 scene = tex2D(sceneTexture, uv);
    float3 hdr = scene.rgb;
    if (occlusionMode.x > 0) {
        // The stock forward renderer has already mixed its lights together.
        // Ease off the contact shading on bright HDR highlights and glows.
        float highlights = smoothstep(1.0, 3.0, max(hdr.r, max(hdr.g, hdr.b)));
        float amount = occlusionControls.y * saturate(scene.a) * (1.0 - highlights);
        hdr *= 1.0 - min(0.75, (1.0 - AmbientVisibility(uv)) * amount);
    }
    if (passParameters.x > 0) {
        float2 dx = float2(1.5 * passParameters.y, 0);
        float2 dy = float2(0, 1.5 * passParameters.z);
        float3 tight = tex2D(secondTexture, uv).rgb * 0.40 +
                       (tex2D(secondTexture, uv + dx).rgb + tex2D(secondTexture, uv - dx).rgb +
                        tex2D(secondTexture, uv + dy).rgb + tex2D(secondTexture, uv - dy).rgb) * 0.15;
        float3 glow = tight * 0.16 + tex2D(thirdTexture, uv).rgb * 0.34 +
                      tex2D(fourthTexture, uv).rgb * 0.30 + tex2D(fifthTexture, uv).rgb * 0.20;
        hdr += glow * passParameters.x;
    }
    return max(hdr, 0.0) * passParameters.w;
}

float4 AcesComposite(float2 uv : TEXCOORD0) : COLOR0
{
    if (occlusionMode.x > 1.5) return float4(AmbientVisibility(uv).xxx, 1);
    // Same shaper and lattice as tools/bake-aces-lut.py. The table stores
    // display-encoded sRGB, so the final draw leaves sRGB writes disabled.
    float3 shaped = saturate(log2(1.0 + 32.0 * CompositeHdr(uv)) / log2(2049.0)) * 63.0;
    float lower = floor(shaped.z);
    float upper = min(lower + 1.0, 63.0);
    float2 tile0 = float2(fmod(lower, 8.0), floor(lower / 8.0));
    float2 tile1 = float2(fmod(upper, 8.0), floor(upper / 8.0));
    float3 rgb0 = tex2D(acesTexture, (tile0 * 64.0 + shaped.xy + 0.5) / 512.0).rgb;
    float3 rgb1 = tex2D(acesTexture, (tile1 * 64.0 + shaped.xy + 0.5) / 512.0).rgb;
    return float4(lerp(rgb0, rgb1, frac(shaped.z)), 1);
}

float3 NeutralToneMap(float3 color)
{
    // Khronos PBR Neutral on linear Rec.709.
    // https://github.com/KhronosGroup/ToneMapping/tree/main/PBR_Neutral
    float darkest = min(color.r, min(color.g, color.b));
    float blackOffset = darkest < 0.08 ? darkest * (1.0 - 6.25 * darkest) : 0.04;
    float3 shifted = color - blackOffset;
    float peak = max(shifted.r, max(shifted.g, shifted.b));
    const float shoulder = 0.76;
    if (peak <= shoulder) return shifted;

    const float headroom = 1.0 - shoulder;
    float excess = peak - shoulder;
    float compressed = shoulder + headroom * excess / (headroom + excess);
    float saturation = 1.0 / (1.0 + 0.15 * (peak - compressed));
    return compressed * lerp(1.0, shifted / peak, saturation);
}

float4 NeutralComposite(float2 uv : TEXCOORD0) : COLOR0
{
    if (occlusionMode.x > 1.5) return float4(AmbientVisibility(uv).xxx, 1);
    float3 color = saturate(NeutralToneMap(CompositeHdr(uv)));
    float3 srgb = lerp(12.92 * color, 1.055 * pow(color, 1.0 / 2.4) - 0.055, step(0.0031308, color));
    return float4(srgb, 1);
}

float ViewZ(float depth)
{
    return (projection[3][2] - depth * projection[3][3]) /
        (depth * projection[2][3] - projection[2][2]);
}

float3 ViewPosition(float2 uv, float z)
{
    float w = z * projection[2][3] + projection[3][3];
    float2 ndc = uv * float2(2, -2) + float2(-1, 1);
    float2 xy = (ndc * w - z * projection[2].xy - projection[3].xy) /
        float2(projection[0][0], projection[1][1]);
    return float3(xy, z);
}

float3 DecodeNormal(float2 folded)
{
    float3 normal = float3(folded, 1.0 - abs(folded.x) - abs(folded.y));
    normal.xy += (normal.xy >= 0 ? -1.0 : 1.0) * max(0, -normal.z);
    return normalize(normal);
}

float2 EncodeNormal(float3 normal)
{
    normal /= abs(normal.x) + abs(normal.y) + abs(normal.z);
    return normal.z < 0 ? (1.0 - abs(normal.yx)) * (normal.xy >= 0 ? 1 : -1) : normal.xy;
}

bool ProbeDepth(float3 position, float zSign, out float2 uv, out float depthDifference)
{
    float4 clip = mul(float4(position, 1), projection);
    uv = clip.xy / max(clip.w, 0.05) * float2(0.5, -0.5) + 0.5;
    depthDifference = -1e6;
    if (clip.w <= 0.05 || any(uv <= 0.0) || any(uv >= 1.0)) return false;
    float depth = tex2Dlod(secondTexture, float4(uv, 0, 0)).r;
    if (depth < 0.999999) depthDifference = (position.z - ViewZ(depth)) * zSign;
    return true;
}

bool VisibleMaterial(float2 uv, float4 material, out float z)
{
    z = ViewZ(tex2Dlod(secondTexture, float4(uv, 0, 0)).r);
    return material.w != 0 && abs(z - material.w) <= max(0.025, abs(z) * 0.0015);
}

float4 ScreenReflection(float2 uv : TEXCOORD0) : COLOR0
{
    float4 scene = tex2D(sceneTexture, uv);
    float4 material = tex2D(thirdTexture, uv);
    if (material.w == 0 || material.z >= 0.65 || scene.a <= 0.001) return scene;

    float z;
    if (!VisibleMaterial(uv, material, z)) return scene;
    float3 position = ViewPosition(uv, z);
    float3 normal = DecodeNormal(material.xy);
    float3 ray = reflect(normalize(position), normal);
    float3 start = position + normal * 0.06;
    int steps = (int)passParameters.z;
    float maxDistance = passParameters.w;
    float frontDistance = 0;
    float2 hit = uv;
    float confidence = 0;
    float zSign = sign(projection[2][3]);
    [loop] for (int step = 0; step < steps; ++step) {
        // Concentrate samples near the reflector, but always cover the full
        // configured distance. Refine each crossing against the depth buffer.
        float t = (step + 1.0) / steps;
        float distance = lerp(0.12, maxDistance, t * t);
        float2 candidate;
        float delta;
        if (!ProbeDepth(start + ray * distance, zSign, candidate, delta)) break;
        if (delta < 0) {
            frontDistance = distance;
            continue;
        }
        if (frontDistance == 0) continue;

        float backDistance = distance;
        [unroll] for (int refine = 0; refine < 4; ++refine) {
            float middle = (frontDistance + backDistance) * 0.5;
            float2 middleUv;
            float middleDelta;
            ProbeDepth(start + ray * middle, zSign, middleUv, middleDelta);
            if (middleDelta < 0) frontDistance = middle;
            else {
                backDistance = middle;
                candidate = middleUv;
                delta = middleDelta;
            }
        }
        if (delta < min(0.7, 0.22 + backDistance * 0.012) &&
            length((candidate - uv) / passParameters.xy) > 1.5) {
            hit = candidate;
            float2 edge = min(hit, 1.0 - hit);
            confidence = saturate(min(edge.x, edge.y) * 12.0) *
                         (1.0 - smoothstep(maxDistance * 0.60, maxDistance, backDistance));
            break;
        }
        frontDistance = 0;
    }

    if (confidence == 0) return scene;
    float2 spread = passParameters.xy * material.z * 7.0;
    float3 radiance = (tex2D(sceneTexture, hit + spread).rgb + tex2D(sceneTexture, hit - spread).rgb +
                       tex2D(sceneTexture, hit + spread * float2(1, -1)).rgb +
                       tex2D(sceneTexture, hit + spread * float2(-1, 1)).rgb) * 0.25;
    float4 fallback = tex2D(fourthTexture, uv);
    float4 base = tex2D(fifthTexture, uv);
    float3 f0 = lerp(0.04, base.rgb, base.a);
    float3 grazing = max(1.0 - material.z, f0);
    float NoV = max(saturate(dot(normal, normalize(-position))), 0.0001);
    float3 response = (f0 + (grazing - f0) * pow(1.0 - NoV, 5.0)) * fallback.a;
    float weight = saturate(scene.a) * confidence * (1.0 - smoothstep(0.1, 0.65, material.z));
    return float4(max(0.0, scene.rgb + weight * (radiance * response - fallback.rgb)), scene.a);
}

float3 DepthTangent(sampler2D depthMap, float2 pixel, float2 uv, float2 offset, float3 position)
{
    float2 left = uv - offset;
    float2 right = uv + offset;
    float leftDepth = tex2Dlod(depthMap, float4(left, 0, 0)).r;
    float rightDepth = tex2Dlod(depthMap, float4(right, 0, 0)).r;
    bool leftValid = all(left >= pixel * 0.25) && leftDepth < 0.999999;
    bool rightValid = all(right <= 1.0 - pixel * 0.25) && rightDepth < 0.999999;
    if (!leftValid && !rightValid) return ViewPosition(uv + offset, position.z) - position;
    float leftZ = ViewZ(leftDepth);
    float rightZ = ViewZ(rightDepth);
    return rightValid && (!leftValid || abs(rightZ - position.z) < abs(leftZ - position.z))
        ? ViewPosition(right, rightZ) - position : position - ViewPosition(left, leftZ);
}

float4 StaticIrradianceShape(float2 uv : TEXCOORD0) : COLOR0
{
    float4 pbr = tex2D(thirdTexture, uv);
    float z;
    if (VisibleMaterial(uv, pbr, z)) return pbr;

    float depth = tex2D(secondTexture, uv).r;
    float staticDepth = tex2D(staticTexture, uv).r;
    // Match the final Z buffer so foreground cars and characters can cover
    // the scenery. Two depth-buffer ticks leave room for driver rounding.
    if (depth >= 0.999999 || abs(depth - staticDepth) > 0.00000012) return 0;
    float3 position = ViewPosition(uv, z);
    float3 dx = DepthTangent(secondTexture, passParameters.zw, uv, float2(passParameters.z, 0), position);
    float3 dy = DepthTangent(secondTexture, passParameters.zw, uv, float2(0, passParameters.w), position);
    float3 normal = cross(dx, dy);
    float total = abs(normal.x) + abs(normal.y) + abs(normal.z);
    if (total < 1e-10) return 0;
    if (dot(normal, position) > 0) normal = -normal;
    // Negative roughness tags a stock receiver. This texture is SSGI-only.
    return float4(EncodeNormal(normal), -1, z);
}

float4 IndirectShape(float2 uv)
{
    if (indirectControls.w != 0) return tex2Dlod(staticTexture, float4(uv, 0, 0));
    return tex2Dlod(thirdTexture, float4(uv, 0, 0));
}

float3 DiffuseRay(float2 origin, float3 start, float3 ray, float zSign)
{
    const float radius = indirectControls.y;
    const int steps = 14 + (int)max(0.0, radius - 3.0);
    float front = 0;
    [loop] for (int step = 1; step <= steps; ++step) {
        float t = (float)step / steps;
        float distance = lerp(0.10, radius, t * t);
        float2 hit;
        float delta;
        if (!ProbeDepth(start + ray * distance, zSign, hit, delta)) break;
        if (delta < 0) {
            front = distance;
            continue;
        }
        if (front == 0) continue;

        float back = distance;
        [unroll] for (int refine = 0; refine < 3; ++refine) {
            float middle = (front + back) * 0.5;
            float2 probe;
            float difference;
            ProbeDepth(start + ray * middle, zSign, probe, difference);
            if (difference < 0) front = middle;
            else {
                back = middle;
                hit = probe;
                delta = difference;
            }
        }
        if (delta < 0.12 && length((hit - origin) / passParameters.zw) > 1.5) {
            float4 donor = IndirectShape(hit);
            float donorZ;
            if (VisibleMaterial(hit, donor, donorZ) && dot(DecodeNormal(donor.xy), ray) >= 0.0)
                return 0;
            float2 edge = min(hit, 1.0 - hit);
            float confidence = saturate(min(edge.x, edge.y) * 20.0);
            float falloff = 1.0 - smoothstep(radius * 0.2, radius, back);
            return clamp(tex2Dlod(sceneTexture, float4(hit, 0, 0)).rgb, 0.0, 8.0) * confidence * falloff;
        }
        front = 0;
    }
    return 0;
}

float4 ScreenIrradiance(float2 uv : TEXCOORD0) : COLOR0
{
    float4 material = IndirectShape(uv);
    float z;
    if (!VisibleMaterial(uv, material, z) || (material.z >= 0 && tex2D(fifthTexture, uv).a >= 0.999)) return 0;

    float3 position = ViewPosition(uv, z);
    float3 normal = DecodeNormal(material.xy);
    float3 axis = abs(normal.z) < 0.95 ? float3(0, 0, 1) : float3(0, 1, 0);
    float3 tangent = normalize(cross(axis, normal));
    float3 bitangent = cross(normal, tangent);
    float bias = min(0.10, 0.025 + abs(z) * 0.001);
    float3 start = position + normal * bias;
    float spin = frac(dot(floor(uv / passParameters.xy), float2(0.75487766, 0.56984029)));
    float3 radiance = 0;
    const int rays = (int)indirectControls.x;
    [loop] for (int i = 0; i < rays; ++i) {
        float r2 = (i + 0.5) / rays;
        float angle = frac(spin + i * 0.618034) * 6.2831853;
        float s, c;
        sincos(angle, s, c);
        // Cosine-weighted hemisphere. Missed rays still count toward the mean.
        float3 ray = (c * tangent + s * bitangent) * sqrt(r2) + normal * sqrt(1.0 - r2);
        radiance += DiffuseRay(uv, start, ray, sign(projection[2][3]));
    }
    return float4(radiance / rays, z);
}

float SurfaceWeight(float2 uv, float z, float3 position, float3 normal)
{
    if (z == 0 || any(uv <= 0.0) || any(uv >= 1.0)) return 0;
    float4 shape = IndirectShape(uv);
    float planeGap = abs(dot(normal, ViewPosition(uv, z) - position));
    float samePlane = saturate(1.0 - planeGap / (0.08 + abs(position.z) * 0.004));
    return samePlane * smoothstep(0.6, 0.95, dot(normal, DecodeNormal(shape.xy)));
}

float4 FilterIrradiance(float2 uv : TEXCOORD0) : COLOR0
{
    float4 centre = tex2D(irradianceTexture, uv);
    if (centre.a == 0) return centre;
    float3 position = ViewPosition(uv, centre.a);
    float3 normal = DecodeNormal(IndirectShape(uv).xy);
    float3 filtered = centre.rgb;
    float total = 1.0;
    [loop] for (int i = 1; i <= 5; ++i) {
        float2 offset = passParameters.zw * i;
        float kernel = exp2(-0.10 * i * i);
        float4 low = tex2Dlod(irradianceTexture, float4(uv - offset, 0, 0));
        float4 high = tex2Dlod(irradianceTexture, float4(uv + offset, 0, 0));
        float lowWeight = kernel * SurfaceWeight(uv - offset, low.a, position, normal);
        float highWeight = kernel * SurfaceWeight(uv + offset, high.a, position, normal);
        filtered += low.rgb * lowWeight + high.rgb * highWeight;
        total += lowWeight + highWeight;
    }
    return float4(filtered / total, centre.a);
}

float4 IndirectComposite(float2 uv : TEXCOORD0) : COLOR0
{
    float4 scene = tex2D(sceneTexture, uv);
    float4 material = IndirectShape(uv);
    float z;
    // Stock materials have no separate albedo buffer. Use the lit scene
    // color as a diffuse tint, capped to keep emissive values out of it.
    float4 base = material.z < 0 ? float4(saturate(scene.rgb), 0) : tex2D(fifthTexture, uv);
    if (scene.a <= 0.001 || !VisibleMaterial(uv, material, z) || base.a >= 0.999) return scene;

    float3 position = ViewPosition(uv, z);
    float3 normal = DecodeNormal(material.xy);
    float2 pixel = passParameters.xy;
    float2 centre = (floor(uv / pixel) + 0.5) * pixel;
    float3 gathered = 0;
    float weights = 0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            float2 offset = centre + float2(x, y) * pixel;
            float4 neighbourLight = tex2Dlod(irradianceTexture, float4(offset, 0, 0));
            float2 tent = max(0, 1.6 - abs((offset - uv) / pixel));
            float weight = tent.x * tent.y * SurfaceWeight(offset, neighbourLight.a, position, normal);
            gathered += neighbourLight.rgb * weight;
            weights += weight;
        }
    }
    float3 bounce = gathered / max(weights, 0.0001) * base.rgb * (1.0 - base.a);
    return float4(scene.rgb + bounce * (indirectControls.z * saturate(scene.a)), scene.a);
}

#include "ao.hlsli"
