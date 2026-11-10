// Renderer test: writes a known value into a texture of every format an
// effect may use and reads it back in a later pass. The values need the
// precision, the sign and the range that the name of the format promises.
// Pixel i of the output is green if format i returned its value, red
// otherwise. The file was written by a script; edit it by hand from here on.

//!MAGPIE EFFECT
//!VERSION 4

//!TEXTURE
Texture2D INPUT;

//!TEXTURE
//!WIDTH 20
//!HEIGHT 1
Texture2D OUTPUT;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R32G32B32A32_FLOAT
Texture2D F0;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R16G16B16A16_FLOAT
Texture2D F1;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R16G16B16A16_UNORM
Texture2D F2;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R16G16B16A16_SNORM
Texture2D F3;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R32G32_FLOAT
Texture2D F4;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R10G10B10A2_UNORM
Texture2D F5;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R11G11B10_FLOAT
Texture2D F6;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R8G8B8A8_UNORM
Texture2D F7;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R8G8B8A8_SNORM
Texture2D F8;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R16G16_FLOAT
Texture2D F9;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R16G16_UNORM
Texture2D F10;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R16G16_SNORM
Texture2D F11;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R32_FLOAT
Texture2D F12;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R8G8_UNORM
Texture2D F13;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R8G8_SNORM
Texture2D F14;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R16_FLOAT
Texture2D F15;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R16_UNORM
Texture2D F16;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R16_SNORM
Texture2D F17;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R8_UNORM
Texture2D F18;

//!TEXTURE
//!WIDTH 2
//!HEIGHT 2
//!FORMAT R8_SNORM
Texture2D F19;

//!PASS 1
//!IN INPUT
//!OUT F0, F1, F2, F3, F4, F5, F6, F7
//!BLOCK_SIZE 8
//!NUM_THREADS 8, 8

void Pass1(uint2 blockStart, uint3 threadId) {
	const uint2 gxy = blockStart + threadId.xy;
	if (gxy.x >= 2 || gxy.y >= 2) {
		return;
	}

	F0[gxy] = float4(-2.5, 1024.0, 0.5, 3.0);
	F1[gxy] = float4(-2.5, 1024.0, 0.5, 3.0);
	F2[gxy] = float4(1.0 / 65535, 32768.0 / 65535, 65534.0 / 65535, 16384.0 / 65535);
	F3[gxy] = float4(-1.0 / 32767, -16384.0 / 32767, 12345.0 / 32767, 1.0);
	F4[gxy] = float2(-2.5, 1024.0);
	F5[gxy] = float4(1.0 / 1023, 512.0 / 1023, 1022.0 / 1023, 2.0 / 3);
	F6[gxy] = float3(0.5, 2.0, 8.0);
	F7[gxy] = float4(51.0 / 255, 102.0 / 255, 153.0 / 255, 204.0 / 255);
}

//!PASS 2
//!IN INPUT
//!OUT F8, F9, F10, F11, F12, F13, F14, F15
//!BLOCK_SIZE 8
//!NUM_THREADS 8, 8

void Pass2(uint2 blockStart, uint3 threadId) {
	const uint2 gxy = blockStart + threadId.xy;
	if (gxy.x >= 2 || gxy.y >= 2) {
		return;
	}

	F8[gxy] = float4(-1.0, -1.0 / 127, 64.0 / 127, 1.0);
	F9[gxy] = float2(-2.5, 1024.0);
	F10[gxy] = float2(1.0 / 65535, 32768.0 / 65535);
	F11[gxy] = float2(-1.0 / 32767, -16384.0 / 32767);
	F12[gxy] = -2.5;
	F13[gxy] = float2(51.0 / 255, 102.0 / 255);
	F14[gxy] = float2(-1.0, -1.0 / 127);
	F15[gxy] = -2.5;
}

//!PASS 3
//!IN INPUT
//!OUT F16, F17, F18, F19
//!BLOCK_SIZE 8
//!NUM_THREADS 8, 8

void Pass3(uint2 blockStart, uint3 threadId) {
	const uint2 gxy = blockStart + threadId.xy;
	if (gxy.x >= 2 || gxy.y >= 2) {
		return;
	}

	F16[gxy] = 1.0 / 65535;
	F17[gxy] = -1.0 / 32767;
	F18[gxy] = 51.0 / 255;
	F19[gxy] = -1.0;
}

//!PASS 4
//!IN F0, F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12, F13, F14, F15, F16, F17, F18, F19
//!OUT OUTPUT
//!BLOCK_SIZE 8
//!NUM_THREADS 8, 8

void Pass4(uint2 blockStart, uint3 threadId) {
	const uint2 gxy = blockStart + threadId.xy;
	if (gxy.x >= 20 || gxy.y >= 1) {
		return;
	}

	bool ok = false;
	switch (gxy.x) {
	case 0:	// R32G32B32A32_FLOAT
		ok = all(abs(F0.Load(int3(1, 1, 0)) - float4(-2.5, 1024.0, 0.5, 3.0)) <= 1e-6);
		break;
	case 1:	// R16G16B16A16_FLOAT
		ok = all(abs(F1.Load(int3(1, 1, 0)) - float4(-2.5, 1024.0, 0.5, 3.0)) <= 1e-6);
		break;
	case 2:	// R16G16B16A16_UNORM
		ok = all(abs(F2.Load(int3(1, 1, 0)) - float4(1.0 / 65535, 32768.0 / 65535, 65534.0 / 65535, 16384.0 / 65535)) <= 0.25 / 65535);
		break;
	case 3:	// R16G16B16A16_SNORM
		ok = all(abs(F3.Load(int3(1, 1, 0)) - float4(-1.0 / 32767, -16384.0 / 32767, 12345.0 / 32767, 1.0)) <= 0.25 / 32767);
		break;
	case 4:	// R32G32_FLOAT
		ok = all(abs(F4.Load(int3(1, 1, 0)) - float2(-2.5, 1024.0)) <= 1e-6);
		break;
	case 5:	// R10G10B10A2_UNORM
		ok = all(abs(F5.Load(int3(1, 1, 0)) - float4(1.0 / 1023, 512.0 / 1023, 1022.0 / 1023, 2.0 / 3)) <= 0.25 / 1023);
		break;
	case 6:	// R11G11B10_FLOAT
		ok = all(abs(F6.Load(int3(1, 1, 0)) - float3(0.5, 2.0, 8.0)) <= 1e-6);
		break;
	case 7:	// R8G8B8A8_UNORM
		ok = all(abs(F7.Load(int3(1, 1, 0)) - float4(51.0 / 255, 102.0 / 255, 153.0 / 255, 204.0 / 255)) <= 0.25 / 255);
		break;
	case 8:	// R8G8B8A8_SNORM
		ok = all(abs(F8.Load(int3(1, 1, 0)) - float4(-1.0, -1.0 / 127, 64.0 / 127, 1.0)) <= 0.25 / 127);
		break;
	case 9:	// R16G16_FLOAT
		ok = all(abs(F9.Load(int3(1, 1, 0)) - float2(-2.5, 1024.0)) <= 1e-6);
		break;
	case 10:	// R16G16_UNORM
		ok = all(abs(F10.Load(int3(1, 1, 0)) - float2(1.0 / 65535, 32768.0 / 65535)) <= 0.25 / 65535);
		break;
	case 11:	// R16G16_SNORM
		ok = all(abs(F11.Load(int3(1, 1, 0)) - float2(-1.0 / 32767, -16384.0 / 32767)) <= 0.25 / 32767);
		break;
	case 12:	// R32_FLOAT
		ok = abs(F12.Load(int3(1, 1, 0)) - -2.5) <= 1e-6;
		break;
	case 13:	// R8G8_UNORM
		ok = all(abs(F13.Load(int3(1, 1, 0)) - float2(51.0 / 255, 102.0 / 255)) <= 0.25 / 255);
		break;
	case 14:	// R8G8_SNORM
		ok = all(abs(F14.Load(int3(1, 1, 0)) - float2(-1.0, -1.0 / 127)) <= 0.25 / 127);
		break;
	case 15:	// R16_FLOAT
		ok = abs(F15.Load(int3(1, 1, 0)) - -2.5) <= 1e-6;
		break;
	case 16:	// R16_UNORM
		ok = abs(F16.Load(int3(1, 1, 0)) - 1.0 / 65535) <= 0.25 / 65535;
		break;
	case 17:	// R16_SNORM
		ok = abs(F17.Load(int3(1, 1, 0)) - -1.0 / 32767) <= 0.25 / 32767;
		break;
	case 18:	// R8_UNORM
		ok = abs(F18.Load(int3(1, 1, 0)) - 51.0 / 255) <= 0.25 / 255;
		break;
	case 19:	// R8_SNORM
		ok = abs(F19.Load(int3(1, 1, 0)) - -1.0) <= 0.25 / 127;
		break;
	}

	OUTPUT[gxy] = float4(ok ? 0 : 1, ok ? 1 : 0, 0, 1);
}
