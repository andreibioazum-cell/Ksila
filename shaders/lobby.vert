#version 450

// Ksila lobby vertex shader: fullscreen-pixel -> clip-space transform via
// push constants, passes UV (font atlas) and per-vertex color through.

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;

layout(push_constant) uniform Push {
    vec2 scale;
    vec2 translate;
} push;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
    v_uv = a_uv;
    v_color = a_color;
    gl_Position = vec4(a_position * push.scale + push.translate, 0.0, 1.0);
}
