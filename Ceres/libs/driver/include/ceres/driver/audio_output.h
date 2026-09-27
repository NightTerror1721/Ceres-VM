#pragma once

// The host's speakers. A host without them (the default) is no AudioOutput, and the machine is silent.

#include <ceres/devices/audio/audio.h>

namespace ceres::driver
{
	class AudioOutput
	{
	public:
		virtual ~AudioOutput() = default;

		// Give the host the audio device to play, by installing a sink on it. Called once before the machine runs.
		virtual void attachAudio(devices::AudioDevice&) = 0;

		// The machine is done with the audio device: stop making sound and never touch it again.
		virtual void detachAudio() = 0;
	};
}
