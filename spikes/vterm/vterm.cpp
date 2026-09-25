// F0.4 spike: the virtual terminal's output side (plan/v2 SPEC §8.2) run over real Ceres program output.
//
//   vterm [--cols <n>] [--rows <n>] [--screen] <file>...
//
// Interprets each file as a byte stream written to the terminal: UTF-8, the control characters and the ANSI subset
// of §8.2, on a grid of cells with a cursor, colours, a saved cursor and a scroll region. Prints every distinct
// sequence it met, how many times, and whether §8.2 covers it; --screen also prints the final grid as text. Exits 1
// when some sequence is not covered.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace
{
	struct Cell
	{
		char32_t ch = U' ';
		std::uint8_t fg = 7;
		std::uint8_t bg = 0;
		bool bold = false;
		bool inverse = false;
	};

	struct Seen
	{
		std::size_t count = 0;
		bool covered = false;
	};

	class Terminal
	{
		int _cols;
		int _rows;
		std::vector<Cell> _cells;
		int _x = 0;
		int _y = 0;
		int _savedX = 0;
		int _savedY = 0;
		int _top = 0;
		int _bottom;
		Cell _pen;
		bool _cursorVisible = true;

		enum class State { Ground, Escape, Csi };
		State _state = State::Ground;
		std::string _sequence;   // the bytes of the escape sequence being read, after ESC
		char32_t _utf8 = 0;
		int _utf8Left = 0;

	public:
		std::map<std::string, Seen> seen;
		std::size_t invalidUtf8 = 0;
		std::size_t bells = 0;

		Terminal(int cols, int rows) : _cols(cols), _rows(rows), _cells(static_cast<std::size_t>(cols * rows)), _bottom(rows - 1) {}

		void write(unsigned char byte)
		{
			switch (_state)
			{
				case State::Ground: ground(byte); break;
				case State::Escape: escape(byte); break;
				case State::Csi: csi(byte); break;
			}
		}

		std::string screen() const
		{
			std::string out;
			for (int y = 0; y < _rows; ++y)
			{
				std::string line;
				for (int x = 0; x < _cols; ++x)
					appendUtf8(line, _cells[static_cast<std::size_t>(y * _cols + x)].ch);
				line.erase(line.find_last_not_of(' ') + 1);
				out += line + '\n';
			}
			return out;
		}

		bool cursorVisible() const { return _cursorVisible; }

	private:
		static void appendUtf8(std::string& out, char32_t c)
		{
			if (c < 0x80) out += static_cast<char>(c);
			else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
			else if (c < 0x10000) { out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
			else { out += static_cast<char>(0xF0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F)); }
		}

		void note(const std::string& key, bool covered)
		{
			Seen& s = seen[key];
			++s.count;
			s.covered = covered;
		}

		Cell& at(int x, int y) { return _cells[static_cast<std::size_t>(y * _cols + x)]; }

		void clamp()
		{
			_x = std::clamp(_x, 0, _cols - 1);
			_y = std::clamp(_y, 0, _rows - 1);
		}

		void scrollUp()
		{
			for (int y = _top; y < _bottom; ++y)
				for (int x = 0; x < _cols; ++x)
					at(x, y) = at(x, y + 1);
			for (int x = 0; x < _cols; ++x)
				at(x, _bottom) = Cell{ U' ', _pen.fg, _pen.bg };
		}

		void lineFeed()
		{
			if (_y == _bottom)
				scrollUp();
			else if (_y < _rows - 1)
				++_y;
		}

		void put(char32_t c)
		{
			if (_x >= _cols)
			{
				_x = 0;
				lineFeed();
			}
			Cell cell = _pen;
			cell.ch = c;
			at(_x, _y) = cell;
			++_x;
		}

		void ground(unsigned char b)
		{
			if (_utf8Left > 0)
			{
				if ((b & 0xC0) == 0x80)
				{
					_utf8 = (_utf8 << 6) | (b & 0x3F);
					if (--_utf8Left == 0)
						put(_utf8);
					return;
				}
				++invalidUtf8;
				_utf8Left = 0;
				put(U'?');
			}
			if (b == 0x1B) { _state = State::Escape; _sequence.clear(); return; }
			if (b >= 0x80)
			{
				if ((b & 0xE0) == 0xC0) { _utf8 = b & 0x1F; _utf8Left = 1; }
				else if ((b & 0xF0) == 0xE0) { _utf8 = b & 0x0F; _utf8Left = 2; }
				else if ((b & 0xF8) == 0xF0) { _utf8 = b & 0x07; _utf8Left = 3; }
				else { ++invalidUtf8; put(U'?'); }
				return;
			}
			switch (b)
			{
				case '\r': _x = 0; note("CR", true); return;
				case '\n': _x = 0; lineFeed(); note("LF", true); return;
				case '\b': if (_x > 0) --_x; note("BS", true); return;
				case '\t': _x = std::min(_cols - 1, (_x / 8 + 1) * 8); note("TAB", true); return;
				case '\a': ++bells; note("BEL", true); return;
				default: break;
			}
			if (b < 0x20 || b == 0x7F)
			{
				char name[16];
				std::snprintf(name, sizeof name, "C0 0x%02X", b);
				note(name, false);
				return;
			}
			put(b);
		}

		void escape(unsigned char b)
		{
			if (b == '[') { _state = State::Csi; _sequence = "["; return; }
			_state = State::Ground;
			if (b == '7') { _savedX = _x; _savedY = _y; note("ESC 7", true); return; }
			if (b == '8') { _x = _savedX; _y = _savedY; note("ESC 8", true); return; }
			std::string key = "ESC ";
			key += static_cast<char>(b);
			note(key, false);
		}

		static std::vector<int> params(const std::string& body)
		{
			std::vector<int> out;
			int value = -1;
			for (char c : body)
			{
				if (c >= '0' && c <= '9') value = (value < 0 ? 0 : value * 10) + (c - '0');
				else if (c == ';') { out.push_back(value); value = -1; }
			}
			out.push_back(value);
			return out;
		}

		void csi(unsigned char b)
		{
			if (b >= 0x20 && b <= 0x3F) { _sequence += static_cast<char>(b); return; }   // parameters and '?'
			_state = State::Ground;
			const std::string body = _sequence.substr(1);
			const bool privateMode = !body.empty() && body[0] == '?';
			const std::vector<int> p = params(privateMode ? body.substr(1) : body);
			const int n = p[0] < 1 ? 1 : p[0];
			const int mode = p[0] < 0 ? 0 : p[0];
			std::string key = "CSI ";
			key += privateMode ? "?" : "";
			key += static_cast<char>(b);

			if (privateMode)
			{
				const bool cursor = b == 'l' || b == 'h';
				if (cursor && p[0] == 25)
				{
					_cursorVisible = b == 'h';
					note("CSI ?25" + std::string(1, static_cast<char>(b)), true);
				}
				else
					note(key + " " + body, false);
				return;
			}

			switch (b)
			{
				case 'A': _y -= n; clamp(); note(key, true); return;
				case 'B': _y += n; clamp(); note(key, true); return;
				case 'C': _x += n; clamp(); note(key, true); return;
				case 'D': _x -= n; clamp(); note(key, true); return;
				case 'H':
				case 'f':
					_y = (p[0] < 1 ? 1 : p[0]) - 1;
					_x = (p.size() > 1 && p[1] >= 1 ? p[1] : 1) - 1;
					clamp();
					note(key, true);
					return;
				case 'J': erase(mode, true); note(key + " " + std::to_string(mode), mode <= 2); return;
				case 'K': erase(mode, false); note(key + " " + std::to_string(mode), mode <= 2); return;
				case 's': _savedX = _x; _savedY = _y; note(key, true); return;
				case 'u': _x = _savedX; _y = _savedY; note(key, true); return;
				case 'r':
					_top = (p[0] < 1 ? 1 : p[0]) - 1;
					_bottom = (p.size() > 1 && p[1] >= 1 ? std::min(p[1], _rows) : _rows) - 1;
					_x = 0; _y = 0;
					note(key, true);
					return;
				case 'm': sgr(p); return;
				default: note(key + (body.empty() ? "" : " " + body), false); return;
			}
		}

		void erase(int mode, bool screen)
		{
			const Cell blank{ U' ', _pen.fg, _pen.bg };
			const int from = mode == 0 ? _x : 0;
			const int to = mode == 1 ? _x + 1 : _cols;
			for (int x = from; x < to; ++x)
				at(x, _y) = blank;
			if (!screen)
				return;
			const int yFrom = mode == 0 ? _y + 1 : 0;
			const int yTo = mode == 1 ? _y : _rows;
			for (int y = yFrom; y < yTo; ++y)
				for (int x = 0; x < _cols; ++x)
					at(x, y) = blank;
		}

		void sgr(const std::vector<int>& p)
		{
			for (std::size_t i = 0; i < p.size(); ++i)
			{
				const int v = p[i] < 0 ? 0 : p[i];
				if ((v == 38 || v == 48) && i + 2 < p.size() && p[i + 1] == 5)
				{
					(v == 38 ? _pen.fg : _pen.bg) = static_cast<std::uint8_t>(p[i + 2]);
					note(v == 38 ? "SGR 38;5;n" : "SGR 48;5;n", true);
					i += 2;
					continue;
				}
				if ((v == 38 || v == 48) && i + 4 < p.size() && p[i + 1] == 2)
				{
					note(v == 38 ? "SGR 38;2;r;g;b" : "SGR 48;2;r;g;b", false);
					i += 4;
					continue;
				}
				bool covered = true;
				if (v == 0) _pen = Cell{};
				else if (v == 1) _pen.bold = true;
				else if (v == 7) _pen.inverse = true;
				else if (v == 22) _pen.bold = false;
				else if (v == 27) _pen.inverse = false;
				else if (v >= 30 && v <= 37) _pen.fg = static_cast<std::uint8_t>(v - 30);
				else if (v == 39) _pen.fg = 7;
				else if (v >= 40 && v <= 47) _pen.bg = static_cast<std::uint8_t>(v - 40);
				else if (v == 49) _pen.bg = 0;
				else if (v >= 90 && v <= 97) _pen.fg = static_cast<std::uint8_t>(v - 90 + 8);
				else if (v >= 100 && v <= 107) _pen.bg = static_cast<std::uint8_t>(v - 100 + 8);
				else covered = false;
				std::string key = "SGR ";
				if (v >= 30 && v <= 37) key += "30-37";
				else if (v >= 40 && v <= 47) key += "40-47";
				else if (v >= 90 && v <= 97) key += "90-97";
				else if (v >= 100 && v <= 107) key += "100-107";
				else key += std::to_string(v);
				note(key, covered);
			}
		}
	};

	[[noreturn]] void usage()
	{
		std::fprintf(stderr, "usage: vterm [--cols <n>] [--rows <n>] [--screen] <file>...\n");
		std::exit(2);
	}
}

int main(int argc, char** argv)
{
	int cols = 80;
	int rows = 25;
	bool showScreen = false;
	std::vector<std::string> files;
	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		if (a == "--cols" && i + 1 < argc) cols = std::atoi(argv[++i]);
		else if (a == "--rows" && i + 1 < argc) rows = std::atoi(argv[++i]);
		else if (a == "--screen") showScreen = true;
		else if (!a.empty() && a[0] == '-') usage();
		else files.push_back(a);
	}
	if (files.empty() || cols < 1 || rows < 1)
		usage();

	bool allCovered = true;
	for (const std::string& file : files)
	{
		std::ifstream in(file, std::ios::binary);
		if (!in)
		{
			std::fprintf(stderr, "vterm: cannot open %s\n", file.c_str());
			return 2;
		}
		const std::string bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
		Terminal terminal(cols, rows);
		for (const char c : bytes)
			terminal.write(static_cast<unsigned char>(c));

		std::printf("== %s (%zu bytes)\n", file.c_str(), bytes.size());
		for (const auto& [key, s] : terminal.seen)
		{
			std::printf("  %-8s %6zu  %s\n", s.covered ? "ok" : "MISSING", s.count, key.c_str());
			allCovered = allCovered && s.covered;
		}
		if (terminal.invalidUtf8 != 0)
			std::printf("  invalid UTF-8 sequences: %zu\n", terminal.invalidUtf8);
		if (showScreen)
			std::printf("-- final screen (cursor %s)\n%s", terminal.cursorVisible() ? "visible" : "hidden", terminal.screen().c_str());
	}
	return allCovered ? 0 : 1;
}
