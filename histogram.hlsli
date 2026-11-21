float3 DecodeSrgb(float3 display)
{
    return lerp(display / 12.92, pow(abs((display + 0.055) / 1.055), 2.4),
                step(0.04045, display));
}

float DisplayLuminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float4 HistogramMeter(float2 uv : TEXCOORD0) : COLOR0
{
    float3 color = DecodeSrgb(saturate(tex2D(sceneTexture, uv).rgb));
    float luminance = EncodeSrgb(DisplayLuminance(color).xxx).r;
    float bin = min(floor(luminance * passParameters.x), passParameters.x - 1.0);
    return float4(bin * passParameters.y, 0, 0, 1);
}

float4 HistogramRows(float2 uv : TEXCOORD0) : COLOR0
{
    // Each pixel counts the samples up to one bin in one meter row.
    float bin = floor(uv.x * passParameters.z) * passParameters.w;
    float cdf = 0;
    float darkest = 1;
    float brightest = 0;
    [loop] for (int x = 0; x < (int)passParameters.x; ++x) {
        float u = (x + 0.5) * passParameters.y;
        float value = tex2Dlod(sceneTexture, float4(u, uv.y, 0, 0)).r;
        cdf += step(value, bin);
        darkest = min(darkest, value);
        brightest = max(brightest, value);
    }
    return float4(cdf * passParameters.y, darkest, brightest, 1);
}

float4 HistogramCurve(float2 uv : TEXCOORD0) : COLOR0
{
    float cdf = 0;
    float darkest = 1;
    float brightest = 0;
    [loop] for (int y = 0; y < (int)passParameters.x; ++y) {
        float v = (y + 0.5) * passParameters.y;
        float3 row = tex2Dlod(sceneTexture, float4(uv.x, v, 0, 0)).rgb;
        cdf += row.r;
        darkest = min(darkest, row.g);
        brightest = max(brightest, row.b);
    }
    float cdfMin = 0;
    float darkestU = darkest + 0.5 * passParameters.z;
    [loop] for (int row = 0; row < (int)passParameters.x; ++row) {
        float v = (row + 0.5) * passParameters.y;
        cdfMin += tex2Dlod(sceneTexture, float4(darkestU, v, 0, 0)).r;
    }
    float equalized = saturate((cdf - cdfMin) / max(passParameters.x - cdfMin, 0.001));
    // Suppress the curve on a flat frame and ease it in on very narrow ranges.
    float rangeWeight = smoothstep(0.025, 0.125, brightest - darkest);
    float mapped = lerp(uv.x, equalized, rangeWeight);
    [branch] if (passParameters.w < 1.0) {
        float previous = tex2Dlod(secondTexture, float4(uv.x, 0.5, 0, 0)).r;
        mapped = lerp(previous, mapped, passParameters.w);
    }
    return float4(mapped, 0, 0, 1);
}

float4 ApplyHistogram(float2 uv : TEXCOORD0) : COLOR0
{
    float4 scene = tex2D(sceneTexture, uv);
    float3 linearColor = DecodeSrgb(saturate(scene.rgb));
    float luminance = DisplayLuminance(linearColor);
    float perceptual = EncodeSrgb(luminance.xxx).r;
    float edge = passParameters.y;
    float sampleX = clamp(perceptual, edge, 1.0 - edge);
    float mapped = tex2D(secondTexture, float2(sampleX, 0.5)).r + (perceptual - sampleX);
    float outputLuminance = DecodeSrgb(lerp(perceptual, saturate(mapped), passParameters.x).xxx).r;
    float3 color = linearColor * (outputLuminance / max(luminance, 0.000001));
    float peak = max(color.r, max(color.g, color.b));
    // Move only out-of-gamut colours toward neutral at the same luminance.
    float chroma = saturate((1.0 - outputLuminance) / max(peak - outputLuminance, 0.000001));
    color = lerp(outputLuminance, color, chroma);
    return float4(EncodeSrgb(saturate(color)), scene.a);
}
