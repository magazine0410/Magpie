// Renderer test: every pixel shows the frame counter.

//!MAGPIE EFFECT
//!VERSION 4
//!USE _DYNAMIC

//!TEXTURE
Texture2D INPUT;

//!TEXTURE
//!WIDTH INPUT_WIDTH
//!HEIGHT INPUT_HEIGHT
Texture2D OUTPUT;

//!PASS 1
//!STYLE PS
//!IN INPUT
//!OUT OUTPUT

float4 Pass1(float2 pos) {
	return float4(__frameCount / 255.0f, 0, 0, 1);
}
