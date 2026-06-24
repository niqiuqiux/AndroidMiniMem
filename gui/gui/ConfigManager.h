#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <map>

class ConfigManager {
public:
    static ConfigManager& getInstance() {
        static ConfigManager instance;
        return instance;
    }

    // 加载配置
    bool loadConfig(const std::string& filename = "config.ini");
    
    // 保存配置
    bool saveConfig(const std::string& filename = "config.ini");
    
    // 获取配置值
    std::string getString(const std::string& key, const std::string& defaultValue = "") const;
    int getInt(const std::string& key, int defaultValue = 0) const;
    char getChar(const std::string& key, char defaultValue = '\0') const;
    
    // 设置配置值
    void setString(const std::string& key, const std::string& value);
    void setInt(const std::string& key, int value);
    void setChar(const std::string& key, char value);

private:
    ConfigManager() = default;
    ~ConfigManager() = default;
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;
    
    std::string trim(const std::string& str) const;
    void parseLine(const std::string& line);
    
    std::map<std::string, std::string> configMap;
};

