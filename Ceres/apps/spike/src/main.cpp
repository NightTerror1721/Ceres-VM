// Fase 0 spike: prove that SDL3 builds and links, that a cooperative step + poll + present loop
// can drive the VM, and that real keyboard/mouse events map onto KeyboardDevice/MouseDevice.
//
// This deliberately touches NO library: it uses only the public APIs of ceres-vm, ceres-devices
// and ceres-asm, plus SDL3. Fase 1 widens the key code to 32 bits and adds a pixel framebuffer;
// Fase 2 folds this loop into a proper HostBackend behind `ceres run --window`.
//
// Optional argv[1] is a frame cap for headless smoke tests: `ceres_sdl_spike 50` runs 50 frames
// and quits, so it can be exercised under SDL_VIDEODRIVER=dummy without a display or a human.

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

#include <ceres/vm/ceresvm.h>
#include <ceres/devices/devices.h>
#include <ceres/devices/input_devices.h>
#include <ceres/asm/assembler.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

using namespace ceres;
using namespace ceres::vm;
using namespace ceres::devices;
using namespace ceres::casm;

namespace
{
	// SDL's button bits do not line up with MouseDevice's (SDL: right = bit 2, middle = bit 1), so
	// the mapping is explicit rather than a cast - exactly the kind of thing this spike pins down.
	u8 toMouseButtons(Uint32 sdlState)
	{
		u8 buttons = 0;
		if (sdlState & SDL_BUTTON_LMASK) buttons |= MouseDevice::ButtonLeft;
		if (sdlState & SDL_BUTTON_RMASK) buttons |= MouseDevice::ButtonRight;
		if (sdlState & SDL_BUTTON_MMASK) buttons |= MouseDevice::ButtonMiddle;
		return buttons;
	}

	// Pumps pending SDL events into the two input devices. Returns false when the window closes.
	bool pumpEvents(KeyboardDevice& keyboard, MouseDevice& mouse)
	{
		SDL_Event event;
		while (SDL_PollEvent(&event))
		{
			switch (event.type)
			{
				case SDL_EVENT_QUIT:
					return false;

				case SDL_EVENT_KEY_DOWN:
					// Truncated to u8: SDL scancodes go past 255. Exactly what Fase 1 fixes by
					// widening KeyboardDevice to 32-bit key codes.
					keyboard.pushKey(static_cast<u8>(event.key.scancode), true);
					if (event.key.scancode == SDL_SCANCODE_ESCAPE)
						return false;
					break;

				case SDL_EVENT_KEY_UP:
					keyboard.pushKey(static_cast<u8>(event.key.scancode), false);
					break;

				case SDL_EVENT_MOUSE_MOTION:
					mouse.pushMotion(static_cast<i32>(event.motion.xrel), static_cast<i32>(event.motion.yrel),
						toMouseButtons(event.motion.state), 0);
					break;

				case SDL_EVENT_MOUSE_BUTTON_DOWN:
				case SDL_EVENT_MOUSE_BUTTON_UP:
					// A button event names which button changed, not the whole mask, so ask SDL for it.
					mouse.pushMotion(0, 0, toMouseButtons(SDL_GetMouseState(nullptr, nullptr)), 0);
					break;

				case SDL_EVENT_MOUSE_WHEEL:
					mouse.pushMotion(0, 0, toMouseButtons(SDL_GetMouseState(nullptr, nullptr)), static_cast<i8>(event.wheel.y));
					break;

				default:
					break;
			}
		}
		return true;
	}
}

int main(int argc, char* argv[])
{
	int frameLimit = argc > 1 ? std::atoi(argv[1]) : 0;

	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	SDL_Window* window = SDL_CreateWindow("Ceres SDL spike", 640, 480, SDL_WINDOW_RESIZABLE);
	if (!window)
	{
		std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		SDL_Quit();
		return 1;
	}

	SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
	if (!renderer)
	{
		std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 1;
	}

	// A game wants unbounded deltas, not a cursor that stops at the window edge.
	SDL_SetWindowRelativeMouseMode(window, true);

	Assembler assembler;
	auto program = assembler.assemble({ std::filesystem::path(SPIKE_ECHO_SOURCE) });
	if (!program)
	{
		std::fprintf(stderr, "Failed to assemble %s\n", SPIKE_ECHO_SOURCE);
		SDL_DestroyRenderer(renderer);
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 1;
	}

	CeresVM vm;
	TerminalDevice terminal;
	KeyboardDevice keyboard;
	MouseDevice mouse;
	terminal.attachTo(vm.io());
	keyboard.attachTo(vm.io());
	mouse.attachTo(vm.io());
	terminal.setOutputSink([](u8 byte) { std::fputc(static_cast<char>(byte), stdout); });

	if (auto loaded = vm.loadProgram(*program); !loaded)
	{
		std::fprintf(stderr, "loadProgram failed: %s\n", loaded.error().c_str());
		SDL_DestroyRenderer(renderer);
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 1;
	}

	// The cooperative loop: run a slice of instructions, pump SDL events, then present. SDL wants
	// its events polled on the thread that owns the window, so the VM is stepped here a slice at a
	// time instead of running CeresVM::run() to completion.
	constexpr int instructionsPerFrame = 4096;

	// Smoke mode (a frame limit was given): inject one synthetic key and one mouse motion so the
	// SDL -> device mapping is exercised headlessly - SDL_PollEvent under the dummy driver yields
	// nothing on its own. The program echoes 'A' for the key and 0x03 for the delta-X.
	if (frameLimit > 0)
	{
		SDL_Event key{};
		key.type = SDL_EVENT_KEY_DOWN;
		key.key.scancode = static_cast<SDL_Scancode>('A');
		SDL_PushEvent(&key);

		SDL_Event motion{};
		motion.type = SDL_EVENT_MOUSE_MOTION;
		motion.motion.xrel = 3.0f;
		motion.motion.yrel = 0.0f;
		motion.motion.state = 0;
		SDL_PushEvent(&motion);
	}

	bool running = true;
	int frame = 0;
	while (running)
	{
		if (!pumpEvents(keyboard, mouse))
			break;

		for (int i = 0; i < instructionsPerFrame; ++i)
			vm.engine().step();

		SDL_SetRenderDrawColor(renderer, 16, 16, 16, 255);
		SDL_RenderClear(renderer);
		SDL_RenderPresent(renderer);

		std::fflush(stdout);

		if (frameLimit > 0 && ++frame >= frameLimit)
			break;
	}

	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
}
