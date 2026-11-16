#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outputColour;
layout(push_constant) uniform Parameters { mat4 vp; vec4 background; vec4 options; } p;
void main() {
    vec3 colour = p.background.rgb;
    if (p.options.x > .5) {
        bool dark = p.options.x > 1.5;
        vec3 top = dark ? vec3(11.,18.,29.) / 255. : vec3(.09,.23,.40);
        vec3 bottom = dark ? vec3(20.,30.,44.) / 255. : vec3(.55,.70,.80);
        vec3 gridColour = dark ? vec3(80.,93.,108.) / 255. : vec3(.25,.38,.45);
        colour = mix(top,bottom,uv.y);
        if (uv.y > .55) {
            float depth = (uv.y - .55) / .45;
            vec2 point = vec2((uv.x - .5) * 6.,3.) / max(depth,.02);
            vec2 grid = abs(fract(point - .5) - .5) / max(fwidth(point),vec2(.0001));
            float line = 1. - min(min(grid.x,grid.y),1.);
            colour = mix(colour,gridColour,line * depth * .35);
        }
    }
    outputColour = vec4(colour,1);
}
