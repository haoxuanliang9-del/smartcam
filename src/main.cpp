#include "app/main_service.h"
#include <spdlog/spdlog.h>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    std::string config_path = "/etc/smartcam/config.yaml";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: smartcam [options]\n"
                      << "Options:\n"
                      << "  -c, --config <path>  Configuration file path\n"
                      << "  -h, --help           Show this help\n";
            return 0;
        }
    }

    smartcam::MainService service;
    if (!service.init(config_path)) {
        SPDLOG_ERROR("Failed to initialize service");
        return 1;
    }

    service.run();
    return 0;
}
