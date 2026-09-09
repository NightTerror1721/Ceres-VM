// Test runner. Optional argument filters by suite name: encoding, vm, pipeline.
#include "framework.h"
#include <cstdio>
#include <string_view>

int main(int argc, char** argv)
{
	const std::string_view filter = argc > 1 ? std::string_view(argv[1]) : std::string_view{};

	if (!filter.empty())
		std::printf("Running suite '%.*s'\n\n", static_cast<int>(filter.size()), filter.data());
	else
		std::printf("Running all suites\n\n");

	return ceres::testing::Registry::instance().runAll(filter);
}
