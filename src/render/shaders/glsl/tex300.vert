#version 300 es

uniform mat3 proj;
uniform vec4 color;

in vec2 pos;
in vec2 texcoord;
in vec2 texcoordMatte;

out vec4 v_color;
out vec2 v_texcoord;
out vec2 v_texcoordMatte;
// keystone: the unit-quad position (0..1 across the window), warped to the trapezoid
// by proj. surface.frag uses it to compute an anti-aliased rounded-corner coverage in
// window-local space -- consistent across the window texture AND the frost pass (which
// carries a custom v_texcoord), so both get identical trapezoid edges.
out vec2 v_boxUV;

void main() {
    vec3 p_ks = proj * vec3(pos, 1.0);
    gl_Position = vec4(p_ks.xy, 0.0, p_ks.z);
    v_color = color;
    v_texcoord = texcoord;
    v_texcoordMatte = texcoordMatte;
    v_boxUV = pos;
}
