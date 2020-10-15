/*!
 * @file main.cpp
 * @brief Main Function for the WBC Controller
 *
 * The main function parses command line arguments and starts the appropriate
 * driver.
 */

#include <main_helper.h>
#include "MIT_Controller.hpp"
#include <memory>

int main(int argc, char **argv) {
    auto mitController = std::make_unique<MIT_Controller>();
    main_helper(argc, argv, mitController.get());
    return 0;
}
