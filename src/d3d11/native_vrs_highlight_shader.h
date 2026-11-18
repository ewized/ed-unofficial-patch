#pragma once

namespace edvr {
// Visualize the actual tile indices from the world pass on the resolved eye.
// Resolve pixel centres back to the full input, accounting for render jitter.
// Palette 0 and unused entries are full rate. Preserve the eye's alpha.
inline constexpr char kNativeVrsHighlightHlsl[] = R"hlsl(
Texture2D<float4> World : register(t0);
Texture2D<uint> RateMap : register(t1);
RWTexture2D<float4> Highlight : register(u0);
cbuffer Geometry : register(b0) {
    uint2 OutputSize;
    uint2 InputSize;
    float2 RenderedJitter;
    float2 Padding;
};

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= OutputSize)) return;
    float2 position = (float2(id.xy) + 0.5) * float2(InputSize) / float2(OutputSize) + RenderedJitter;
    uint2 tile = uint2(clamp(position, 0.0, float2(InputSize - 1))) / 16;
    uint rate = RateMap.Load(int3(tile, 0));
    float4 color = World.Load(int3(id.xy, 0));
    if (rate == 1 || rate == 2) color.rgb = lerp(color.rgb, float3(0.10, 0.65, 1.0), 0.26);
    Highlight[id.xy] = color;
}
)hlsl";
}
