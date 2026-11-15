#version 450
layout(location=0) in vec3 vertexNormal;
layout(location=1) in vec3 vertexColour;
layout(location=0) out vec4 outputColour;
void main() {
    float light = .25 + .75 * max(dot(normalize(vertexNormal),normalize(vec3(.4,.8,1))),0);
    outputColour = vec4(vertexColour * light,1);
}
