#include "headless_output.h"

#include <format>

namespace ceres::driver
{
	std::string HeadlessOutput::open(const std::filesystem::path& transcript, const std::filesystem::path& screenLog)
	{
		if (!transcript.empty())
		{
			_transcript.open(transcript, std::ios::binary | std::ios::trunc);
			if (!_transcript)
				return "Cannot write the transcript " + transcript.string();
		}
		if (!screenLog.empty())
		{
			_screenLog.open(screenLog, std::ios::binary | std::ios::trunc);
			if (!_screenLog)
				return "Cannot write the screen log " + screenLog.string();
		}
		return {};
	}

	void HeadlessOutput::transcriptByte(u8 byte, bool error)
	{
		if (!_transcript.is_open())
			return;
		if (error != _inError)
		{
			_transcript << (error ? ErrorStart : ErrorEnd);
			_inError = error;
		}
		_transcript.put(static_cast<char>(byte));
	}

	void HeadlessOutput::screen(std::string_view text, bool final)
	{
		if (!_screenLog.is_open())
			return;
		_screenLog << (final ? std::string("--- end ---\n") : std::format("--- present {} ---\n", ++_screens)) << text;
	}

	void HeadlessOutput::finish()
	{
		if (_transcript.is_open())
		{
			if (_inError)
				_transcript << ErrorEnd;
			_inError = false;
			_transcript.flush();
		}
		if (_screenLog.is_open())
			_screenLog.flush();
	}
}
