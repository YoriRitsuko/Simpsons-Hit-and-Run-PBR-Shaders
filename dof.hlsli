float CircleOfConfusion(float depth)
{
    if (depth >= 0.999999) return 1.0;
    float invZ = rcp(max(abs(ViewZ(depth)), 0.1));
    float farCoc = saturate((1.0 - passParameters.y * invZ) * 1.7);
    float nearCoc = saturate((passParameters.x * invZ - 1.10) * 1.7) * passParameters.z;
    return farCoc - nearCoc;
}

float4 DepthOfFieldSource(float2 uv : TEXCOORD0) : COLOR0
{
    float3 color = tex2D(sceneTexture, uv).rgb;
    float depth = tex2D(secondTexture, uv).r;
    return float4(color, CircleOfConfusion(depth));
}

float DofLayerWeight(float coc, float tapCoc, float distance, bool blurForeground)
{
    if (coc > 0.0 && tapCoc > 0.0) {
        // A defocused receiver gathers farther color across its silhouette.
        // A closer donor can reach the receiver within its own blur disk.
        float coverage = smoothstep(-0.07, 0.07, tapCoc - distance);
        float ratio = coc / max(tapCoc, 0.12);
        float closer = coverage * min(ratio * ratio, 4.0);
        return (tapCoc >= coc ? 1.0 : closer) * saturate(tapCoc * 64.0);
    }
    return blurForeground ? 1.0 - smoothstep(0.08, 0.40, abs(coc - tapCoc)) : 0.0;
}

float DofForegroundWeight(float coc, float tapCoc, float distance)
{
    return saturate((-tapCoc - distance) * 5.0 + 0.5) * smoothstep(0.05, 0.30, coc - tapCoc);
}

float4 SampleDof(float2 uv)
{
    float4 nearest = tex2Dlod(sceneTexture, float4(uv, 0, 0));
    float4 filtered = tex2Dlod(thirdTexture, float4(uv, 0, 0));
    // Bilinear color suppresses aliasing within a surface. At an edge, use
    // the point sample so the same surface supplies both color and defocus.
    if (abs(filtered.a - nearest.a) <= 0.0005) nearest.rgb = filtered.rgb;
    return nearest;
}

float4 DepthOfField(float2 uv : TEXCOORD0) : COLOR0
{
    float4 center = tex2D(sceneTexture, uv);
    float alpha = tex2D(secondTexture, uv).a;
    bool blurForeground = passParameters.x != 0.0;
    float coc = center.a;
    [branch] if (!blurForeground && coc <= 0.0) return float4(center.rgb, alpha);

    float radius = blurForeground ? 1.0 : coc;
    // Mirrored samples in equal-area annuli avoid a directional bias. The
    // angle advances by half the golden angle for each pair.
    const float2 rotation = float2(0.36237489, 0.93203242);
    float2 direction = float2(1.0, 0.0);
    float4 layer = float4(center.rgb, 1);
    float4 foreground = 0;
    [loop] for (int i = 0; i < 16; ++i) {
        float distance = sqrt((i + 0.2) / 16.0) * radius;
        float2 offset = direction * distance * passParameters.zw;
        float4 left = SampleDof(uv - offset);
        float4 right = SampleDof(uv + offset);
        float receiver = blurForeground ? saturate((abs(coc) - distance) * 5.0 + 0.5) : 1.0;
        layer += float4(left.rgb, 1) * (receiver * DofLayerWeight(coc, left.a, distance, blurForeground));
        layer += float4(right.rgb, 1) * (receiver * DofLayerWeight(coc, right.a, distance, blurForeground));
        [branch] if (blurForeground) {
            foreground += float4(left.rgb, 1) * DofForegroundWeight(coc, left.a, distance);
            foreground += float4(right.rgb, 1) * DofForegroundWeight(coc, right.a, distance);
        }
        direction = float2(dot(direction, float2(rotation.x, -rotation.y)), dot(direction, rotation.yx));
    }
    float coverage = saturate(foreground.a / 16.0);
    float3 color = lerp(layer.rgb / layer.a, foreground.rgb / max(foreground.a, 0.0001), coverage);
    return float4(color, alpha);
}
