// Fullscreen-quad VS + textured PS. The PS samples a 2D texture with
// point (nearest) filtering for pixel-perfect output.
//
// rect    = NDC rect to draw to: x0, y0, x1, y1
// uvscale = portion of the source texture to sample: scale_u, scale_v, 0, 0
//
// Build with fxc:
//   fxc /T vs_4_0 /E VSMain /Fo quad_vs.cso quad.hlsl
//   fxc /T ps_4_0 /E PSMain /Fo quad_ps.cso quad.hlsl

cbuffer Transform : register(b0) {
    float4 rect;
    float4 uvscale;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
    // Triangle strip: 4 verts forming a quad over `rect`.
    float2 uv = float2((id & 1), (id >> 1)); // (0,0) (1,0) (0,1) (1,1)
    float2 ndc = lerp(rect.xy, rect.zw, uv);
    VSOut o;
    o.pos = float4(ndc, 0.0, 1.0);
    o.uv  = float2(uv.x * uvscale.x, uv.y * uvscale.y);
    return o;
}

Texture2D    Tex : register(t0);
SamplerState Smp : register(s0); // point clamp

float4 PSMain(VSOut i) : SV_Target {
    return Tex.Sample(Smp, i.uv);
}
