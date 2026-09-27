#pragma once

// A frame as a PNG file (--frames): 8-bit RGB, the image data stored without compression. Nothing but the standard
// library, and the same bytes for the same frame on every host, so a test can compare the files.

#include <ceres/devices/video/gpu_executor.h>

#include <filesystem>
#include <string>

namespace ceres::driver
{
	// The PNG file's bytes.
	std::string encodePng(const devices::video::VideoFrame& frame);

	// Writes it; false when the file cannot be written.
	bool writePng(const std::filesystem::path& path, const devices::video::VideoFrame& frame);
}
