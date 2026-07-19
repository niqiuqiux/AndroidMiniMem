#include "../gui/ConfigManager.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    const std::string path = "minimem_config_security_test.ini";
    {
        std::ofstream output(path, std::ios::trunc);
        output << "host=127.0.0.1\n"
               << "cardKey=legacy-plaintext-secret\n"
               << "kernelVersion=6\n";
    }

    auto& config = ConfigManager::getInstance();
    check(config.loadConfig(path), "legacy configuration should load");
    check(config.remove("cardKey"),
          "legacy plaintext card entry should be removed");
    check(!config.remove("cardKey"),
          "removing a missing card entry should be idempotent");
    check(config.saveConfig(path), "sanitized configuration should save");

    std::ifstream input(path);
    std::ostringstream contents;
    contents << input.rdbuf();
    const std::string text = contents.str();
    check(text.find("cardKey") == std::string::npos &&
              text.find("legacy-plaintext-secret") == std::string::npos,
          "saved configuration must not contain the card key or its value");
    check(text.find("host=127.0.0.1") != std::string::npos &&
              text.find("kernelVersion=6") != std::string::npos,
          "sanitizing the card entry should preserve ordinary settings");

    (void)std::remove(path.c_str());
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "Config security tests passed\n";
    return 0;
}
