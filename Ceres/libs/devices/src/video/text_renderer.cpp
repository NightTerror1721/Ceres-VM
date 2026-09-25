#include <ceres/devices/video/text_renderer.h>

namespace ceres::devices
{
	u32 TextRenderer::armsOf(const FramebufferDevice::Frame& frame, u32 row, u32 column) noexcept
	{
		const auto at = [&](u32 r, u32 c) { return frame.cells[static_cast<usize>(r) * frame.width + c]; };
		u32 arms = 0;
		if (column > 0 && joinsHorizontally(at(row, column - 1))) arms |= ArmLeft;
		if (column + 1 < frame.width && joinsHorizontally(at(row, column + 1))) arms |= ArmRight;
		if (row > 0 && joinsVertically(at(row - 1, column))) arms |= ArmUp;
		if (row + 1 < frame.height && joinsVertically(at(row + 1, column))) arms |= ArmDown;
		return arms;
	}

	void TextRenderer::render(const FramebufferDevice::Frame& frame, std::vector<u32>& pixels)
	{
		const u32 width = imageWidth(frame);
		const u32 height = imageHeight(frame);
		pixels.assign(static_cast<usize>(width) * height, colour(DefaultBackground));
		if (frame.cells.size() < static_cast<usize>(frame.width) * frame.height ||
			frame.attributes.size() < frame.cells.size())
			return;

		for (u32 row = 0; row < frame.height; ++row)
		{
			for (u32 column = 0; column < frame.width; ++column)
			{
				const usize index = static_cast<usize>(row) * frame.width + column;
				const u8 attribute = frame.attributes[index];
				const u32 foreground = attribute == 0 ? colour(DefaultForeground) : colour(attribute & 0x0Fu);
				const u32 background = attribute == 0 ? colour(DefaultBackground) : colour(static_cast<u32>(attribute) >> 4);
				const u8 character = frame.cells[index];
				const u32 arms = character == '+' ? armsOf(frame, row, column) : 0;
				for (u32 y = 0; y < CellHeight; ++y)
				{
					u32* line = pixels.data() + static_cast<usize>(row * CellHeight + y) * width + column * CellWidth;
					for (u32 x = 0; x < CellWidth; ++x)
					{
						const bool lit = character == '+' ? plusPixel(arms, x, y) : glyphPixel(character, x, y);
						line[x] = lit ? foreground : background;
					}
				}
			}
		}
	}
}
