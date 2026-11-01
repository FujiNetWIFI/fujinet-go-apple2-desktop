#version 440

// Alpha is forced to 1: the session's pixels are 0x00RRGGBB, and a window
// composited with alpha 0 shows what is behind it (KWin does).
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;
layout(binding = 1) uniform sampler2D tex;

void main()
{
    fragColor = vec4(texture(tex, vUv).rgb, 1.0);
}
