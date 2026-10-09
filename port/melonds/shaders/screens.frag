#version 450
// PS5CEMU-HAR: a DS screen scaled to the TV, as the screen filter asks (port/melonds/screens.cpp),
// through a bilinear sampler:
//  0 sharp: each of the DS's pixels a solid square, its edges blended over one pixel of the TV, so
//    the squares are even at any scale (11.25x, 5.625x...)
//  1 smooth: bilinear, the DS's pixels blurred into each other
//  2 square pixels: the nearest of the DS's pixels, its edges hard (uneven at a scale that is not
//    a whole number)
//  3 flat white: a bar of the touch cursor, which its pipeline's blend inverts what is under

layout(location = 0) in vec2 vUv;

layout(set = 0, binding = 0) uniform sampler2D uScreen;

layout(push_constant) uniform Constants
{
	vec4 rect;
	vec2 size;
	float mode;
} pc;

layout(location = 0) out vec4 outColour;

void main()
{
	if (pc.mode > 2.5)
	{
		outColour = vec4(1.0);
		return;
	}
	vec2 texel = vUv * pc.size;
	if (pc.mode < 0.5)
	{
		vec2 seam = floor(texel + 0.5);
		vec2 width = max(fwidth(texel), vec2(1e-5));
		texel = seam + clamp((texel - seam) / width, -0.5, 0.5);
	}
	else if (pc.mode > 1.5)
		texel = floor(texel) + 0.5;
	// the DS's pixels have no alpha worth reading
	outColour = vec4(texture(uScreen, texel / pc.size).rgb, 1.0);
}
