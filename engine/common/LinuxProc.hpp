#pragma once

#include "ProcessMap.hpp"
#include <asm-generic/mman-common.h>
#include <dirent.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <sys/types.h>
#include <signal.h>


// 去除多余空格的辅助函数
static std::string trimSpaces(const std::string& input) {
    std::string result;
    result.reserve(input.size()); // 预留空间，避免多次扩容

    bool inSpace = false;
    for (char ch : input) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!inSpace) {
                // 第一次遇到空格，追加一个
                result.push_back(' ');
                inSpace = true;
            }
        } else {
            result.push_back(ch);
            inSpace = false;
        }
    }

    // 去掉首尾的空格
    if (!result.empty() && result.front() == ' ')
        result.erase(result.begin());
    if (!result.empty() && result.back() == ' ')
        result.pop_back();

    return result;
}



class LinuxProc {
public:
  static std::vector<std::pair<int, std::string>> GetProcessPidList() {
    DIR *dir = NULL;
    struct dirent *ptr = NULL;
    std::vector<std::pair<int, std::string>> vPID;

    dir = opendir("/proc");
    if (dir) {
      while ((ptr = readdir(dir)) !=
             NULL) { // 循环读取路径下的每一个文件/文件夹
        // 如果读取到的是"."或者".."则跳过，读取到的不是文件夹名字也跳过
        if ((strcmp(ptr->d_name, ".") == 0) ||
            (strcmp(ptr->d_name, "..") == 0)) {
          continue;
        } else if (ptr->d_type != DT_DIR) {
          continue;
        } else if (strspn(ptr->d_name, "1234567890") != strlen(ptr->d_name)) {
          continue;
        }

        int pid = atoi(ptr->d_name);
        std::string name = GetProcessCmdline(pid);
        if (name.empty()) {
          name = GetProcessName(pid);
        }
        //读取链接
        std::string link = "/proc/" + std::to_string(pid) + "/exe";
        char buf[1024] = {0};
        readlink(link.c_str(), buf, sizeof(buf)-1);
        link = buf;
        //printf("link %s\n",link.c_str());
        if (link == "/system/bin/app_process64" || link == "/system/bin/app_process32") {
          //app 
          std::string app = link.substr(link.find_last_of('/') + 1);
          name =  app + " " + name;
           vPID.push_back(std::make_pair(pid, name));
          continue;
        
        } else if (link.starts_with("/system/bin/")
        || link.empty()
         || link.starts_with("/system_ext/bin/")
        ||link.starts_with("/vendor/bin/") 
           ||link.starts_with("/apex/com.") ) {
          //ignore system process
          continue;
        }else {
          //other process
          name = "elf " + name;
          vPID.push_back(std::make_pair(pid, name));
        }

       
      }
      closedir(dir);
      return vPID;
    }
    return std::vector<std::pair<int, std::string>>();
  }

  static std::vector<ProcessMap> GetProcessMaps(int pid) {
    std::vector<ProcessMap> maps;
    
    std::string mapsPath = "/proc/" + std::to_string(pid) + "/maps";
    FILE* fp = fopen(mapsPath.c_str(), "r");
    if (!fp) return maps;

    char line[512] = {0};
    while (fgets(line, sizeof(line), fp)) {
      ProcessMap map;
      map.pid = pid;
      map.allinfo = trimSpaces(line);
      char perms[5] = {0}, dev[11] = {0}, pathname[512] = {0};
      // parse a line in maps file
      // (format) startAddress-endAddress perms offset dev inode pathname
      sscanf(line, "%llx-%llx %s %llx %s %lu %s",
             (unsigned long long*)&map.startAddress, 
             (unsigned long long*)&map.endAddress,
             perms, &map.offset, dev, &map.inode, pathname);

      map.length = map.endAddress - map.startAddress;
      map.dev = dev;
      map.pathname = pathname;
      map.perms = std::string(perms);
      // Parse permissions
      map.readable = (perms[0] == 'r');
      map.writable = (perms[1] == 'w');
      map.executable = (perms[2] == 'x');
      map.is_private = (perms[3] == 'p');
      map.is_shared = (perms[3] == 's');
      map.flag = 0;
      map.type = DetermineMemoryType(pathname,map.perms);

      if (map.readable){
        map.flag |= 1;
        map.protection |= PROT_READ;
      } 
      if (map.writable){
        map.flag |= 2;
        map.protection |= PROT_WRITE;
      }
      if (map.executable){
        map.flag |= 4;
        map.protection |= PROT_EXEC;
      }
      if (map.is_private){
        map.flag |= 8;
      }
      if (map.is_shared){
        map.flag |= 16;
      }

      maps.push_back(map);
    }

    fclose(fp);
    return maps;
  }
   static uint64_t GetModBase(int pid,const std::string& name){
        uint64_t result=0;
        auto maps = GetProcessMaps(pid);
        
        for (const auto& map : maps) {
            if (!map.IsUnknown() && map.pathname.find(name) != std::string::npos) {
                result=map.startAddress;
                break;
            }
        }
        return result;
    }

  static std::string GetProcessName(int pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/comm";
    FILE *fp = fopen(path.c_str(), "r");
    if (fp) {
      char name[256] = {0};
      fgets(name, sizeof(name), fp);
      fclose(fp);
      // 去除 fgets 保留的换行符
      size_t len = strlen(name);
      if (len > 0 && name[len - 1] == '\n') {
          name[len - 1] = '\0';
      }
      return name;
    }
    return "";
  }

  static std::string GetProcessCmdline(int pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/cmdline";
    FILE *fp = fopen(path.c_str(), "r");
    if (fp) {
      char cmdline[200] = {0};
      fgets(cmdline, sizeof(cmdline), fp);
      fclose(fp);
      return cmdline;
    }
    return "";
  }
  static uint64_t GetProcessPhyMemSize(int pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/statm";
    FILE *fp = fopen(path.c_str(), "r");
    if (fp) {
      uint64_t rss = 0;
      fscanf(fp, "%*lu %lu", &rss);
      fclose(fp);
      return rss * getpagesize();
    }
    return 0;
  }
  static bool IsProcessRoot(int pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/status";
    FILE *fp = fopen(path.c_str(), "r");
    if (fp) {
      char line[256] = {0};
      while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "Uid:")) {
          int uid = 0;
          sscanf(line, "Uid: %d", &uid);
          fclose(fp);
          return uid == 0;
        }
      }
      fclose(fp);
    }
    return false;
  }
  static bool IsProcessAlive(int pid) {
    return kill(pid, 0) == 0;
  }
  static bool IsProcess64Bit(int pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/auxv";
    FILE *fp = fopen(path.c_str(), "r");
    if (fp) {
      uint64_t auxv[2] = {0};
      while (fread(auxv, sizeof(auxv), 1, fp)) {
        if (auxv[0] == 16) {
          fclose(fp);
          return auxv[1] == 2;
        }
      }
      fclose(fp);
    }
    return false;
  }
  static bool IsProcessDebuggable(int pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/status";
    FILE *fp = fopen(path.c_str(), "r");
    if (fp) {
      char line[256] = {0};
      while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "TracerPid:")) {
          int tracerPid = 0;
          sscanf(line, "TracerPid: %d", &tracerPid);
          fclose(fp);
          return tracerPid != 0;
        }
      }
      fclose(fp);
    }
    return false;
  }

};
