float CircleOfConfusion(float depth)
{
    if (depth >= 0.999999) return 1.0;
    float z = max(abs(ViewZ(depth)), 0.1);
    float focus = passParameters.x;
    float offset = 1.0 - focus / z;
    // Keep a small sharp region around the focus plane.
    return sign(offset) * saturate((abs(offset) - 0.10) * 1.7);
}

float4 DepthOfField(float2 uv : TEXCOORD0) : COLOR0
{
    float4 scene = tex2D(sceneTexture, uv);
    float depth = tex2D(secondTexture, uv).r;
    float coc = CircleOfConfusion(depth);
    static const float2 disk[12] = {
        float2(0.28, 0.00), float2(0.00, 0.28), float2(-0.28, 0.00), float2(0.00, -0.28),
        float2(0.74, 0.31), float2(0.31, 0.74), float2(-0.31, 0.74), float2(-0.74, 0.31),
        float2(-0.74, -0.31), float2(-0.31, -0.74), float2(0.31, -0.74), float2(0.74, -0.31)
    };
    float4 layer = float4(scene.rgb, 1);
    float4 foreground = 0;
    [loop] for (int i = 0; i < 12; ++i) {
        float2 tapUv = uv + disk[i] * passParameters.zw;
        float tapDepth = tex2Dlod(secondTexture, float4(tapUv, 0, 0)).r;
        float tapCoc = CircleOfConfusion(tapDepth);
        float distance = length(disk[i]);
        float3 tap = tex2Dlod(sceneTexture, float4(tapUv, 0, 0)).rgb;

        // Limit the gather to surfaces with a similar blur radius. Foreground
        // bokeh may extend across a farther surface and has its own weight.
        float receiver = saturate((abs(coc) - distance) * 5.0 + 0.5);
        float sameLayer = 1.0 - smoothstep(0.08, 0.40, abs(coc - tapCoc));
        layer += float4(tap, 1) * (receiver * sameLayer);
        float closer = smoothstep(0.05, 0.30, coc - tapCoc);
        float near = saturate((-tapCoc - distance) * 5.0 + 0.5) * closer;
        foreground += float4(tap, 1) * (near * step(tapDepth, depth));
    }
    float alpha = saturate(foreground.a / 6.0);
    float3 color = lerp(layer.rgb / layer.a, foreground.rgb / max(foreground.a, 0.0001), alpha);
    return float4(color, scene.a);
}
