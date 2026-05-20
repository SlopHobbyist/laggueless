#version 450

// Fullscreen-quad vertex shader. Mirrors src/shaders/quad.hlsl.
// Push constants:
//   rect    = NDC rect: (x0, y0, x1, y1)
//   uvscale = portion of the source texture to sample: (scale_u, scale_v)
layout(push_constant) uniform PC {
    vec4 rect;
    vec4 uvscale;
} pc;

layout(location = 0) out vec2 out_uv;

// Triangle strip, 4 vertices. id bit 0 = x, bit 1 = y.
void main() {
    vec2 uv = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    vec2 ndc = mix(pc.rect.xy, pc.rect.zw, uv);
    gl_Position = vec4(ndc, 0.0, 1.0);
    out_uv = uv * pc.uvscale.xy;
}
