# The application-facing operations shared by command-line, graphical and automated hosts.
ceres_add_library(driver DEPENDS core vm devices asm debug)

ceres_add_tests(driver)
