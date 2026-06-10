#version 450

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2DArray srcColor;

void main()
{
    outColor = texture(srcColor, vec3(texcoord, 0.0));
}

