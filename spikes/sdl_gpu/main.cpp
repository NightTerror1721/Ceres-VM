// F0.3 spike: does SDL_GPU give the Ceres v2 GPU what its hardware mode needs, on this machine?
//
//   sdl_gpu_spike [--driver <name>] [--size <w>x<h>] [--frames <n>] [--offscreen] [--immediate]
//
// 1. Lists the GPU drivers SDL was built with and whether a device starts on each.
// 2. Creates a device, uploads a W x H test pattern (B8G8R8A8, the byte order of a Ceres 0x00RRGGBB pixel) with a
//    copy pass, draws it on a textured quad into an offscreen target of the same size, downloads that target with
//    SDL_DownloadFromGPUTexture and compares it with the pattern pixel by pixel: the path a readback test takes.
// 3. Times the offscreen loop (upload + draw + wait), then, unless --offscreen, opens a window, claims it and
//    times upload + draw + present per frame (vsync, or --immediate when the window supports it).
//
// Prints one "SPIKE ..." line per finding; exits non-zero if the readback does not match.
#include <SDL3/SDL.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "quad_ps_dxil.h"
#include "quad_vs_dxil.h"
#if SPIKE_HAS_SPIRV
#include "quad_ps_spirv.h"
#include "quad_vs_spirv.h"
#endif

namespace
{
	constexpr SDL_GPUShaderFormat AllFormats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
	// The formats this build carries shaders in (CMakeLists.txt leaves SPIR-V out when its dxc cannot make it).
#if SPIKE_HAS_SPIRV
	constexpr SDL_GPUShaderFormat BuiltFormats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL;
#else
	constexpr SDL_GPUShaderFormat BuiltFormats = SDL_GPU_SHADERFORMAT_DXIL;
#endif
	constexpr SDL_GPUTextureFormat PixelFormat = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;

	struct Options
	{
		const char* driver = nullptr;
		Uint32 width = 320;
		Uint32 height = 240;
		int frames = 600;
		bool offscreen = false;
		bool immediate = false;
	};

	[[noreturn]] void fail(const char* what)
	{
		std::printf("SPIKE error %s: %s\n", what, SDL_GetError());
		std::exit(2);
	}

	double seconds(Uint64 from, Uint64 to) { return static_cast<double>(to - from) / static_cast<double>(SDL_GetPerformanceFrequency()); }

	// 0xAARRGGBB, stored little-endian as B, G, R, A: what B8G8R8A8_UNORM reads.
	Uint32 pattern(Uint32 x, Uint32 y, Uint32 w, Uint32 h, Uint32 frame)
	{
		const Uint32 r = (x * 255u) / (w - 1);
		const Uint32 g = (y * 255u) / (h - 1);
		const Uint32 b = ((x ^ y) + frame) & 0xFFu;
		return 0xFF000000u | (r << 16) | (g << 8) | b;
	}

	void fill(std::vector<Uint32>& pixels, Uint32 w, Uint32 h, Uint32 frame)
	{
		for (Uint32 y = 0; y < h; ++y)
			for (Uint32 x = 0; x < w; ++x)
				pixels[y * w + x] = pattern(x, y, w, h, frame);
	}

	SDL_GPUShader* shader(SDL_GPUDevice* device, SDL_GPUShaderFormat format, SDL_GPUShaderStage stage)
	{
		const bool vertex = stage == SDL_GPU_SHADERSTAGE_VERTEX;
		SDL_GPUShaderCreateInfo info{};
		info.code = vertex ? quad_vs_dxil : quad_ps_dxil;
		info.code_size = vertex ? sizeof(quad_vs_dxil) : sizeof(quad_ps_dxil);
#if SPIKE_HAS_SPIRV
		if (format == SDL_GPU_SHADERFORMAT_SPIRV)
		{
			info.code = vertex ? quad_vs_spirv : quad_ps_spirv;
			info.code_size = vertex ? sizeof(quad_vs_spirv) : sizeof(quad_ps_spirv);
		}
#endif
		info.entrypoint = vertex ? "vs_main" : "ps_main";
		info.format = format;
		info.stage = stage;
		info.num_samplers = vertex ? 0 : 1;
		SDL_GPUShader* result = SDL_CreateGPUShader(device, &info);
		if (!result)
			fail(vertex ? "SDL_CreateGPUShader(vertex)" : "SDL_CreateGPUShader(fragment)");
		return result;
	}

	SDL_GPUGraphicsPipeline* pipeline(SDL_GPUDevice* device, SDL_GPUShader* vs, SDL_GPUShader* ps, SDL_GPUTextureFormat target)
	{
		SDL_GPUColorTargetDescription color{};
		color.format = target;
		SDL_GPUGraphicsPipelineCreateInfo info{};
		info.vertex_shader = vs;
		info.fragment_shader = ps;
		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
		info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		info.target_info.color_target_descriptions = &color;
		info.target_info.num_color_targets = 1;
		SDL_GPUGraphicsPipeline* result = SDL_CreateGPUGraphicsPipeline(device, &info);
		if (!result)
			fail("SDL_CreateGPUGraphicsPipeline");
		return result;
	}

	SDL_GPUTexture* texture(SDL_GPUDevice* device, Uint32 w, Uint32 h, SDL_GPUTextureUsageFlags usage)
	{
		SDL_GPUTextureCreateInfo info{};
		info.type = SDL_GPU_TEXTURETYPE_2D;
		info.format = PixelFormat;
		info.usage = usage;
		info.width = w;
		info.height = h;
		info.layer_count_or_depth = 1;
		info.num_levels = 1;
		SDL_GPUTexture* result = SDL_CreateGPUTexture(device, &info);
		if (!result)
			fail("SDL_CreateGPUTexture");
		return result;
	}

	SDL_GPUTransferBuffer* transferBuffer(SDL_GPUDevice* device, SDL_GPUTransferBufferUsage usage, Uint32 size)
	{
		SDL_GPUTransferBufferCreateInfo info{};
		info.usage = usage;
		info.size = size;
		SDL_GPUTransferBuffer* result = SDL_CreateGPUTransferBuffer(device, &info);
		if (!result)
			fail("SDL_CreateGPUTransferBuffer");
		return result;
	}

	// Copies the pixels into the upload buffer (cycled, so a frame in flight keeps its copy) and records the upload.
	void upload(SDL_GPUDevice* device, SDL_GPUCommandBuffer* cmd, SDL_GPUTransferBuffer* buffer, SDL_GPUTexture* target,
		const std::vector<Uint32>& pixels, Uint32 w, Uint32 h)
	{
		void* mapped = SDL_MapGPUTransferBuffer(device, buffer, true);
		if (!mapped)
			fail("SDL_MapGPUTransferBuffer");
		std::memcpy(mapped, pixels.data(), pixels.size() * sizeof(Uint32));
		SDL_UnmapGPUTransferBuffer(device, buffer);

		SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
		SDL_GPUTextureTransferInfo source{};
		source.transfer_buffer = buffer;
		source.pixels_per_row = w;
		source.rows_per_layer = h;
		SDL_GPUTextureRegion destination{};
		destination.texture = target;
		destination.w = w;
		destination.h = h;
		destination.d = 1;
		SDL_UploadToGPUTexture(copy, &source, &destination, true);
		SDL_EndGPUCopyPass(copy);
	}

	void draw(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, SDL_GPUGraphicsPipeline* pipe, SDL_GPUTexture* screen, SDL_GPUSampler* sampler)
	{
		SDL_GPUColorTargetInfo color{};
		color.texture = target;
		color.clear_color = SDL_FColor{ 1.0f, 0.0f, 1.0f, 1.0f };
		color.load_op = SDL_GPU_LOADOP_CLEAR;
		color.store_op = SDL_GPU_STOREOP_STORE;
		SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, nullptr);
		SDL_BindGPUGraphicsPipeline(pass, pipe);
		const SDL_GPUTextureSamplerBinding binding{ screen, sampler };
		SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
		SDL_DrawGPUPrimitives(pass, 4, 1, 0, 0);
		SDL_EndGPURenderPass(pass);
	}

	void submitAndWait(SDL_GPUDevice* device, SDL_GPUCommandBuffer* cmd)
	{
		SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
		if (!fence)
			fail("SDL_SubmitGPUCommandBufferAndAcquireFence");
		SDL_WaitForGPUFences(device, true, &fence, 1);
		SDL_ReleaseGPUFence(device, fence);
	}

	std::string formats(SDL_GPUShaderFormat f)
	{
		std::string s;
		const auto add = [&](SDL_GPUShaderFormat bit, const char* name) { if (f & bit) { if (!s.empty()) s += ","; s += name; } };
		add(SDL_GPU_SHADERFORMAT_PRIVATE, "private");
		add(SDL_GPU_SHADERFORMAT_SPIRV, "spirv");
		add(SDL_GPU_SHADERFORMAT_DXBC, "dxbc");
		add(SDL_GPU_SHADERFORMAT_DXIL, "dxil");
		add(SDL_GPU_SHADERFORMAT_MSL, "msl");
		add(SDL_GPU_SHADERFORMAT_METALLIB, "metallib");
		return s;
	}

	Options parse(int argc, char** argv)
	{
		Options o;
		for (int i = 1; i < argc; ++i)
		{
			const std::string a = argv[i];
			if (a == "--driver" && i + 1 < argc) o.driver = argv[++i];
			else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%ux%u", &o.width, &o.height);
			else if (a == "--frames" && i + 1 < argc) o.frames = std::atoi(argv[++i]);
			else if (a == "--offscreen") o.offscreen = true;
			else if (a == "--immediate") o.immediate = true;
			else { std::printf("usage: sdl_gpu_spike [--driver <name>] [--size <w>x<h>] [--frames <n>] [--offscreen] [--immediate]\n"); std::exit(1); }
		}
		return o;
	}
}

int main(int argc, char** argv)
{
	const Options options = parse(argc, argv);
	const Uint32 w = options.width;
	const Uint32 h = options.height;
	if (!SDL_Init(SDL_INIT_VIDEO))
		fail("SDL_Init");

	// 1. Which drivers start here.
	for (int i = 0; i < SDL_GetNumGPUDrivers(); ++i)
	{
		const char* name = SDL_GetGPUDriver(i);
		SDL_GPUDevice* probe = SDL_CreateGPUDevice(AllFormats, false, name);
		std::printf("SPIKE driver %-10s %s", name, probe ? "starts" : "fails");
		if (probe)
		{
			std::printf(" (shaders: %s)\n", formats(SDL_GetGPUShaderFormats(probe)).c_str());
			SDL_DestroyGPUDevice(probe);
		}
		else
			std::printf(": %s\n", SDL_GetError());
	}

	// 2. One device, the readback round trip.
	SDL_GPUDevice* device = SDL_CreateGPUDevice(BuiltFormats, false, options.driver);
	if (!device)
		fail("SDL_CreateGPUDevice");
	const SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(device);
	const SDL_GPUShaderFormat format = (supported & SDL_GPU_SHADERFORMAT_DXIL) ? SDL_GPU_SHADERFORMAT_DXIL : SDL_GPU_SHADERFORMAT_SPIRV;
	std::printf("SPIKE device %s, shaders %s, size %ux%u\n", SDL_GetGPUDeviceDriver(device), formats(format).c_str(), w, h);

	SDL_GPUShader* vs = shader(device, format, SDL_GPU_SHADERSTAGE_VERTEX);
	SDL_GPUShader* ps = shader(device, format, SDL_GPU_SHADERSTAGE_FRAGMENT);
	SDL_GPUGraphicsPipeline* offscreenPipe = pipeline(device, vs, ps, PixelFormat);

	SDL_GPUSamplerCreateInfo samplerInfo{};
	samplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
	samplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
	samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
	samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	SDL_GPUSampler* sampler = SDL_CreateGPUSampler(device, &samplerInfo);
	if (!sampler)
		fail("SDL_CreateGPUSampler");

	SDL_GPUTexture* screen = texture(device, w, h, SDL_GPU_TEXTUREUSAGE_SAMPLER);
	SDL_GPUTexture* target = texture(device, w, h, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
	const Uint32 bytes = w * h * 4;
	SDL_GPUTransferBuffer* up = transferBuffer(device, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes);
	SDL_GPUTransferBuffer* down = transferBuffer(device, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, bytes);

	std::vector<Uint32> pixels(static_cast<size_t>(w) * h);
	fill(pixels, w, h, 0);
	{
		SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
		upload(device, cmd, up, screen, pixels, w, h);
		draw(cmd, target, offscreenPipe, screen, sampler);
		SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
		SDL_GPUTextureRegion source{};
		source.texture = target;
		source.w = w;
		source.h = h;
		source.d = 1;
		SDL_GPUTextureTransferInfo destination{};
		destination.transfer_buffer = down;
		destination.pixels_per_row = w;
		destination.rows_per_layer = h;
		SDL_DownloadFromGPUTexture(copy, &source, &destination);
		SDL_EndGPUCopyPass(copy);
		submitAndWait(device, cmd);
	}
	const auto* read = static_cast<const Uint32*>(SDL_MapGPUTransferBuffer(device, down, false));
	if (!read)
		fail("SDL_MapGPUTransferBuffer(download)");
	size_t mismatches = 0;
	size_t first = 0;
	for (size_t i = 0; i < pixels.size(); ++i)
		if (read[i] != pixels[i] && mismatches++ == 0)
			first = i;
	std::printf("SPIKE readback %s: %zu of %zu pixels differ", mismatches == 0 ? "exact" : "WRONG", mismatches, pixels.size());
	if (mismatches != 0)
		std::printf(" (first at %zu,%zu: got %08X, want %08X)", first % w, first / w, read[first], pixels[first]);
	std::printf("\n");
	SDL_UnmapGPUTransferBuffer(device, down);

	// 3a. Offscreen: upload + draw + wait for the GPU, every frame.
	{
		const Uint64 start = SDL_GetPerformanceCounter();
		for (int frame = 0; frame < options.frames; ++frame)
		{
			SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
			upload(device, cmd, up, screen, pixels, w, h);
			draw(cmd, target, offscreenPipe, screen, sampler);
			submitAndWait(device, cmd);
		}
		const double s = seconds(start, SDL_GetPerformanceCounter());
		std::printf("SPIKE offscreen %d frames: %.1f us per upload+draw+wait\n", options.frames, s * 1e6 / options.frames);
	}

	// 3b. A window: upload + draw to the swapchain + present.
	if (!options.offscreen)
	{
		const int scale = SDL_max(1, SDL_min(1280 / static_cast<int>(w), 720 / static_cast<int>(h)));
		SDL_Window* window = SDL_CreateWindow("SDL_GPU spike", static_cast<int>(w) * scale, static_cast<int>(h) * scale, 0);
		if (!window)
			fail("SDL_CreateWindow");
		if (!SDL_ClaimWindowForGPUDevice(device, window))
			fail("SDL_ClaimWindowForGPUDevice");
		const bool immediate = options.immediate && SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_IMMEDIATE);
		if (immediate)
			SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_IMMEDIATE);
		const SDL_GPUTextureFormat swapFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
		SDL_GPUGraphicsPipeline* windowPipe = pipeline(device, vs, ps, swapFormat);

		int presented = 0;
		double cpu = 0.0;
		double filling = 0.0;
		const Uint64 start = SDL_GetPerformanceCounter();
		for (int frame = 0; frame < options.frames; ++frame)
		{
			SDL_Event event;
			bool quit = false;
			while (SDL_PollEvent(&event))
				quit |= event.type == SDL_EVENT_QUIT;
			if (quit)
				break;

			const Uint64 tf = SDL_GetPerformanceCounter();
			fill(pixels, w, h, static_cast<Uint32>(frame));   // stands in for the machine drawing into VRAM
			const Uint64 t0 = SDL_GetPerformanceCounter();
			filling += seconds(tf, t0);
			SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
			upload(device, cmd, up, screen, pixels, w, h);
			const Uint64 t1 = SDL_GetPerformanceCounter();
			SDL_GPUTexture* swap = nullptr;
			if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, nullptr, nullptr))
				fail("SDL_WaitAndAcquireGPUSwapchainTexture");
			const Uint64 t2 = SDL_GetPerformanceCounter();
			if (swap)
			{
				draw(cmd, swap, windowPipe, screen, sampler);
				++presented;
			}
			SDL_SubmitGPUCommandBuffer(cmd);
			cpu += seconds(t0, t1) + seconds(t2, SDL_GetPerformanceCounter());   // leaves out the swapchain wait
		}
		const double s = seconds(start, SDL_GetPerformanceCounter());
		std::printf("SPIKE window %s, %d frames in %.2f s: %.1f fps, %.1f us of CPU per frame in SDL_GPU (upload+draw+submit, "
			"without the swapchain wait), %.1f us filling the pattern\n", immediate ? "immediate" : "vsync", presented, s, presented / s,
			cpu * 1e6 / SDL_max(presented, 1), filling * 1e6 / SDL_max(presented, 1));
		SDL_WaitForGPUIdle(device);
		SDL_ReleaseGPUGraphicsPipeline(device, windowPipe);
		SDL_ReleaseWindowFromGPUDevice(device, window);
		SDL_DestroyWindow(window);
	}

	SDL_ReleaseGPUTransferBuffer(device, up);
	SDL_ReleaseGPUTransferBuffer(device, down);
	SDL_ReleaseGPUTexture(device, screen);
	SDL_ReleaseGPUTexture(device, target);
	SDL_ReleaseGPUSampler(device, sampler);
	SDL_ReleaseGPUGraphicsPipeline(device, offscreenPipe);
	SDL_ReleaseGPUShader(device, vs);
	SDL_ReleaseGPUShader(device, ps);
	SDL_DestroyGPUDevice(device);
	SDL_Quit();
	return mismatches == 0 ? 0 : 3;
}
