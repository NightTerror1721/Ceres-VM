#pragma once

// The GPU (plan/v2 SPEC 7): one device in the video group's slots, with the display's timing, the planes the scanout
// composes and the engines that draw. Its internal parts - the display controller, the planes, the engines, the
// executor - are not devices of their own (SPEC 5.2); only this is on the bus.
//
// F5 builds levels V0 (the text plane) and V1 (the bitmap plane and the copy engine). The registers of the core and
// the display are at 0x000-0x120 of slot 0x40 (SPEC 7.3). The slots 0x41-0x43 are the command processor's and the
// 2D and 3D engines' and are not attached until the phases that fill them (F10, F13, F14).

#include <ceres/devices/video/display_controller.h>
#include <ceres/devices/video/gpu_executor.h>
#include <ceres/devices/video/software_executor.h>
#include <ceres/devices/video/text_plane.h>
#include <ceres/vm/mmio_bus.h>

#include <functional>
#include <memory>

namespace ceres::devices
{
	using namespace vm;

	class GpuDevice final : public IODevice
	{
	public:
		// The core.
		static inline constexpr Address IdRegister = Address(0x000);            // R: 0x55504743, "CGPU"
		static inline constexpr Address VersionRegister = Address(0x004);       // R: major << 16 | minor
		static inline constexpr Address CapsRegister = Address(0x008);          // R: b0-b6 levels, b8-b15 formats, b31 hardware executor
		static inline constexpr Address VramSizeRegister = Address(0x00C);      // R: bytes
		static inline constexpr Address GpuClockHzRegister = Address(0x010);    // R
		static inline constexpr Address ModeRegister = Address(0x014);          // RW: the video level, 0-6, never above MaxLevel
		static inline constexpr Address MaxLevelRegister = Address(0x018);      // R: the profile's
		static inline constexpr Address ControlRegister = Address(0x01C);       // RW: b0 display on, b1 command processor, b2 reset the GPU
		static inline constexpr Address StatusRegister = Address(0x020);        // R: b0 busy, b1 fault, b2 in the vertical blank, b3 flip pending
		static inline constexpr Address IrqEnableRegister = Address(0x024);     // RW: b0 VBlank (32), b1 line (33), b2 fence/copy (34), b3 fault (35)
		static inline constexpr Address IrqStatusRegister = Address(0x028);     // W1C
		static inline constexpr Address FaultCodeRegister = Address(0x02C);     // R
		static inline constexpr Address FaultAddressRegister = Address(0x030);  // R
		// The display.
		static inline constexpr Address WidthRegister = Address(0x100);         // RW: pixels, up to the profile's largest
		static inline constexpr Address HeightRegister = Address(0x104);        // RW
		static inline constexpr Address RefreshRegister = Address(0x108);       // R: 50 or 60
		static inline constexpr Address LinesTotalRegister = Address(0x10C);    // R: visible lines plus the blanking
		static inline constexpr Address VCountRegister = Address(0x110);        // R: the line being scanned now
		static inline constexpr Address LineCompareRegister = Address(0x114);   // RW: interrupt 33 when the scan reaches this line
		static inline constexpr Address FrameCounterRegister = Address(0x118);  // R: vertical blanks since the start
		static inline constexpr Address BackgroundColorRegister = Address(0x11C); // RW: 0x00RRGGBB behind every plane
		static inline constexpr Address PresentRegister = Address(0x120);       // W: 1 applies the pending bases at the next vertical blank

		static inline constexpr u32 IdValue = 0x55504743;
		static inline constexpr u32 VersionValue = 0x00010000;   // 1.0
		// The highest level this GPU builds so far: V1. A Mode above it (or above the profile's MaxLevel) is clamped.
		static inline constexpr u32 ImplementedLevel = 1;
		static inline constexpr u32 CapsHardwareExecutor = 1u << 31;

		static inline constexpr u32 ControlDisplayOn = 1u << 0;
		static inline constexpr u32 ControlCommandProcessor = 1u << 1;
		static inline constexpr u32 ControlReset = 1u << 2;

		static inline constexpr u32 StatusBusy = 1u << 0;
		static inline constexpr u32 StatusFault = 1u << 1;
		static inline constexpr u32 StatusVblank = 1u << 2;
		static inline constexpr u32 StatusFlipPending = 1u << 3;

		static inline constexpr u32 IrqVblank = 1u << 0;
		static inline constexpr u32 IrqLine = 1u << 1;
		static inline constexpr u32 IrqCopy = 1u << 2;
		static inline constexpr u32 IrqFault = 1u << 3;

		// 32-35 (plan/v2 SPEC 5.6).
		static inline constexpr InterruptNumber VblankInterrupt = InterruptNumber::UserInterrupt16;
		static inline constexpr InterruptNumber LineInterrupt = InterruptNumber::UserInterrupt17;
		static inline constexpr InterruptNumber CopyInterrupt = InterruptNumber::UserInterrupt18;
		static inline constexpr InterruptNumber FaultInterrupt = InterruptNumber::UserInterrupt19;

		// The scheduler tags of the GPU's events.
		static inline constexpr u32 VblankEvent = 0;
		static inline constexpr u32 LineEvent = 1;
		static inline constexpr u32 CopyEvent = 2;

		// The resolution the GPU starts in: 640x480 (80x30 cells of text), or the profile's largest when that is smaller.
		static inline constexpr u32 BootWidth = 640;
		static inline constexpr u32 BootHeight = 480;

		// What the machine's profile gives the GPU (plan/v2 SPEC 4); set before it is attached.
		struct Config
		{
			u64 gpuClockHz = 200'000'000;
			u32 maxLevel = 5;
			u32 maxWidth = 1280;
			u32 maxHeight = 720;
			u32 refresh = 60;
		};

		// Called on the machine's thread at every vertical blank, after the pending bases were applied; `presented`
		// says whether a Present was among them. A host composes its frame here.
		using VblankObserver = std::function<void(bool presented)>;

	private:
		Config _config;
		video::DisplayController _display;
		std::unique_ptr<video::GpuExecutor> _executor = std::make_unique<video::SoftwareExecutor>();
		video::TextPlane _text;

		u32 _mode = 0;
		u32 _control = ControlDisplayOn;
		u32 _irqEnable = 0;
		u32 _irqStatus = 0;
		u32 _faultCode = 0;
		u32 _faultAddress = 0;
		u32 _width = BootWidth;
		u32 _height = BootHeight;
		u32 _lineCompare = 0;
		u32 _background = 0;
		bool _presentPending = false;
		u64 _frameCounter = 0;   // vertical blanks since the start
		u64 _nextFrame = 0;      // the frame whose vertical blank is scheduled
		VblankObserver _vblankObserver;

	public:
		GpuDevice() = default;
		GpuDevice(const GpuDevice&) = delete;
		GpuDevice(GpuDevice&&) = delete;
		~GpuDevice() override = default;

		GpuDevice& operator=(const GpuDevice&) = delete;
		GpuDevice& operator=(GpuDevice&&) = delete;

	public:
		void configure(const Config& config) noexcept;
		const Config& config() const noexcept { return _config; }

		void attachTo(MmioBus& bus);
		void detachFrom(MmioBus& bus);

		void setVblankObserver(VblankObserver observer) { _vblankObserver = std::move(observer); }

		// The picture as the scanout would show it now.
		void compose(video::VideoFrame& frame);

		u32 width() const noexcept { return _width; }
		u32 height() const noexcept { return _height; }
		u32 mode() const noexcept { return _mode; }
		u64 frameCounter() const noexcept { return _frameCounter; }
		const video::DisplayController& display() const noexcept { return _display; }
		video::TextPlane& textPlane() noexcept { return _text; }
		const video::TextPlane& textPlane() const noexcept { return _text; }
		// The text plane's screen as text, a line a row (what --screen-log writes).
		std::string screenText() const { return _text.toText(vram()); }

		// Back to the state the machine starts in: the boot resolution, level V0, the timing from now.
		void reset() override;
		void onEvent(u32 tag, u64 cycle) override;

		u32 read(Address offset) override;
		void write(Address offset, u32 value) override;
		const RegisterMap& registers() const override;

	private:
		u64 now() const noexcept;
		u32 availableLevel() const noexcept;
		void setResolution(u32 width, u32 height);
		void scheduleVblank();
		void scheduleLine();
		void vblank();
	};
}
