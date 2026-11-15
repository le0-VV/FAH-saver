#version 450
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in mat4 model;
layout(location=6) in vec4 colour;
layout(push_constant) uniform Parameters { mat4 vp; vec4 background; vec4 options; } p;
layout(location=0) out vec3 vertexNormal;
layout(location=1) out vec3 vertexColour;
void main() {
    gl_Position = p.vp * model * vec4(position,1);
    gl_Position.y = -gl_Position.y;
    vertexNormal = transpose(inverse(mat3(model))) * normal;
    vertexColour = colour.rgb;
}
