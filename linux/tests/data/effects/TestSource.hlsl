// Renderer test: copies two textures loaded from files to the output. The
// 8-bit one fills the left half, the 16-bit float one the right half.

//!MAGPIE EFFECT
//!VERSION 4

//!TEXTURE
Texture2D INPUT;

//!TEXTURE
//!WIDTH 8
//!HEIGHT 2
Texture2D OUTPUT;

//!TEXTURE
//!SOURCE TestSource8.dds
//!FORMAT R8G8B8A8_UNORM
Texture2D tex8;

//!TEXTURE
//!SOURCE TestSource16F.dds
//!FORMAT R16G16B16A16_FLOAT
Texture2D tex16F;

//!PASS 1
//!IN INPUT, tex8, tex16F
//!OUT OUTPUT
//!BLOCK_SIZE 8
//!NUM_THREADS 8, 8

void Pass1(uint2 blockStart, uint3 threadId) {
	const uint2 gxy = blockStart + threadId.xy;
	if (gxy.x >= 8 || gxy.y >= 2) {
		return;
	}

	if (gxy.x < 4) {
		OUTPUT[gxy] = float4(tex8.Load(int3(gxy, 0)).rgb, 1);
	} else {
		// The file holds values up to 4
		OUTPUT[gxy] = float4(tex16F.Load(int3(gxy.x - 4, gxy.y, 0)).rgb / 4, 1);
	}
}
