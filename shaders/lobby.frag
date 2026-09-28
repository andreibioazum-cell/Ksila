#version 450

// Ksila lobby fragment shader: single-channel (alpha) font atlas sampled
// with a linear filter, multiplied by the interpolated vertex color.
// The atlas also holds one white texel used by untextured geometry.

layout(binding = 0) uniform sampler2D u_atlas;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

void main() {
    float mask = texture(u_atlas, v_uv).r;
    out_color = vec4(v_color.rgb, v_color.a * mask);
}
