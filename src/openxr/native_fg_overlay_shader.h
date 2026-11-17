#pragma once
namespace edvr::openxr {
// SDR SRVs decode into linear light. Compose native premultiplied coverage
// after FG and before the one output encode. No UI reconstruction by NGX.
inline constexpr char kStereoNativeUiHlsl[]=R"HLSL(
cbuffer NativeUi : register(b0) {
  float4 bounds; float4 clampUV; float encodeSRGB; float3 pad;
  float4 rot0; float4 rot1; float4 rot2;
  float4 destinationTan; float4 sourceTan;
};
Texture2D<float4> worldTexture : register(t0);
Texture2D<float4> uiTexture : register(t1);
SamplerState worldSampler : register(s0);
SamplerState uiSampler : register(s1);
struct O { float4 position:SV_POSITION; float2 uv:TEXCOORD; };
float3 srgb(float3 c) { return lerp(12.92*c,1.055*pow(max(c,0),1.0/2.4)-0.055,step(0.0031308,c)); }
float4 ps(O v):SV_TARGET {
  float2 worldUV=clamp(bounds.xy+v.uv*(bounds.zw-bounds.xy),clampUV.xy,clampUV.zw);
  float4 world=worldTexture.Sample(worldSampler,worldUV);
  float3 d=float3(lerp(destinationTan.x,destinationTan.y,v.uv.x),lerp(destinationTan.z,destinationTan.w,v.uv.y),-1);
  float3 r=float3(dot(rot0.xyz,d),dot(rot1.xyz,d),dot(rot2.xyz,d));
  float4 ui=0;
  if(r.z < -1e-6) {
    float2 sourceUV=float2((r.x/-r.z-sourceTan.x)/(sourceTan.y-sourceTan.x),
      (sourceTan.z-r.y/-r.z)/(sourceTan.z-sourceTan.w));
    if(all(sourceUV>=0)&&all(sourceUV<=1))ui=uiTexture.SampleLevel(uiSampler,sourceUV,0);
  }
  float3 color=ui.rgb+world.rgb*(1-ui.a);
  return float4(encodeSRGB>.5?srgb(color):color,world.a);
}
)HLSL";
} // namespace edvr::openxr
