#version 450

layout(set = 1, binding = 0) uniform sampler2D albedoTex;

layout(location = 0) in vec3 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main()
{
  vec4 texel = texture(albedoTex, vUV);
  outColor = vec4(vColor, 1.0) * texel;
}
