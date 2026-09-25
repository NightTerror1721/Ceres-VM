// F0.4 spike: a 4-operator FM voice (plan/v2 SPEC §9, level A2), to validate the cost per sample and determinism.
//
//   fm [--wav <file>] [--algorithm <0-7>] [--seconds <s>]
//
// Writes one note (A4, 2 s by default) as a 48 kHz 16-bit mono WAV, then times 8 voices x 4 operators, cycling the
// 8 algorithms, and prints the cost per voice-sample and an FNV-1a hash of everything it rendered. The synthesis is
// integer only (a 32-bit phase, a sine table, an attenuation table and a linear-in-dB envelope), the way the A2
// synthesizer has to be to give the same samples on every host. The two tables are built with std::sin and
// std::exp2 at start-up; comparing the hash between compilers checks that this rounds the same everywhere.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	constexpr int SampleRate = 48000;
	constexpr int SineBits = 12;                       // 4096 entries over a full cycle
	constexpr int SineSize = 1 << SineBits;
	constexpr int OutputBits = 14;                     // an operator's output is a signed Q14: -16384..16384
	constexpr int AttenuationSteps = 1024;             // 0 .. 1023 in 3/32 dB steps: 0 .. about 96 dB
	constexpr std::int32_t Silent = AttenuationSteps - 1;

	std::array<std::int16_t, SineSize> sineTable;
	std::array<std::int32_t, AttenuationSteps> gainTable;   // Q14 linear gain for each attenuation step

	void buildTables()
	{
		const double pi = 3.14159265358979323846;
		for (int i = 0; i < SineSize; ++i)
			sineTable[i] = static_cast<std::int16_t>(std::lround(std::sin(2.0 * pi * (i + 0.5) / SineSize) * (1 << OutputBits)));
		for (int i = 0; i < AttenuationSteps; ++i)
			gainTable[i] = static_cast<std::int32_t>(std::lround(std::exp2(-(i * 3.0 / 32.0) / 6.0205999132796239) * (1 << OutputBits)));
		gainTable[Silent] = 0;
	}

	// The twelve semitones of octave 4 in millihertz; other octaves shift. No floating point at note time.
	constexpr std::array<std::uint64_t, 12> Octave4 = { 261626, 277183, 293665, 311127, 329628, 349228,
		369994, 391995, 415305, 440000, 466164, 493883 };

	std::uint32_t phaseStep(int midiNote)
	{
		const int octave = midiNote / 12 - 1;
		std::uint64_t milliHz = Octave4[static_cast<std::size_t>(midiNote % 12)];
		milliHz = octave >= 4 ? milliHz << (octave - 4) : milliHz >> (4 - octave);
		return static_cast<std::uint32_t>((milliHz << 32) / (static_cast<std::uint64_t>(SampleRate) * 1000));
	}

	struct Envelope
	{
		enum class Stage : std::uint8_t { Attack, Decay, Sustain, Release, Off };
		// Rates in attenuation steps per sample, Q8; sustain level in attenuation steps.
		std::int32_t attack = 0, decay = 0, sustainLevel = 0, release = 0;
		std::int32_t level = Silent << 8;
		Stage stage = Stage::Off;

		void keyOn() { stage = Stage::Attack; }
		void keyOff() { if (stage != Stage::Off) stage = Stage::Release; }

		std::int32_t step()
		{
			switch (stage)
			{
				case Stage::Attack:
					// Exponential in the attenuation domain, as FM chips do: fast at first, slowing near full level.
					level -= ((level >> 4) * attack >> 8) + attack;
					if (level <= 0) { level = 0; stage = Stage::Decay; }
					break;
				case Stage::Decay:
					level += decay;
					if (level >= sustainLevel << 8) { level = sustainLevel << 8; stage = Stage::Sustain; }
					break;
				case Stage::Sustain:
					break;
				case Stage::Release:
					level += release;
					if (level >= Silent << 8) { level = Silent << 8; stage = Stage::Off; }
					break;
				case Stage::Off:
					break;
			}
			return level >> 8;
		}
	};

	struct Operator
	{
		std::uint32_t phase = 0;
		std::uint32_t step = 0;
		std::int32_t totalLevel = 0;   // attenuation steps added to the envelope's
		std::int32_t multiple = 2;     // frequency multiple in halves: 1 = x0.5, 2 = x1 ... 30 = x15
		Envelope envelope;

		// One sample: modulation is a Q14 value added to the phase, where 16384 is two whole cycles (4 pi).
		std::int32_t run(std::int32_t modulation)
		{
			const std::int32_t attenuation = std::min(Silent, envelope.step() + totalLevel);
			const std::uint32_t at = phase + (static_cast<std::uint32_t>(modulation) << 19);
			phase += step;
			return sineTable[at >> (32 - SineBits)] * gainTable[static_cast<std::size_t>(attenuation)] >> OutputBits;
		}
	};

	struct Voice
	{
		std::array<Operator, 4> op;
		int algorithm = 0;
		int feedback = 5;              // 0 = none, 7 = most
		std::int32_t feedbackHistory[2] = { 0, 0 };

		void noteOn(int midiNote)
		{
			const std::uint32_t base = phaseStep(midiNote);
			for (Operator& o : op)
			{
				o.step = static_cast<std::uint32_t>((static_cast<std::uint64_t>(base) * static_cast<std::uint64_t>(o.multiple)) >> 1);
				o.phase = 0;
				o.envelope.keyOn();
			}
		}

		void noteOff()
		{
			for (Operator& o : op)
				o.envelope.keyOff();
		}

		// The eight classic 4-operator algorithms (operator 1 carries the feedback; "a>b" means a modulates b).
		std::int32_t sample()
		{
			const std::int32_t fb = feedback == 0 ? 0 : (feedbackHistory[0] + feedbackHistory[1]) >> (10 - feedback);
			const std::int32_t o1 = op[0].run(fb);
			feedbackHistory[1] = feedbackHistory[0];
			feedbackHistory[0] = o1;
			std::int32_t o2, o3, o4;
			switch (algorithm)
			{
				case 0: o2 = op[1].run(o1); o3 = op[2].run(o2); return op[3].run(o3);                  // 1>2>3>4
				case 1: o2 = op[1].run(0); o3 = op[2].run(o1 + o2); return op[3].run(o3);             // (1+2)>3>4
				case 2: o2 = op[1].run(0); o3 = op[2].run(o2); return op[3].run(o1 + o3);             // (1+(2>3))>4
				case 3: o2 = op[1].run(o1); o3 = op[2].run(0); return op[3].run(o2 + o3);             // ((1>2)+3)>4
				case 4: o2 = op[1].run(o1); o3 = op[2].run(0); o4 = op[3].run(o3); return (o2 + o4) >> 1;   // (1>2)+(3>4)
				case 5: o2 = op[1].run(o1); o3 = op[2].run(o1); o4 = op[3].run(o1); return (o2 + o3 + o4) / 3; // 1>(2,3,4)
				case 6: o2 = op[1].run(o1); o3 = op[2].run(0); o4 = op[3].run(0); return (o2 + o3 + o4) / 3;   // (1>2)+3+4
				default: o2 = op[1].run(0); o3 = op[2].run(0); o4 = op[3].run(0); return (o1 + o2 + o3 + o4) >> 2; // 1+2+3+4
			}
		}
	};

	// An electric-piano-like patch: modulators decay faster than carriers.
	Voice patch(int algorithm)
	{
		Voice v;
		v.algorithm = algorithm;
		const std::array<std::int32_t, 4> multiples = { 2, 28, 2, 2 };
		const std::array<std::int32_t, 4> levels = { 180, 220, 150, 0 };
		for (std::size_t i = 0; i < 4; ++i)
		{
			Operator& o = v.op[i];
			o.multiple = multiples[i];
			o.totalLevel = levels[i];
			o.envelope.attack = 2048;
			o.envelope.decay = i == 3 ? 3 : 12;
			o.envelope.sustainLevel = i == 3 ? 160 : 400;
			o.envelope.release = 40;
		}
		return v;
	}

	std::uint64_t fnv1a(std::uint64_t hash, std::int16_t sample)
	{
		for (int i = 0; i < 2; ++i)
		{
			hash ^= static_cast<std::uint8_t>(static_cast<std::uint16_t>(sample) >> (8 * i));
			hash *= 1099511628211ull;
		}
		return hash;
	}

	void writeWav(const char* path, const std::vector<std::int16_t>& samples)
	{
		std::FILE* f = std::fopen(path, "wb");
		if (!f) { std::perror(path); std::exit(2); }
		const auto u32 = [&](std::uint32_t v) { const unsigned char b[4] = { std::uint8_t(v), std::uint8_t(v >> 8), std::uint8_t(v >> 16), std::uint8_t(v >> 24) }; std::fwrite(b, 1, 4, f); };
		const auto u16 = [&](std::uint16_t v) { const unsigned char b[2] = { std::uint8_t(v), std::uint8_t(v >> 8) }; std::fwrite(b, 1, 2, f); };
		const std::uint32_t bytes = static_cast<std::uint32_t>(samples.size() * 2);
		std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
		u32(16); u16(1); u16(1); u32(SampleRate); u32(SampleRate * 2); u16(2); u16(16);
		std::fwrite("data", 1, 4, f); u32(bytes);
		for (const std::int16_t s : samples) u16(static_cast<std::uint16_t>(s));
		std::fclose(f);
	}

	std::int16_t clip(std::int32_t v) { return static_cast<std::int16_t>(std::clamp(v, -32768, 32767)); }
}

int main(int argc, char** argv)
{
	const char* wav = "fm_note.wav";
	int algorithm = 4;
	double seconds = 10.0;
	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		if (a == "--wav" && i + 1 < argc) wav = argv[++i];
		else if (a == "--algorithm" && i + 1 < argc) algorithm = std::atoi(argv[++i]) & 7;
		else if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
		else { std::fprintf(stderr, "usage: fm [--wav <file>] [--algorithm <0-7>] [--seconds <s>]\n"); return 2; }
	}
	buildTables();

	// One note: 1.5 s held, 0.5 s released.
	{
		Voice v = patch(algorithm);
		std::vector<std::int16_t> samples;
		v.noteOn(69);
		std::uint64_t hash = 14695981039346656037ull;
		for (int i = 0; i < SampleRate * 2; ++i)
		{
			if (i == SampleRate * 3 / 2)
				v.noteOff();
			const std::int16_t s = clip(v.sample() * 2);
			samples.push_back(s);
			hash = fnv1a(hash, s);
		}
		writeWav(wav, samples);
		std::printf("FM note A4, algorithm %d: %s, %zu samples, hash %016llx\n", algorithm, wav, samples.size(), static_cast<unsigned long long>(hash));
	}

	// Cost: 8 voices, algorithms 0..7, a chord that changes every half second.
	{
		std::array<Voice, 8> voices;
		for (int i = 0; i < 8; ++i)
			voices[static_cast<std::size_t>(i)] = patch(i);
		const long long total = static_cast<long long>(seconds * SampleRate);
		std::uint64_t hash = 14695981039346656037ull;
		const auto start = std::chrono::steady_clock::now();
		for (long long n = 0; n < total; ++n)
		{
			if (n % (SampleRate / 2) == 0)
				for (int i = 0; i < 8; ++i)
					voices[static_cast<std::size_t>(i)].noteOn(48 + ((static_cast<int>(n / (SampleRate / 2)) * 5 + i * 4) % 36));
			std::int32_t mix = 0;
			for (Voice& v : voices)
				mix += v.sample();
			hash = fnv1a(hash, clip(mix >> 1));
		}
		const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		const double voiceSamples = static_cast<double>(total) * 8;
		std::printf("FM 8 voices x 4 operators, %.1f s of audio in %.3f s: %.1f ns per voice-sample, %.2f%% of one core at 48 kHz, hash %016llx\n",
			seconds, elapsed, elapsed * 1e9 / voiceSamples, elapsed / seconds * 100.0, static_cast<unsigned long long>(hash));
	}
}
