// Renderer test: copies the input through group-shared memory, in the way
// the bundled effects that use it do (FSRCNNX, NNEDI3, RAVU, NIS, Ani4Kv2).
// The output must equal the input.

//!MAGPIE EFFECT
//!VERSION 4

//!TEXTURE
Texture2D INPUT;

//!TEXTURE
//!WIDTH INPUT_WIDTH
//!HEIGHT INPUT_HEIGHT
Texture2D OUTPUT;

//!SAMPLER
//!FILTER POINT
SamplerState sam;

//!PASS 1
//!IN INPUT
//!OUT OUTPUT
//!BLOCK_SIZE 8
//!NUM_THREADS 8, 8

#define K 8
static const uint2 isize = uint2(MP_BLOCK_WIDTH + 2, MP_BLOCK_HEIGHT + 2);
groupshared float4 inp[K][MP_BLOCK_HEIGHT + 2][MP_BLOCK_WIDTH + 2];

void Pass1(uint2 blockStart, uint3 tid) {
	const float2 inputPt = GetInputPt();
	const float2 base = (blockStart - float2(1, 1) + 0.5f) * inputPt;

	for (uint y = tid.y; y < isize.y; y += MP_NUM_THREADS_Y) {
		for (uint x = tid.x; x < isize.x; x += MP_NUM_THREADS_X) {
			const float4 c = INPUT.SampleLevel(sam, mad(float2(x, y), inputPt, base), 0);
			for (uint k = 0; k < K; ++k) {
				inp[k][y][x] = c * (k + 1);
			}
		}
	}

	GroupMemoryBarrierWithGroupSync();

	float4 sum = 0;
	float n = 0;
	for (uint k = 0; k < K; ++k) {
		sum += inp[k][tid.y + 1][tid.x + 1];
		n += k + 1;
	}
	OUTPUT[blockStart + tid.xy] = float4(sum.rgb / n, 1);
}
