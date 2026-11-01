#version 440

// A quad from the vertex index, no vertex buffer: strip order is
// (0,0) (1,0) (0,1) (1,1). rect holds the two corners in clip space
// (already through QRhi::clipSpaceCorrMatrix), so every backend agrees on
// which way is up. The texture is exactly the frame's size, so uv runs 0..1.
layout(location = 0) out vec2 vUv;

layout(std140, binding = 0) uniform Params {
    vec4 rect;   // x0, y0, x1, y1
};

void main()
{
    vec2 p = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    vUv = p;
    gl_Position = vec4(mix(rect.xy, rect.zw, p), 0.0, 1.0);
}
