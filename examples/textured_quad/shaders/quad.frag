#version 450
layout(set = 0, binding = 0) uniform Tint { vec4 color; } tint;
layout(set = 0, binding = 1) uniform sampler2D checker;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() { out_color = texture(checker, in_uv) * tint.color; }
