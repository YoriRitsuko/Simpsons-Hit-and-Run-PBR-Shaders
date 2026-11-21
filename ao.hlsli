float2 DepthPixelCentre(float2 uv)
{
    return (floor(uv / occlusionGrid.zw) + 0.5) * occlusionGrid.zw;
}

float ReadOcclusionDepth(float2 uv)
{
    return tex2Dlod(occlusionDepth, float4(uv, 0, 0)).r;
}

float3 OcclusionNormal(float2 uv, float3 position, float3 view)
{
    float2 pixel = occlusionGrid.zw;
    float3 dx = DepthTangent(occlusionDepth, pixel, uv, float2(pixel.x, 0), position);
    float3 dy = DepthTangent(occlusionDepth, pixel, uv, float2(0, pixel.y), position);
    float3 normal = cross(dx, dy);
    if (dot(normal, normal) < 1e-20) return view;
    return normalize(dot(normal, view) < 0 ? -normal : normal);
}

float HorizonSample(float2 uv, float3 origin, float3 view, float openHorizon, float radius)
{
    if (any(uv <= 0) || any(uv >= 1)) return openHorizon;
    uv = DepthPixelCentre(uv);
    float depth = ReadOcclusionDepth(uv);
    if (depth >= 0.999999) return openHorizon;
    float3 delta = ViewPosition(uv, ViewZ(depth)) - origin;
    float distanceSquared = dot(delta, delta);
    float fade = saturate((1.0 - distanceSquared / (radius * radius)) * 1.5);
    float horizon = dot(delta, view) * rsqrt(max(distanceSquared, 1e-10));
    return lerp(openHorizon, max(openHorizon, horizon), fade);
}

float VisibilityArc(float horizon, float normalAngle, float normalCos, float normalSin)
{
    return 0.25 * (normalCos + 2.0 * horizon * normalSin - cos(2.0 * horizon - normalAngle));
}

float4 HorizonOcclusion(float2 uv : TEXCOORD0) : COLOR0
{
    float2 gridPosition = floor(uv / occlusionGrid.xy);
    uv = DepthPixelCentre(uv);
    float depth = ReadOcclusionDepth(uv);
    if (depth >= 0.999999) return float4(1, 0, 0, 0);

    float z = ViewZ(depth);
    float3 position = ViewPosition(uv, z);
    float3 view = normalize(-position);
    float3 normal = OcclusionNormal(uv, position, view);
    float3 origin = position + normal * (0.009 + abs(z) * 0.00025);
    float radius = occlusionControls.x;
    float clipW = z * projection[2][3] + projection[3][3];
    float2 radiusInUv = float2(projection[0][0], -projection[1][1]) * (0.5 * radius / clipW);
    float spin = frac(dot(gridPosition, float2(0.75487766, 0.56984029)));
    float jitter = frac(dot(gridPosition, float2(0.43857902, 0.81917251)));
    int slices = (int)occlusionControls.z;
    int steps = (int)occlusionControls.w;
    float openVisibility = 0;
    float visible = 0;
    // Integrate the cosine-weighted visible arc on each view-aligned slice.
    // See Jimenez et al., Practical Realtime Strategies for Accurate Indirect Occlusion, Eq. 7.
    [loop] for (int slice = 0; slice < slices; ++slice) {
        float s, c;
        sincos(3.14159265 * (slice + spin) / slices, s, c);
        float3 axis = float3(c, s, 0);
        float3 tangent = normalize(axis - view * dot(axis, view));
        float2 normalInSlice = float2(dot(normal, view), dot(normal, tangent));
        float normalLength = max(length(normalInSlice), 1e-5);
        float normalCos = saturate(normalInSlice.x / normalLength);
        float normalSin = clamp(normalInSlice.y / normalLength, -1.0, 1.0);
        float normalAngle = acos(normalCos) * (normalSin < 0 ? -1.0 : 1.0);

        // Start at the unoccluded tangent limits. A flat surface then
        // integrates to full visibility even at a steep viewing angle.
        float2 openHorizon = float2(-normalSin, normalSin);
        float2 horizon = openHorizon;
        float2 previous = openHorizon;
        float2 reach = float2(c, s) * radiusInUv;
        float pixels = max(length(reach / occlusionGrid.zw), 1.0);
        float limit = min(pixels, 96.0);
        reach *= limit / pixels;
        [loop] for (int step = 0; step < steps; ++step) {
            float t = (step + 0.5 + 0.5 * jitter) / steps;
            float2 offset = reach * lerp(1.25 / max(limit, 1.25), 1.0, t * t);
            float2 sampleHorizon = float2(HorizonSample(uv + offset, origin, view, openHorizon.x, radius),
                                          HorizonSample(uv - offset, origin, view, openHorizon.y, radius));
            // A one-sample spike is a weaker occluder than a sustained edge.
            horizon = max(horizon, lerp(previous, sampleHorizon, 0.76));
            previous = sampleHorizon;
        }
        float upper = acos(clamp(horizon.x, -1.0, 1.0));
        float lower = -acos(clamp(horizon.y, -1.0, 1.0));
        visible += normalLength * (VisibilityArc(upper, normalAngle, normalCos, normalSin) +
                                   VisibilityArc(lower, normalAngle, normalCos, normalSin));
        openVisibility += normalLength * (normalCos + normalAngle * normalSin);
    }
    float visibility = saturate(visible / max(openVisibility, 1e-5));
    float projectedSize = max(abs(radiusInUv.x / occlusionGrid.z), abs(radiusInUv.y / occlusionGrid.w));
    visibility = lerp(1.0, visibility, smoothstep(1.25, 4.0, projectedSize));
    return float4(visibility, EncodeNormal(normal), z);
}

float OcclusionWeight(float2 uv, float4 packed, float3 position, float3 normal)
{
    if (packed.a == 0 || any(uv <= 0) || any(uv >= 1)) return 0;
    float3 neighbour = ViewPosition(DepthPixelCentre(uv), packed.a);
    float separation = abs(dot(neighbour - position, normal));
    float samePlane = saturate(1.0 - separation / (0.025 + abs(position.z) * 0.0015));
    return samePlane * smoothstep(0.65, 0.96, dot(normal, DecodeNormal(packed.gb)));
}

float4 FilterOcclusion(float2 uv : TEXCOORD0) : COLOR0
{
    float4 centre = tex2D(occlusionTexture, uv);
    if (centre.a == 0) return centre;
    float3 position = ViewPosition(DepthPixelCentre(uv), centre.a);
    float3 normal = DecodeNormal(centre.gb);
    float sum = centre.r;
    float weights = 1;
    [loop] for (int tap = -3; tap <= 3; ++tap) {
        if (tap == 0) continue;
        float2 neighbour = uv + tap * passParameters.xy;
        float4 packed = tex2Dlod(occlusionTexture, float4(neighbour, 0, 0));
        float weight = exp2(-0.22 * tap * tap) * OcclusionWeight(neighbour, packed, position, normal);
        sum += weight * packed.r;
        weights += weight;
    }
    return float4(sum / weights, centre.gba);
}

float AmbientVisibility(float2 uv)
{
    float depth = ReadOcclusionDepth(uv);
    if (depth >= 0.999999) return 1;

    float3 position = ViewPosition(uv, ViewZ(depth));
    float3 normal = OcclusionNormal(uv, position, normalize(-position));
    float2 pixel = occlusionGrid.xy;
    float2 centre = (floor(uv / pixel) + 0.5) * pixel;
    float sum = 0;
    float weights = 0;
    [loop] for (int y = -1; y <= 1; ++y) {
        [loop] for (int x = -1; x <= 1; ++x) {
            float2 neighbour = centre + float2(x, y) * pixel;
            float4 packed = tex2Dlod(occlusionTexture, float4(neighbour, 0, 0));
            float2 tent = max(0, 1.45 - abs((neighbour - uv) / pixel));
            float weight = tent.x * tent.y * OcclusionWeight(neighbour, packed, position, normal);
            sum += weight * packed.r;
            weights += weight;
        }
    }
    return weights > 0.05 ? sum / weights : 1.0;
}
