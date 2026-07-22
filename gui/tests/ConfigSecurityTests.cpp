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
               << "cardKey=saved-card-value\n"
               << "kernelVersion=6\n";
    }

    auto& config = ConfigManager::getInstance();
    check(config.loadConfig(path), "configuration should load");
    check(config.getString("cardKey") == "saved-card-value",
          "saved card should be loaded");
    config.setString("cardKey", "updated-card-value");
    check(config.saveConfig(path), "configuration should save");

    check(config.loadConfig(path), "saved configuration should reload");
    check(config.getString("cardKey") == "updated-card-value",
          "updated card should survive a save and reload");

    std::ifstream input(path);
    std::ostringstream contents;
    contents << input.rdbuf();
    const std::string text = contents.str();
    check(text.find("cardKey=updated-card-value") != std::string::npos,
          "saved configuration should contain the card value");
    check(text.find("host=127.0.0.1") != std::string::npos &&
              text.find("kernelVersion=6") != std::string::npos,
          "saving the card should preserve ordinary settings");

    (void)std::remove(path.c_str());
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "Config persistence tests passed\n";
    return 0;
}
