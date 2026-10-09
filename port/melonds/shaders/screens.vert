#version 450
// PS5CEMU-HAR: a DS screen, or a bar of the touch cursor, as one quad on VideoOut's picture
// (port/melonds/screens.cpp). Its corners come from the vertex index (two triangles): no vertex buffer.

layout(push_constant) uniform Constants
{
	vec4 rect;  // the quad: left, top, right, bottom, in clip space
	vec2 size;  // the screen's pixels: 256 x 192
	float mode; // 0 sharp, 1 smooth, 2 square pixels; 3 flat white (the cursor)
} pc;

layout(location = 0) out vec2 vUv;

const vec2 kCorners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void main()
{
	vec2 corner = kCorners[gl_VertexIndex];
	vUv = corner;
	gl_Position = vec4(mix(pc.rect.xy, pc.rect.zw, corner), 0.0, 1.0);
}
