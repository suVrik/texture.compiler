$input v_position

#include <bgfx_shader.sh>

SAMPLER2D(s_texture, 0);

vec2 sample_spherical_map(vec3 v) {
    vec2 uv1 = vec2(atan2(-v.z, v.x), asin(v.y));
    uv1 *= vec2(0.1591, 0.3183);
    uv1 += 0.5;
    return uv1;
}

void main() {
    vec3 dir = normalize(v_position);
    #if (BGFX_SHADER_LANGUAGE_HLSL || BGFX_SHADER_LANGUAGE_PSSL || BGFX_SHADER_LANGUAGE_METAL)
    dir.y = -dir.y;
    #endif
    vec2 uv = sample_spherical_map(dir);
    gl_FragColor = vec4(texture2D(s_texture, uv).xyz, 1.0);
}
