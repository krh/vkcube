#version 420 core

in vec4 vVaryingColor;

void main()
{
    gl_FragColor = vVaryingColor;
}
