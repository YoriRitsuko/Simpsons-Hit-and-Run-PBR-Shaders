row_major float4x4 clipTransform : register(c0);
row_major float4x4 viewTransform : register(c4);

struct VertexInput
{
    float4 position : POSITION0;
    float3 normal : NORMAL0;
    float2 uv : TEXCOORD0;
};

struct PixelInput
{
    float4 position : POSITION0;
    float2 uv : TEXCOORD0;
    float3 viewPosition : TEXCOORD1;
    float3 viewNormal : TEXCOORD2;
    float4 masks : TEXCOORD3;
};

PixelInput TransformVertex(VertexInput input, float4 masks)
{
    PixelInput output;
    output.position = mul(input.position, clipTransform);
    output.viewPosition = mul(input.position, viewTransform).xyz;
    output.viewNormal = mul(input.normal, (float3x3)viewTransform);
    output.uv = input.uv;
    output.masks = masks;
    return output;
}

PixelInput GlassVertex(VertexInput input)
{
    return TransformVertex(input, 1.0);
}

PixelInput PaintVertex(VertexInput input, float4 masks : COLOR0)
{
    return TransformVertex(input, masks);
}

PixelInput PbrVertex(VertexInput input)
{
    return TransformVertex(input, 1.0);
}

sampler2D albedoTexture : register(s0);
sampler2D normalTexture : register(s1);
sampler2D sphereTexture : register(s2);
sampler2D ormTexture : register(s3);
sampler2D emissiveTexture : register(s4);
sampler2D interiorTexture : register(s5);
float4 paintAndReflection : register(c0);
float4 lightAndNormal : register(c1);
float4 renderParameters : register(c2);
row_major float3x3 worldFromView : register(c3);
float4 pbrSurface : register(c6);
float4 pbrRender : register(c7);
float4 pbrLighting : register(c8);
float4 pbrLightDirections[8] : register(c9);
float4 pbrLightColors[8] : register(c17);
float4 materialColorSpace : register(c25);
float2 environmentSize : register(c26);
float4 interiorParameters : register(c27);
float2 windowGlass : register(c28);

float3 LinearRGB(float3 color);

float2 SphereUV(float3 reflected)
{
    // In this view space a head-on reflection points back toward negative Z.
    // Sample the middle of the sphere map there.
    float3 poleOffset = reflected - float3(0, 0, 1);
    return 0.5 + reflected.xy * float2(0.5, -0.5) *
        rsqrt(max(dot(poleOffset, poleOffset), 0.0001));
}

float Fresnel(float3 eye, float3 normal)
{
    return pow(1.0 - saturate(dot(eye, normal)), 5.0);
}

float3 UnitNormal(float3 direction)
{
    return direction * rsqrt(max(dot(direction, direction), 0.00000001));
}

float4 GlassPixel(PixelInput input) : COLOR0
{
    float3 normal = UnitNormal(input.viewNormal);
    float3 eye = UnitNormal(-input.viewPosition);
    float3 ray = reflect(-eye, normal);
    float2 uv = SphereUV(ray);
    if (renderParameters.z == 1.0) {
        ray = UnitNormal(mul(ray, worldFromView));
        uv = float2(atan2(ray.x, ray.z) * 0.15915494 + 0.5,
                    acos(clamp(ray.y, -1.0, 1.0)) * 0.31830989);
    } else if (renderParameters.z == 2.0) {
        // The game's EnvMap uses the reflection vector's X and Y directly.
        uv = 0.5 + ray.xy * float2(0.5, -0.5);
    }
    float3 reflected = tex2D(sphereTexture, uv).rgb;
    float amount = min(0.60, (0.22 + 0.78 * Fresnel(eye, normal)) * paintAndReflection.w);
    if (renderParameters.w) {
        // Let the original alpha test keep masking the cutouts.
        float coverage = renderParameters.x ? tex2D(albedoTexture, input.uv).a : 1;
        return float4(reflected * amount, coverage);
    }
    return float4(reflected, amount);
}

float3 PerturbNormal(PixelInput input, float3 normal)
{
    // Remove the magnitude shared by each derivative pair. Screen derivatives
    // shrink as a triangle fills more pixels, even with a valid UV mapping.
    float3 dx = ddx(input.viewPosition);
    float3 dy = ddy(input.viewPosition);
    float2 tu = ddx(input.uv);
    float2 tv = ddy(input.uv);
    float positionScale = rsqrt(max(max(dot(dx, dx), dot(dy, dy)), 1e-20));
    float uvScale = rsqrt(max(max(dot(tu, tu), dot(tv, tv)), 1e-20));
    dx *= positionScale;
    dy *= positionScale;
    tu *= uvScale;
    tv *= uvScale;

    // Keep the UV orientation on mirrored islands.
    float determinant = tu.x * tv.y - tu.y * tv.x;
    float3 tangent = (dx * tv.y - dy * tu.y) * sign(determinant);
    tangent -= normal * dot(tangent, normal);
    float3 bitangent = (dy * tu.x - dx * tv.x) * sign(determinant);
    float handedness = dot(cross(normal, tangent), bitangent) < 0 ? -1.0 : 1.0;
    float3 sampleNormal = tex2D(normalTexture, input.uv).xyz * 2.0 - 1.0;
    sampleNormal.xy *= lightAndNormal.w;
    float3 perturbed = UnitNormal(UnitNormal(tangent) * sampleNormal.x +
        UnitNormal(cross(normal, tangent)) * handedness * sampleNormal.y +
        normal * max(sampleNormal.z, 0.001));
    return abs(determinant) > 0.000001 && dot(tangent, tangent) > 0.00000001 ? perturbed : normal;
}

float4 PaintPixel(PixelInput input) : COLOR0
{
    float4 base = tex2D(albedoTexture, input.uv);
    float3 normal = PerturbNormal(input, UnitNormal(input.viewNormal));
    float3 eye = UnitNormal(-input.viewPosition);
    float3 light = lightAndNormal.xyz;
    float3 halfVector = UnitNormal(light + eye);
    float shine = saturate(input.masks.g);
    float paint = saturate(input.masks.b);
    float ao = saturate(input.masks.a);
    float3 tint = materialColorSpace.x ? LinearRGB(paintAndReflection.rgb) : paintAndReflection.rgb;
    float3 albedo = base.rgb * lerp(1.0, tint, paint);
    float diffuse = saturate(dot(normal, light));
    float exponent = lerp(8.0, 112.0, shine);
    float specular = pow(saturate(dot(normal, halfVector)), exponent) *
        lerp(0.15, 0.6, shine) * step(0.001, diffuse);
    float3 reflected = tex2D(sphereTexture, SphereUV(reflect(-eye, normal))).rgb;
    float reflection = paintAndReflection.w * shine * (0.2 + 0.8 * Fresnel(eye, normal));
    return float4(albedo * (0.32 * ao + 0.68 * diffuse) +
        specular + reflected * reflection * ao, base.a * renderParameters.y);
}

float3 LinearRGB(float3 color)
{
    return lerp(color / 12.92, pow(abs((color + 0.055) / 1.055), 2.4), step(0.04045, color));
}

float3 DisplayRGB(float3 color)
{
    return lerp(12.92 * color, 1.055 * pow(abs(color), 1.0 / 2.4) - 0.055, step(0.0031308, color));
}

float SmithVisibility(float NoV, float NoL, float alphaSquared)
{
    float viewTerm = NoL * sqrt(NoV * NoV * (1.0 - alphaSquared) + alphaSquared);
    float lightTerm = NoV * sqrt(NoL * NoL * (1.0 - alphaSquared) + alphaSquared);
    return 0.5 / max(viewTerm + lightTerm, 0.0001);
}

float3 MappedInterior(PixelInput input, float3 normal, float3 view)
{
    float2 roomUv = input.uv * interiorParameters.yz;
    float3 dx = ddx(input.viewPosition);
    float3 dy = ddy(input.viewPosition);
    float2 du = ddx(input.uv) * interiorParameters.yz;
    float2 dv = ddy(input.uv) * interiorParameters.yz;
    float3 across = cross(dy, normal);
    float3 down = cross(normal, dx);
    float det = dot(dx, across);
    float inverseDet = (det >= 0 ? 1.0 : -1.0) / max(abs(det), 1e-12);
    float3 gradientU = (across * du.x + down * dv.x) * inverseDet;
    float3 gradientV = (across * du.y + down * dv.y) * inverseDet;

    // The UV gradients convert the view ray into room coordinates even
    // when the facade is mirrored or the window is wider than it is tall.
    float depth = interiorParameters.x;
    float3 ray = float3(dot(-view, gradientU), dot(-view, gradientV), max(dot(view, normal), 0.0001) / depth);
    float2 entry = frac(roomUv);
    float2 wall = step(0.0, ray.xy);
    float2 wallTime = abs(wall - entry) / max(abs(ray.xy), 0.00001);
    float hitTime = min(1.0 / ray.z, min(wallTime.x, wallTime.y));
    float3 hit = saturate(float3(entry, 0) + ray * hitTime);

    // Reproject the box hit from the center of the room where the map was
    // captured. A flat spheremap lookup alone would not give room parallax.
    float2 roomSize = rcp(max(float2(length(gradientU), length(gradientV)), 0.015625));
    float3 fromCenter = (hit - 0.5) * float3(roomSize.x, -roomSize.y, -depth);
    float4 room = tex2Dlod(interiorTexture, float4(SphereUV(UnitNormal(fromCenter)), 0, 0));
    return LinearRGB(room.rgb) * (interiorParameters.w + room.a * pbrRender.w);
}

struct PbrOutput
{
    float4 color : COLOR0;
    float4 material : COLOR1;
    float4 fallback : COLOR2;
    float4 baseReflectance : COLOR3;
};

PbrOutput ShadePbr(PixelInput input)
{
    float4 base = tex2D(albedoTexture, input.uv);
    float coverage = saturate(base.a * pbrSurface.x);

    float3 albedo = LinearRGB(base.rgb);
    float3 packed = tex2D(ormTexture, input.uv).rgb;
    float ao = packed.r;
    float roughness = clamp(pbrSurface.w ? packed.g : pbrRender.x, 0.045, 1.0);
    float metallic = saturate(pbrSurface.w ? packed.b : pbrRender.y);
    float3 v = UnitNormal(-input.viewPosition);
    float3 geometricNormal = UnitNormal(input.viewNormal);
    if (interiorParameters.x > 0.0 && dot(geometricNormal, v) < 0.0) geometricNormal = -geometricNormal;
    float3 n = PerturbNormal(input, geometricNormal);
    // Integrate the GGX lobe over the variation of the shading normal across
    // a pixel. A polished surface can otherwise produce a subpixel spike.
    float3 normalDx = ddx(n);
    float3 normalDy = ddy(n);
    float variance = 0.5 * (dot(normalDx, normalDx) + dot(normalDy, normalDy));
    float3 reflected = mul(reflect(-v, n), worldFromView);
    float2 reflectionUv = SphereUV(reflected);
    float2 reflectionDx = ddx(reflectionUv) * environmentSize;
    float2 reflectionDy = ddy(reflectionUv) * environmentSize;
    float footprint = max(dot(reflectionDx, reflectionDx), dot(reflectionDy, reflectionDy));
    float reflectionMip = 0.5 * log2(max(footprint, 1.0));
    float NoV = max(saturate(dot(n, v)), 0.0001);
    float3 f0 = lerp(0.04, albedo, metallic);
    float alpha = roughness * roughness;
    float alphaSquared = saturate(alpha * alpha + min(variance, 0.02));
    float3 direct = 0.0;
    [loop]
    for (int index = 0; index < (int)pbrLighting.y; ++index) {
        float3 l = pbrLightDirections[index].xyz;
        float NoL = saturate(dot(n, l));
        if (NoL > 0) {
            float3 h = UnitNormal(v + l);
            float NoH = saturate(dot(n, h));
            float VoH = saturate(dot(v, h));
            float3 f = f0 + (1.0 - f0) * pow(1.0 - VoH, 5.0);
            // Keep the GGX peak at low roughness without subtracting nearly equal numbers.
            float ggx = (1.0 - NoH * NoH) + NoH * NoH * alphaSquared;
            float distribution = alphaSquared / (3.14159265 * ggx * ggx);
            float3 specular = distribution * SmithVisibility(NoV, NoL, alphaSquared) * f;
            float3 diffuse = (1.0 - f) * (1.0 - metallic) * albedo * 0.31830989;
            direct += (diffuse + specular) * pbrLightColors[index].rgb * NoL;
        }
    }

    float3 normalWorld = mul(n, worldFromView);
    float topMip = pbrLighting.x;
    float mip = min(topMip, max(roughness * topMip, reflectionMip));
    float3 radiance = tex2Dlod(sphereTexture, float4(reflectionUv, 0, mip)).rgb;
    float3 ambient = tex2Dlod(sphereTexture, float4(SphereUV(normalWorld), 0, topMip)).rgb;
    if (materialColorSpace.y) {
        radiance = LinearRGB(radiance);
        ambient = LinearRGB(ambient);
    }
    float3 grazing = max(1.0 - roughness, f0);
    float3 environmentF = f0 + (grazing - f0) * pow(1.0 - NoV, 5.0);
    float3 response = environmentF * (ao * pbrLighting.z);
    float3 fallback = radiance * response;
    float3 indirect = fallback + ambient * albedo * (1.0 - environmentF) * (1.0 - metallic) * ao * pbrLighting.z;
    float3 emission = LinearRGB(tex2D(emissiveTexture, input.uv).rgb) * pbrRender.w;
    float3 color = direct + indirect + emission;
    float windowMask = 0.0;
    [branch] if (interiorParameters.x > 0.0) {
        windowMask = base.a;
        float3 room = MappedInterior(input, geometricNormal, v);
        float glassMip = min(topMip, max(windowGlass.x * topMip, reflectionMip));
        float3 glassReflection = tex2Dlod(sphereTexture, float4(reflectionUv, 0, glassMip)).rgb;
        if (materialColorSpace.y) glassReflection = LinearRGB(glassReflection);
        float fresnel = windowGlass.y + (1.0 - windowGlass.y) * Fresnel(v, n);
        float3 throughGlass = lerp(room, glassReflection * pbrLighting.z, fresnel);
        color = lerp(color, throughGlass, windowMask);
    }
    // Keep neighboring lanes available for derivatives at alpha cutouts.
    if (pbrSurface.z == 1.0) clip(coverage - pbrSurface.y);
    color *= pbrRender.z;
    if (pbrLighting.w == 0.0) color = DisplayRGB(color / (1.0 + color));

    float2 oct = n.xy / (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0) oct = (1.0 - abs(oct.yx)) * (oct >= 0 ? 1.0 : -1.0);
    PbrOutput output;
    output.color = float4(color, pbrSurface.z == 2.0 ? coverage : 1.0);
    // The room and its glass already have lighting and reflection. Keep
    // their depth but leave these pixels out of the SSR and SSGI resolves.
    float window = step(0.001, windowMask);
    output.material = float4(oct, lerp(roughness, 1.0, window), input.viewPosition.z);
    // SSR reconstructs the same Fresnel response from albedo and metalness.
    // SSGI uses that pair to tint the bounce and exclude pure metals.
    output.fallback = float4(fallback * pbrRender.z, ao * pbrLighting.z * pbrRender.z);
    output.baseReflectance = float4(albedo, lerp(metallic, 1.0, window));
    return output;
}

float4 PbrPixel(PixelInput input) : COLOR0
{
    return ShadePbr(input).color;
}

PbrOutput PbrReflectionPixel(PixelInput input)
{
    return ShadePbr(input);
}
