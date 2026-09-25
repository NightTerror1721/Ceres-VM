#include "console_input.h"

#include <ceres/devices/input/keyboard.h>
#include <ceres/driver/key_decoder.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace ceres::driver
{
	namespace
	{
		constexpr int PollMilliseconds = 20; // how long a wait may last before the reader looks at its flags again

#if defined(_WIN32)

		// The scancode a Windows virtual key stands for, in the SDL numbering the keyboard device reports, or 0
		// for a key the device has no name for (the modifiers, the lock keys, punctuation).
		u32 scancodeForVirtualKey(WORD vk) noexcept
		{
			using namespace devices::scancode;
			if (vk >= 'A' && vk <= 'Z') return 4 + (vk - 'A');
			if (vk >= '1' && vk <= '9') return 30 + (vk - '1');
			if (vk == '0') return 39;
			if (vk >= VK_F1 && vk <= VK_F12) return F1 + (vk - VK_F1);
			switch (vk)
			{
				case VK_RETURN: return Return;
				case VK_ESCAPE: return Escape;
				case VK_BACK: return Backspace;
				case VK_TAB: return Tab;
				case VK_SPACE: return Space;
				case VK_INSERT: return Insert;
				case VK_HOME: return Home;
				case VK_PRIOR: return PageUp;
				case VK_DELETE: return Delete;
				case VK_END: return End;
				case VK_NEXT: return PageDown;
				case VK_RIGHT: return Right;
				case VK_LEFT: return Left;
				case VK_DOWN: return Down;
				case VK_UP: return Up;
				default: return 0;
			}
		}

		// Typed by setRaw() to end a pending line read (Ctrl+_): a control character nobody types, so ending
		// a read with it costs a person nothing.
		constexpr wchar_t WakeupCharacter = 0x1F;

		class WindowsConsole;
		std::atomic<WindowsConsole*> activeConsole{nullptr};

		class WindowsConsole final : public ConsoleInput
		{
		public:
			WindowsConsole(HANDLE input, DWORD original) : _input(input), _original(original)
			{
				activeConsole.store(this);
				SetConsoleCtrlHandler(&WindowsConsole::onCtrl, TRUE);
			}

			~WindowsConsole() override
			{
				restore();
				SetConsoleCtrlHandler(&WindowsConsole::onCtrl, FALSE);
				activeConsole.store(nullptr);
			}

			void setRaw(bool raw) override
			{
				_wantRaw.store(raw, std::memory_order_release);
				if (!raw)
					return;
				// The reader may be parked in a line read that only Enter would end. Type the wake-up character
				// for it: the read returns what was typed so far plus that character, without echoing
				// anything, and the reader sees the flag and switches the console itself. (Cancelling the read
				// instead was measured, and does not work: the read returns empty, but the console goes on
				// consuming keys for it until an Enter, so the first keys a program is waiting for vanish.)
				// A character typed just before the read begins is not lost - it starts the next one - so keep
				// typing it while the read is marked as under way.
				for (int attempt = 0; attempt < 200 && _inRead.load(std::memory_order_acquire); ++attempt)
				{
					INPUT_RECORD wake{};
					wake.EventType = KEY_EVENT;
					wake.Event.KeyEvent.bKeyDown = TRUE;
					wake.Event.KeyEvent.wRepeatCount = 1;
					wake.Event.KeyEvent.wVirtualKeyCode = VK_OEM_MINUS;
					wake.Event.KeyEvent.uChar.UnicodeChar = WakeupCharacter;
					wake.Event.KeyEvent.dwControlKeyState = LEFT_CTRL_PRESSED;
					DWORD written = 0;
					WriteConsoleInputW(_input, &wake, 1, &written);
					std::this_thread::sleep_for(std::chrono::milliseconds(2));
				}
			}

			void restore() override
			{
				_done.store(true, std::memory_order_release);
				_wantRaw.store(false, std::memory_order_release);
				SetConsoleMode(_input, _original);
			}

			void run(const Sink& sink) override
			{
				HANDLE self = nullptr;
				DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
				_reader.store(self, std::memory_order_release);

				wchar_t highSurrogate = 0;
				bool raw = false;
				while (!_done.load(std::memory_order_acquire) && !(sink.stopped && sink.stopped()))
				{
					const bool want = _wantRaw.load(std::memory_order_acquire);
					if (want != raw)
					{
						// The switch is made here, on the thread that reads, so no read can be under way
						// while the mode changes underneath it.
						SetConsoleMode(_input, want ? rawMode() : _original);
						FlushConsoleInputBuffer(_input);   // what was typed before the switch belongs to the old mode
						raw = want;
					}
					const bool more = raw ? readKeys(sink, highSurrogate) : readLine(sink);
					if (!more)
						break;
				}
				_reader.store(nullptr, std::memory_order_release);
				if (self)
					CloseHandle(self);
			}

		private:
			DWORD rawMode() const
			{
				// Keys as events: no line editing, no echo. Ctrl+C is still Ctrl+C, and whatever the console
				// had for quick-edit and the like is left as it was.
				DWORD mode = _original;
				mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
				mode |= ENABLE_PROCESSED_INPUT | ENABLE_EXTENDED_FLAGS;
				return mode;
			}

			static BOOL WINAPI onCtrl(DWORD)
			{
				if (WindowsConsole* console = activeConsole.load())
					SetConsoleMode(console->_input, console->_original);
				return FALSE; // and let the default handling end the process
			}

			// Cooked: one line, as the console's own editor produced it.
			bool readLine(const Sink& sink)
			{
				wchar_t buffer[512];
				DWORD count = 0;
				_inRead.store(true, std::memory_order_release);
				if (_wantRaw.load(std::memory_order_acquire) || _done.load(std::memory_order_acquire))
				{
					_inRead.store(false, std::memory_order_release);
					return true;   // the flag flipped before the read began: go round and switch
				}
				// The wake-up character ends the read wherever it is typed, mid-line, without a newline.
				CONSOLE_READCONSOLE_CONTROL control{};
				control.nLength = sizeof(control);
				control.dwCtrlWakeupMask = 1u << WakeupCharacter;
				const BOOL ok = ReadConsoleW(_input, buffer, 512, &count, &control);
				_inRead.store(false, std::memory_order_release);
				if (!ok)
				{
					if (sink.endOfInput) sink.endOfInput();
					return false;
				}
				if (count == 0)
				{
					// Not the end of the input: Ctrl+Z, the console's end-of-file, arrives as a character.
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					return true;
				}

				// Woken to switch to raw keys: what was typed so far belongs to the old mode, and is dropped.
				if (buffer[count - 1] == WakeupCharacter && (_wantRaw.load(std::memory_order_acquire) || _done.load(std::memory_order_acquire)))
					return true;

				// Ctrl+Z is the console's end-of-file: what precedes it is still text.
				bool endOfFile = false;
				std::wstring wide;
				for (DWORD i = 0; i < count; ++i)
				{
					if (buffer[i] == WakeupCharacter) continue;   // a person typed it: it is no text
					if (buffer[i] == 0x1A) { endOfFile = true; break; }
					if (buffer[i] == L'\r' && i + 1 < count && buffer[i + 1] == L'\n') continue; // a line ends in \n, as it does through a stream
					wide += buffer[i];
				}
				if (!wide.empty())
				{
					std::string utf8(static_cast<usize>(WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr)), '\0');
					WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
					if (sink.bytes)
						sink.bytes(std::span<const u8>(reinterpret_cast<const u8*>(utf8.data()), utf8.size()));
				}
				if (endOfFile)
				{
					if (sink.endOfInput) sink.endOfInput();
					return false;
				}
				return true;
			}

			// Raw: key events, as the console reports them - a key down, a key up, and the character it made.
			bool readKeys(const Sink& sink, wchar_t& highSurrogate)
			{
				if (WaitForSingleObject(_input, PollMilliseconds) != WAIT_OBJECT_0)
					return true;   // quiet: back to the flags
				INPUT_RECORD records[64];
				DWORD count = 0;
				if (!ReadConsoleInputW(_input, records, 64, &count))
				{
					if (sink.endOfInput) sink.endOfInput();
					return false;
				}
				for (DWORD i = 0; i < count; ++i)
				{
					if (records[i].EventType != KEY_EVENT)
						continue;
					const KEY_EVENT_RECORD& key = records[i].Event.KeyEvent;
					const u32 code = scancodeForVirtualKey(key.wVirtualKeyCode);
					if (!key.bKeyDown)
					{
						if (code != 0 && sink.key) sink.key(code, false);
						continue;
					}
					const wchar_t ch = key.uChar.UnicodeChar;
					const bool named = devices::scancode::isNamedKey(code);
					for (WORD repeat = 0; repeat < (key.wRepeatCount ? key.wRepeatCount : 1); ++repeat)
					{
						if (code != 0 && sink.key) sink.key(code, true);
						if (named || ch < 32 || ch == 127)
							continue;   // a named key types no character of its own; a control code types none
						u32 codePoint = ch;
						if (ch >= 0xD800 && ch < 0xDC00) { highSurrogate = ch; continue; }
						if (ch >= 0xDC00 && ch < 0xE000)
						{
							if (highSurrogate == 0) continue;
							codePoint = 0x10000 + ((static_cast<u32>(highSurrogate) - 0xD800) << 10) + (ch - 0xDC00);
							highSurrogate = 0;
						}
						if (sink.text) sink.text(codePoint);
					}
				}
				return true;
			}

			HANDLE _input;
			DWORD _original;
			std::atomic<bool> _wantRaw{false};
			std::atomic<bool> _inRead{false};
			std::atomic<bool> _done{false};
			std::atomic<HANDLE> _reader{nullptr};
		};

#else // POSIX

		class PosixConsole;
		std::atomic<PosixConsole*> activeConsole{nullptr};

		class PosixConsole final : public ConsoleInput
		{
		public:
			explicit PosixConsole(const termios& original) : _original(original)
			{
				activeConsole.store(this);
				// Ctrl+C, a hangup or a kill must not leave the terminal without echo.
				struct sigaction action{};
				action.sa_handler = &PosixConsole::onSignal;
				sigemptyset(&action.sa_mask);
				for (int number : { SIGINT, SIGTERM, SIGHUP })
					sigaction(number, &action, &_previous[number == SIGINT ? 0 : number == SIGTERM ? 1 : 2]);
			}

			~PosixConsole() override
			{
				restore();
				const int numbers[] = { SIGINT, SIGTERM, SIGHUP };
				for (int i = 0; i < 3; ++i)
					sigaction(numbers[i], &_previous[i], nullptr);
				activeConsole.store(nullptr);
			}

			void setRaw(bool raw) override { _wantRaw.store(raw, std::memory_order_release); }

			void restore() override
			{
				_done.store(true, std::memory_order_release);
				_wantRaw.store(false, std::memory_order_release);
				tcsetattr(STDIN_FILENO, TCSANOW, &_original);
			}

			void run(const Sink& sink) override
			{
				bool raw = false;
				KeyDecoder decoder;
				std::vector<KeyAction> actions;
				while (!_done.load(std::memory_order_acquire) && !(sink.stopped && sink.stopped()))
				{
					const bool want = _wantRaw.load(std::memory_order_acquire);
					if (want != raw)
					{
						if (want)
						{
							// Keys as they are pressed and no echo; signals and output processing stay as
							// they were, so Ctrl+C still interrupts and "\n" still ends a line.
							termios mode = _original;
							mode.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | IEXTEN);
							mode.c_iflag &= ~static_cast<tcflag_t>(ICRNL | INLCR | IGNCR | IXON);
							mode.c_cc[VMIN] = 1;
							mode.c_cc[VTIME] = 0;
							tcsetattr(STDIN_FILENO, TCSAFLUSH, &mode);
						}
						else
							tcsetattr(STDIN_FILENO, TCSAFLUSH, &_original);
						decoder = KeyDecoder{};
						raw = want;
					}

					pollfd waiting{STDIN_FILENO, POLLIN, 0};
					const int ready = poll(&waiting, 1, PollMilliseconds);
					if (ready < 0)
						continue;   // interrupted
					if (ready == 0)
					{
						// Quiet. A lone Escape is a key, not the start of a sequence.
						if (raw && decoder.pending())
						{
							actions.clear();
							decoder.flushPending(actions);
							deliver(sink, actions);
						}
						continue;
					}

					u8 buffer[256];
					const ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
					if (count <= 0)
					{
						if (sink.endOfInput) sink.endOfInput();
						return;
					}
					if (!raw)
					{
						if (sink.bytes) sink.bytes(std::span<const u8>(buffer, static_cast<usize>(count)));
						continue;
					}
					actions.clear();
					for (ssize_t i = 0; i < count; ++i)
						decoder.feed(buffer[i], actions);
					deliver(sink, actions);
				}
			}

		private:
			static void deliver(const Sink& sink, const std::vector<KeyAction>& actions)
			{
				for (const KeyAction& action : actions)
				{
					if (action.isText) { if (sink.text) sink.text(action.value); }
					else if (sink.key) sink.key(action.value, action.pressed);
				}
			}

			static void onSignal(int number)
			{
				if (PosixConsole* console = activeConsole.load())
					tcsetattr(STDIN_FILENO, TCSANOW, &console->_original);
				signal(number, SIG_DFL);
				raise(number);
			}

			termios _original;
			struct sigaction _previous[3]{};
			std::atomic<bool> _wantRaw{false};
			std::atomic<bool> _done{false};
		};

#endif
	}

	std::shared_ptr<ConsoleInput> ConsoleInput::open()
	{
#if defined(_WIN32)
		const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
		DWORD mode = 0;
		if (input == nullptr || input == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &mode))
			return nullptr;
		return std::make_shared<WindowsConsole>(input, mode);
#else
		termios original{};
		if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &original) != 0)
			return nullptr;
		return std::make_shared<PosixConsole>(original);
#endif
	}
}
