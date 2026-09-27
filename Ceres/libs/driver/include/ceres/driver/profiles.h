#pragma once

#include <ceres/core/base/types.h>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ceres::driver
{
	// The machine a program runs on (plan/v2 SPEC 4): eight profiles, from a 2 MHz `micro` with 64 KiB of RAM to a
	// `workstation`, and `custom`, which is any of them with an option changed. The number is what SystemControl's
	// ProfileId register reads.
	enum class ProfileId : u32
	{
		Micro = 0,
		Pocket = 1,
		Retro = 2,
		Arcade = 3,
		Polygon = 4,
		Standard = 5,
		Workstation = 6,
		Custom = 7,
	};

	struct MachineProfile
	{
		ProfileId id = ProfileId::Standard;
		u64 cpuClockHz = 0;
		u64 gpuClockHz = 0;
		usize ramBytes = 0;
		usize vramBytes = 0;
		u32 maxVideo = 0;          // the highest video level, V0-V6
		u32 maxWidth = 0;          // the largest resolution
		u32 maxHeight = 0;
		u32 maxAudio = 0;          // the highest audio level, A0-A4
		u32 spritesPerLine = 0;

		bool operator==(const MachineProfile&) const = default;
	};

	// What `custom` may go up to. No other profile passes 1280x720; only `custom` reaches 1920x1080.
	struct ProfileLimits
	{
		static inline constexpr u64 MaxCpuClockHz = 400'000'000;
		static inline constexpr u64 MaxGpuClockHz = 1'000'000'000;
		static inline constexpr u32 MaxVideo = 6;
		static inline constexpr u32 MaxAudio = 4;
		static inline constexpr u32 MaxWidth = 1920;
		static inline constexpr u32 MaxHeight = 1080;
		static inline constexpr u32 MinWidth = 8;       // one text cell
		static inline constexpr u32 MinHeight = 16;
	};

	// Every profile, in ProfileId order.
	std::span<const MachineProfile> machineProfiles() noexcept;
	const MachineProfile& machineProfile(ProfileId id) noexcept;
	// The one a machine gets when nothing is asked for: `standard`.
	inline const MachineProfile& defaultMachineProfile() noexcept { return machineProfile(ProfileId::Standard); }

	std::string_view profileName(ProfileId id) noexcept;
	std::optional<ProfileId> profileNamed(std::string_view name) noexcept;

	// Whether a `custom` profile stays inside what `custom` allows; the reason when it does not.
	std::expected<void, std::string> checkCustomProfile(const MachineProfile& profile);
}
