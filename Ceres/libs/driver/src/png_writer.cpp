#include "png_writer.h"

#include <algorithm>
#include <array>
#include <fstream>

namespace ceres::driver
{
	namespace
	{
		constexpr std::array<u32, 256> crcTable() noexcept
		{
			std::array<u32, 256> table{};
			for (u32 n = 0; n < 256; ++n)
			{
				u32 c = n;
				for (int k = 0; k < 8; ++k)
					c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
				table[n] = c;
			}
			return table;
		}

		constexpr std::array<u32, 256> CrcTable = crcTable();

		void putBig(std::string& out, u32 value)
		{
			out += static_cast<char>(value >> 24);
			out += static_cast<char>(value >> 16);
			out += static_cast<char>(value >> 8);
			out += static_cast<char>(value);
		}

		// A chunk: its length, its type and data, and the CRC of the two.
		void putChunk(std::string& out, const char* type, const std::string& data)
		{
			putBig(out, static_cast<u32>(data.size()));
			const usize start = out.size();
			out.append(type, 4);
			out += data;
			u32 crc = 0xFFFFFFFFu;
			for (usize i = start; i < out.size(); ++i)
				crc = CrcTable[(crc ^ static_cast<u8>(out[i])) & 0xFF] ^ (crc >> 8);
			putBig(out, crc ^ 0xFFFFFFFFu);
		}
	}

	std::string encodePng(const devices::video::VideoFrame& frame)
	{
		// The rows as PNG wants them: a filter byte (0, none) and then red, green, blue for each pixel.
		std::string raw;
		raw.reserve(static_cast<usize>(frame.height) * (1 + 3 * static_cast<usize>(frame.width)));
		for (u32 y = 0; y < frame.height; ++y)
		{
			raw += '\0';
			for (u32 x = 0; x < frame.width; ++x)
			{
				const u32 pixel = frame.pixels[static_cast<usize>(y) * frame.width + x];
				raw += static_cast<char>(pixel >> 16);
				raw += static_cast<char>(pixel >> 8);
				raw += static_cast<char>(pixel);
			}
		}

		// zlib: a header, stored deflate blocks of up to 65535 bytes, and the Adler-32 of the data.
		std::string zlib = "\x78\x01";
		usize at = 0;
		do
		{
			const usize length = std::min<usize>(65535, raw.size() - at);
			const bool last = at + length == raw.size();
			zlib += static_cast<char>(last ? 1 : 0);
			zlib += static_cast<char>(length & 0xFF);
			zlib += static_cast<char>(length >> 8);
			zlib += static_cast<char>(~length & 0xFF);
			zlib += static_cast<char>((~length >> 8) & 0xFF);
			zlib.append(raw, at, length);
			at += length;
		} while (at < raw.size());
		u32 a = 1, b = 0;
		for (char c : raw)
		{
			a = (a + static_cast<u8>(c)) % 65521;
			b = (b + a) % 65521;
		}
		putBig(zlib, (b << 16) | a);

		std::string header;
		putBig(header, frame.width);
		putBig(header, frame.height);
		header += '\x08';   // 8 bits a channel
		header += '\x02';   // RGB
		header += std::string(3, '\0');   // deflate, adaptive filtering, no interlace

		std::string png = "\x89PNG\r\n\x1A\n";
		putChunk(png, "IHDR", header);
		putChunk(png, "IDAT", zlib);
		putChunk(png, "IEND", {});
		return png;
	}

	bool writePng(const std::filesystem::path& path, const devices::video::VideoFrame& frame)
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		const std::string bytes = encodePng(frame);
		file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		return static_cast<bool>(file);
	}
}
