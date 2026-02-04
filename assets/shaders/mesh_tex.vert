#version 450

layout(set = 0, binding = 0) uniform CameraUBO
{
  mat4 viewProj;
} ubo;

layout(push_constant) uniform Push
{
  mat4 model;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 vColor;
layout(location = 1) out vec2 vUV;

void main()
{
  gl_Position = ubo.viewProj * pc.model * vec4(inPos, 1.0);
  vColor = inColor;
  vUV = inUV;
}
