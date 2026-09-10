#include <ceres/driver/driver.h>
#include <iostream>

int main(int argc, char** argv)
{
	return ceres::driver::runCommandLine(argc, argv, { &std::cin, &std::cout, &std::cerr });
}
