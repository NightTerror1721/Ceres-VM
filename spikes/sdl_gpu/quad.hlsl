// A textured quad over the whole target: the shape of the Ceres v2 GPU's final composition pass.
// Compiled twice by dxc, to DXIL (Direct3D 12) and to SPIR-V (Vulkan), and embedded in the spike.
//
// Resource slots follow SDL_GPU's layout (SDL_CreateGPUShader): a fragment shader's sampled textures live
// in register space 2 (descriptor set 2 in SPIR-V), and Vulkan wants the texture and its sampler as one
// combined image sampler at the same binding.

struct VSOut
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

// A four-vertex triangle strip, no vertex buffer: 0 top left, 1 top right, 2 bottom left, 3 bottom right.
VSOut vs_main(uint id : SV_VertexID)
{
	const float2 uv = float2(id & 1u, id >> 1u);
	VSOut output;
	output.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
	output.uv = uv;
	return output;
}

#if defined(__spirv__)
#define COMBINED [[vk::combinedImageSampler]]
#else
#define COMBINED
#endif

COMBINED Texture2D<float4> Screen : register(t0, space2);
COMBINED SamplerState ScreenSampler : register(s0, space2);

float4 ps_main(VSOut input) : SV_Target0
{
	return Screen.Sample(ScreenSampler, input.uv);
}
