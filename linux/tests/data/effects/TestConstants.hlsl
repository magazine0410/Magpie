// Renderer test: writes the built-in constants, the parameters and the
// constants of a pass in pixel shader style into the first row of the output,
// one item per pixel, so that the test can read them back.

//!MAGPIE EFFECT
//!VERSION 4

//!PARAMETER
//!LABEL Float parameter
//!DEFAULT 0.25
//!MIN 0
//!MAX 1
//!STEP 0.01
float fParam;

//!PARAMETER
//!LABEL Integer parameter
//!DEFAULT 3
//!MIN 0
//!MAX 200
//!STEP 1
int iParam;

//!TEXTURE
Texture2D INPUT;

//!TEXTURE
//!WIDTH INPUT_WIDTH * 2
//!HEIGHT INPUT_HEIGHT + 3
Texture2D OUTPUT;

//!TEXTURE
//!WIDTH INPUT_WIDTH + 1
//!HEIGHT 5
//!FORMAT R32G32B32A32_FLOAT
Texture2D MID;

//!SAMPLER
//!FILTER POINT
SamplerState sam;

//!PASS 1
//!STYLE PS
//!IN INPUT
//!OUT MID

float4 Pass1(float2 pos) {
	// pos is derived from the size of MID, which the renderer passes in the
	// constant buffer
	return float4(pos, fParam, iParam);
}

//!PASS 2
//!IN MID
//!OUT OUTPUT
//!BLOCK_SIZE 8
//!NUM_THREADS 8, 8

void Pass2(uint2 blockStart, uint3 threadId) {
	const uint2 gxy = blockStart + threadId.xy;
	const uint2 outputSize = GetOutputSize();
	if (gxy.x >= outputSize.x || gxy.y >= outputSize.y) {
		return;
	}

	float3 c = 0;
	if (gxy.y == 0) {
		// The texel (2, 3) of MID
		const float4 mid = MID.Load(int3(2, 3, 0));

		switch (gxy.x) {
		case 0:
			c = float3(GetInputSize(), 0);
			break;
		case 1:
			c = float3(GetOutputSize(), 0);
			break;
		case 2:
			c = float3(round(1 / GetInputPt()), 0);
			break;
		case 3:
			c = float3(round(1 / GetOutputPt()), 0);
			break;
		case 4:
			c = float3(round(GetScale() * 20), 0);
			break;
		case 5:
			c = float3(round(fParam * 100), iParam, 0);
			break;
		case 6:
			// Position of the texel centre, in texels
			c = float3(round(mid.xy * float2(GetInputSize().x + 1, 5) * 2), 0);
			break;
		case 7:
			c = float3(round(mid.z * 100), mid.w, 0);
			break;
		}
	}

	OUTPUT[gxy] = float4(c / 255.0f, 1);
}
