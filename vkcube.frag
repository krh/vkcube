#version 420 core

layout(location = 0) in vec4 vVaryingColor;

void main()
{
    gl_FragColor = vVaryingColor;
}
