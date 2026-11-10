// Renderer test: reads and writes outside the textures. On Direct3D such
// reads give zero and such writes are dropped, and effects rely on it (for
// example by writing 2x2 pixels per thread without checking the edge).

//!MAGPIE EFFECT
//!VERSION 4

//!TEXTURE
Texture2D INPUT;

//!TEXTURE
//!WIDTH 5
//!HEIGHT 3
Texture2D OUTPUT;

//!PASS 1
//!IN INPUT
//!OUT OUTPUT
//!BLOCK_SIZE 8
//!NUM_THREADS 8, 8

void Pass1(uint2 blockStart, uint3 threadId) {
	// No check against the output size: 49 of the 64 writes are outside
	const uint2 gxy = blockStart + threadId.xy;

	const float4 outside1 = INPUT.Load(int3(-1, 0, 0));
	const float4 outside2 = INPUT.Load(int3(0, 100000, 0));
	const float4 inside = INPUT.Load(int3(gxy, 0));

	// Green where both outside reads are zero, red otherwise
	const bool zero = !any(outside1.rgb) && !any(outside2.rgb);
	OUTPUT[gxy] = float4(zero ? 0 : 1, zero ? 1 : 0, inside.b, 1);
}
