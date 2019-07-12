$input v_position

#include <bgfx_shader.sh>

#define PI 3.14159265359

SAMPLERCUBE(s_texture, 0);

uniform vec4 u_settings;

#define u_roughness u_settings.x
#define u_side_resolution u_settings.y

float distribution(vec3 normal_dir, vec3 half_dir, float roughness) {
    float a = roughness * roughness;
    float a_sqr = a * a;
    float dot_normal_half = max(dot(normal_dir, half_dir), 0.0);
    float dot_normal_half_sqr = dot_normal_half * dot_normal_half;

    float denom = (dot_normal_half_sqr * (a_sqr - 1.0) + 1.0);
    denom = PI * denom * denom;

    return a_sqr / denom;
}

float inverse_vdc(int n) {
    float inv_base = 0.5;
    float denom = 1.0;
    float result = 0.0;

    for (int i = 0; i < 32; i++) {
        if (n > 0) {
            denom = mod(float(n), 2.0);
            result += denom * inv_base;
            inv_base = inv_base / 2.0;
            n = int(float(n) / 2.0);
        }
    }
    return result;
}

vec2 hammersley(int i, int n) {
    return vec2(float(i) / float(n), inverse_vdc(i));
}

vec3 importance_sample(vec2 xi, vec3 normal_dir, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    float cos_theta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sin_theta = sqrt(1.0 - cos_theta * cos_theta);

    // from spherical coordinates to cartesian coordinates - halfway vector
    vec3 half_dir;
    half_dir.x = cos(phi) * sin_theta;
    half_dir.y = sin(phi) * sin_theta;
    half_dir.z = cos_theta;

    // from tangent-space halfway vector to world-space sample vector
    vec3 up = abs(normal_dir.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, normal_dir));
    vec3 bitangent = cross(normal_dir, tangent);
    return normalize(tangent * half_dir.x + bitangent * half_dir.y + normal_dir * half_dir.z);
}

void main() {
    vec3 normal = normalize(v_position);
    vec3 reflection = normal;
    vec3 camera = reflection;

    const int SAMPLE_COUNT = 1024;
    vec3 prefiltered_color = vec3(0.0, 0.0, 0.0);
    float total_weight = 0.0;

    for (int i = 0; i < SAMPLE_COUNT; i++) {
        // generates a sample vector that's biased towards the preferred alignment direction (importance sampling).
        vec2 xi = hammersley(i, SAMPLE_COUNT);
        vec3 half_dir = importance_sample(xi, normal, u_roughness);
        vec3 light_dir = normalize(2.0 * dot(camera, half_dir) * half_dir - camera);

        float dot_normal_light = max(dot(normal, light_dir), 0.0);
        if (dot_normal_light > 0.0) {
            // sample from the environment's mip level based on roughness/pdf
            float dist = distribution(normal, half_dir, u_roughness);
            float dot_normal_half = max(dot(normal, half_dir), 0.0);
            float dot_half_camera = max(dot(half_dir, camera), 0.0);
            float pdf = dist * dot_normal_half / (4.0 * dot_half_camera) + 0.0001;

            float sa_texel  = 4.0 * PI / (6.0 * u_side_resolution * u_side_resolution);
            float sa_sample = 1.0 / (float(SAMPLE_COUNT) * pdf + 0.0001);

            float mip_level = u_roughness == 0.0 ? 0.0 : 0.5 * log2(sa_sample / sa_texel);
            #if (BGFX_SHADER_LANGUAGE_HLSL || BGFX_SHADER_LANGUAGE_PSSL || BGFX_SHADER_LANGUAGE_METAL)
            light_dir.y = -light_dir.y;
            #endif
            prefiltered_color += textureCubeLod(s_texture, light_dir, mip_level).xyz * dot_normal_light;
            total_weight += dot_normal_light;
        }
    }
    gl_FragColor = vec4(prefiltered_color / total_weight, 1.0);
}
