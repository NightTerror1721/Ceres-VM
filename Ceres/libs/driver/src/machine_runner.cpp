#include "machine_runner.h"

#include <ceres/driver/driver.h>
#include <ceres/devices/devices.h>
#include <ceres/devices/storage_devices.h>
#include <ceres/vm/ceresvm.h>

#include <format>
#include <map>
#include <memory>
#include <thread>

namespace ceres::driver
{
	using namespace devices;
	using namespace fmt;
	using namespace vm;

	namespace
	{
		void printProfile(CeresVM& vm, const DebugInfo& info, std::ostream& err)
		{
			struct HotLine { u32 fileId; u32 line; u64 count; };
			std::map<std::pair<u32, u32>, u64> lines;
			u64 total = 0;
			const auto counts = vm.engine().executionCounts();
			for (const auto& entry : info.lines())
			{
				if (entry.address < vm.engine().textStart())
					continue;
				const usize index = (entry.address - vm.engine().textStart()) / Instruction::Size;
				if (index < counts.size())
				{
					lines[{entry.expansionFileId, entry.expansionLine}] += counts[index];
					total += counts[index];
				}
			}

			std::vector<HotLine> hot;
			for (const auto& [location, count] : lines)
				if (count != 0)
					hot.push_back({location.first, location.second, count});
			std::ranges::sort(hot, {}, &HotLine::count);
			std::ranges::reverse(hot);

			err << std::format("\n{} instructions executed\n\n     count      share  line\n", total);
			for (const auto& row : hot)
				err << std::format("{:>10}  {:>8.2f}%  {}:{}\n", row.count,
				total ? 100.0 * row.count / total : 0.0, info.fileName(row.fileId), row.line);
		}
	}

	int runMachine(const Program& program, usize memorySize, const DebugInfo* profileInfo,
		const std::filesystem::path& diskImage, HostServices services)
	{
		CeresVM vm{memorySize};
		SystemControlDevice control{[&vm] { vm.shutdown(); }, [&vm] { vm.shutdown(); }};
		auto terminal = std::make_shared<TerminalDevice>();
		TimerDevice timer;
		DiskDevice disk;
		FramebufferDevice framebuffer;
		control.attachTo(vm.io());
		terminal->attachTo(vm.io());
		timer.attachTo(vm.io());
		terminal->setOutputSink([out = services.output](u8 byte) { out->put(static_cast<char>(byte)); out->flush(); });
		framebuffer.setPresentSink([out = services.output](std::string_view frame) { *out << frame; out->flush(); });
		if (!diskImage.empty() && !disk.open(diskImage))
		{
			*services.diagnostics << "Failed to open disk image: " << diskImage.string() << '\n';
			return 1;
		}
		disk.attachTo(vm.io());
		framebuffer.attachTo(vm.io());

		if (services.input != nullptr)
		{
			// A blocked console read cannot be cancelled portably. Shared ownership prevents a stale
			// reader from touching a destroyed device; detachFrom clears its VM connection on return.
			std::thread([input = services.input, terminal]
			{
				char c;
				while (input->get(c))
					terminal->pushInput(c);
			}).detach();
		}

		if (auto loaded = vm.loadProgram(program); !loaded)
		{
			terminal->detachFrom(vm.io());
			*services.diagnostics << "Failed to load program: " << loaded.error() << '\n';
			return 1;
		}
		if (profileInfo)
			vm.engine().enableProfiling();
		if (auto result = vm.run(); !result)
		{
			terminal->detachFrom(vm.io());
			*services.diagnostics << "Failed to run program: " << result.error() << '\n';
			return 1;
		}
		if (profileInfo)
			printProfile(vm, *profileInfo, *services.diagnostics);
		terminal->detachFrom(vm.io());
		return 0;
	}
}
