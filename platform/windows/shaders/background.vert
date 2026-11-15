#version 450
layout(location=0) out vec2 uv;
void main() {
    vec2 point = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = point;
    gl_Position = vec4(point * 2 - 1,0,1);
}
