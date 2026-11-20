#include "driver.hpp"

#include <iostream>

int main(int argc, char **argv)
{
    return cephyr::cephyr_cli_main(argc, argv, std::cin, std::cout, std::cerr);
}
