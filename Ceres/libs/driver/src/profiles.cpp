#include <ceres/driver/profiles.h>

#include <ceres/vm/memory.h>
#include <ceres/vm/vram.h>
#include <array>
#include <format>

namespace ceres::driver
{
	namespace
	{
		constexpr usize KiB = 1024;
		constexpr usize MiB = 1024 * KiB;
		constexpr u64 MHz = 1'000'000;

		// plan/v2 SPEC 4, row by row. `custom` starts as `standard` with custom's ceilings for what it shows. A `custom`
		// made from micro or pocket keeps their VRAM written only in the vertical blank.
		constexpr std::array<MachineProfile, 8> Profiles{ {
			{ ProfileId::Micro,       2 * MHz,   2 * MHz,  64 * KiB,  32 * KiB, 2,  256, 192, 1,  16, true },
			{ ProfileId::Pocket,      8 * MHz,   8 * MHz, 512 * KiB,  96 * KiB, 2,  240, 160, 1,  32, true },
			{ ProfileId::Retro,      16 * MHz,  32 * MHz,   2 * MiB, 512 * KiB, 2,  320, 240, 2,  32, false },
			{ ProfileId::Arcade,     25 * MHz,  50 * MHz,   8 * MiB,   4 * MiB, 3,  640, 480, 3,  96, false },
			{ ProfileId::Polygon,    33 * MHz,  66 * MHz,  16 * MiB,   8 * MiB, 5,  640, 480, 3,  96, false },
			{ ProfileId::Standard,   50 * MHz, 200 * MHz,  64 * MiB,  32 * MiB, 5, 1280, 720, 4, 128, false },
			{ ProfileId::Workstation, 100 * MHz, 400 * MHz, 512 * MiB, 256 * MiB, 6, 1280, 720, 4, 256, false },
			{ ProfileId::Custom,     50 * MHz, 200 * MHz,  64 * MiB,  32 * MiB, 6, 1920, 1080, 4, 256, false },
		} };

		constexpr std::array<std::string_view, 8> Names{ "micro", "pocket", "retro", "arcade", "polygon", "standard", "workstation", "custom" };

		static_assert(Profiles[static_cast<usize>(ProfileId::Standard)].ramBytes == vm::Memory::DefaultSize,
			"the default machine's RAM is the standard profile's");
		static_assert(Profiles[static_cast<usize>(ProfileId::Standard)].vramBytes == vm::Vram::DefaultSize,
			"the default machine's VRAM is the standard profile's");
	}

	std::span<const MachineProfile> machineProfiles() noexcept
	{
		return Profiles;
	}

	const MachineProfile& machineProfile(ProfileId id) noexcept
	{
		return Profiles[static_cast<usize>(id)];
	}

	std::string_view profileName(ProfileId id) noexcept
	{
		return Names[static_cast<usize>(id)];
	}

	std::optional<ProfileId> profileNamed(std::string_view name) noexcept
	{
		for (usize i = 0; i < Names.size(); ++i)
			if (Names[i] == name)
				return static_cast<ProfileId>(i);
		return std::nullopt;
	}

	std::expected<void, std::string> checkCustomProfile(const MachineProfile& profile)
	{
		if (profile.cpuClockHz == 0 || profile.cpuClockHz > ProfileLimits::MaxCpuClockHz)
			return std::unexpected(std::format("the CPU clock goes from 1 Hz to {} Hz", ProfileLimits::MaxCpuClockHz));
		if (profile.gpuClockHz == 0 || profile.gpuClockHz > ProfileLimits::MaxGpuClockHz)
			return std::unexpected(std::format("the GPU clock goes from 1 Hz to {} Hz", ProfileLimits::MaxGpuClockHz));
		if (profile.ramBytes < vm::Memory::MinSize || profile.ramBytes > vm::Memory::MaxSize || profile.ramBytes % 4096 != 0)
			return std::unexpected(std::format("the RAM is a multiple of 4096 bytes from {} to {}", vm::Memory::MinSize, vm::Memory::MaxSize));
		if (profile.vramBytes < vm::Vram::MinSize || profile.vramBytes > vm::Vram::MaxSize || profile.vramBytes % 4096 != 0)
			return std::unexpected(std::format("the VRAM is a multiple of 4096 bytes from {} to {}", vm::Vram::MinSize, vm::Vram::MaxSize));
		if (profile.maxVideo > ProfileLimits::MaxVideo)
			return std::unexpected(std::format("the video levels go from V0 to V{}", ProfileLimits::MaxVideo));
		if (profile.maxAudio > ProfileLimits::MaxAudio)
			return std::unexpected(std::format("the audio levels go from A0 to A{}", ProfileLimits::MaxAudio));
		if (profile.maxWidth < ProfileLimits::MinWidth || profile.maxWidth > ProfileLimits::MaxWidth ||
			profile.maxHeight < ProfileLimits::MinHeight || profile.maxHeight > ProfileLimits::MaxHeight)
			return std::unexpected(std::format("the resolution goes from {}x{} to {}x{}", ProfileLimits::MinWidth, ProfileLimits::MinHeight,
				ProfileLimits::MaxWidth, ProfileLimits::MaxHeight));
		return {};
	}
}
