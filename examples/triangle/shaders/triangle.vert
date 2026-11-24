#version 450

// The three vertices are constants: version 1 of the example needs no vertex
// buffer. gl_VertexIndex selects the position and the color.
layout(location = 0) out vec3 color;

void main() {
  const vec2 positions[3] = vec2[](vec2(0.0, -0.6), vec2(0.6, 0.6), vec2(-0.6, 0.6));
  const vec3 colors[3] = vec3[](vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 0.0, 1.0));
  gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
  color = colors[gl_VertexIndex];
}
