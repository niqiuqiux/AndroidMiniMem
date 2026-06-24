#pragma once

#include <arpa/inet.h>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/in6.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <fstream>
#include <functional>
#include <cstdio>


static bool resolve(const std::string &hostname,std::vector<std::string> &ips) {
  struct addrinfo hints, *result, *rp;
  int status;
  char ipstr[INET_ADDRSTRLEN];

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET; // IPv4
  hints.ai_socktype = SOCK_STREAM;

  status = getaddrinfo(hostname.c_str(), "80", &hints, &result);
  if (status != 0) {
    //std::cerr << "getaddrinfo error: " << gai_strerror(status) << std::endl;
    return false;
  }

  //std::cout << "IP addresses for " << hostname << ":" << std::endl;

  for (rp = result; rp != NULL; rp = rp->ai_next) {
    void *addr;
   
    memset(ipstr, 0, sizeof(ipstr));
    // 获取指向地址结构的指针
    if (rp->ai_family == AF_INET) { // IPv4
      struct sockaddr_in *ipv4 = (struct sockaddr_in *)rp->ai_addr;
      addr = &(ipv4->sin_addr);
     
    }
    //  else { // IPv6
    //   struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)rp->ai_addr;
    //   addr = &(ipv6->sin6_addr);
      
    // }

    // 将IP地址转换为字符串
    inet_ntop(rp->ai_family, addr, ipstr, sizeof(ipstr));
    ips.push_back(std::string(ipstr));
  }

  freeaddrinfo(result);
  return true;
}






static std::string GetLocalPath() {
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len < 0) return "";
    buf[len] = '\0';
//获取路径
	std::string path = std::string(buf);
	path = path.substr(0, path.find_last_of('/'));
	return path;

}


// 生成随机字符串（可打印字符）
static std::string GenerateRandomString(size_t length, bool printable_only = true) {
    std::string result;
    result.reserve(length);
    
    std::random_device rd;
    std::mt19937 gen(rd());
    
    if (printable_only) {
        // 只生成可打印ASCII字符 (32-126)
        std::uniform_int_distribution<int> dis(32, 126);
        for (size_t i = 0; i < length; ++i) {
            result.push_back(static_cast<char>(dis(gen)));
        }
    } else {
        // 生成所有字符 (0-255)
        std::uniform_int_distribution<int> dis(0, 255);
        for (size_t i = 0; i < length; ++i) {
            result.push_back(static_cast<char>(dis(gen)));
        }
    }
    
    return result;
}

// 生成随机数字字符串
static std::string GenerateRandomDigitString(size_t length) {
    std::string result;
    result.reserve(length);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis(0, 9);

    for (size_t i = 0; i < length; ++i) {
        result.push_back(static_cast<char>('0' + dis(gen)));
    }

    return result;
}




// 文件操作模式
enum class FileMode {
	OVERWRITE,    // 覆盖模式（默认）
	APPEND,       // 追加模式
	READ_ONLY,    // 只读模式（文件不存在则创建）
	TRUNCATE      // 截断模式（清空后写入）
};

// 文件类型
enum class FileType {
	TEXT,         // 文本模式（默认）
	BINARY        // 二进制模式
};

/**
 * @brief 读取或创建文件（增强版）
 * @param path 文件路径
 * @param generator 内容生成器函数
 * @param mode 文件操作模式
 * @param type 文件类型（文本/二进制）
 * @param read_full 是否读取全部内容（true），还是仅第一行（false）
 * @return 文件内容
 */
static std::string readfs(
	const std::string &path, 
	std::function<std::string()> generator,
	FileMode mode = FileMode::OVERWRITE,
	FileType type = FileType::TEXT,
	bool read_full = false
) {
	// 构建打开模式标志
	std::ios_base::openmode input_flags = std::ios_base::in;
	std::ios_base::openmode output_flags = std::ios_base::out;
	
	// 设置二进制模式
	if (type == FileType::BINARY) {
		input_flags |= std::ios_base::binary;
		output_flags |= std::ios_base::binary;
	}
	
	// 尝试读取文件
	std::ifstream ifs(path, input_flags);
	if (ifs.good()) {
		std::string content;
		
		if (read_full) {
			// 读取全部内容
			content.assign(
				(std::istreambuf_iterator<char>(ifs)),
				std::istreambuf_iterator<char>()
			);
		} else {
			// 只读取第一行
			std::getline(ifs, content);
		}
		
		ifs.close();
		
		// 如果是只读模式，直接返回
		if (mode == FileMode::READ_ONLY) {
			return content;
		}
		
		// 追加模式：返回已有内容，但稍后会追加
		if (mode == FileMode::APPEND) {
			std::string new_content = generator();
			output_flags |= std::ios_base::app;
			std::ofstream ofs(path, output_flags);
			if (ofs.is_open()) {
				ofs << new_content;
				ofs.close();
			}
			// 返回追加后的完整内容
			return content + new_content;
		}
		
		return content;
	}
	
	// 文件不存在，创建并写入
	std::string new_content = generator();
	
	// 设置输出模式
	switch (mode) {
		case FileMode::APPEND:
			output_flags |= std::ios_base::app;
			break;
		case FileMode::TRUNCATE:
			output_flags |= std::ios_base::trunc;
			break;
		case FileMode::READ_ONLY:
			// 只读模式但文件不存在，先创建
			output_flags |= std::ios_base::trunc;
			break;
		case FileMode::OVERWRITE:
		default:
			// 默认覆盖模式
			output_flags |= std::ios_base::trunc;
			break;
	}
	
	std::ofstream ofs(path, output_flags);
	if (ofs.is_open()) {
		ofs << new_content;
		ofs.close();
	}
	
	return new_content;
}


/**
 * @brief 写入文件
 * @param path 文件路径
 * @param content 要写入的内容
 * @param mode 文件操作模式
 * @param type 文件类型（文本/二进制）
 * @return 是否写入成功
 */
static bool writefs(
	const std::string &path,
	const std::string &content,
	FileMode mode = FileMode::OVERWRITE,
	FileType type = FileType::TEXT
) {
	std::ios_base::openmode flags = std::ios_base::out;
	
	// 设置二进制模式
	if (type == FileType::BINARY) {
		flags |= std::ios_base::binary;
	}
	
	// 设置写入模式
	switch (mode) {
		case FileMode::APPEND:
			flags |= std::ios_base::app;
			break;
		case FileMode::TRUNCATE:
		case FileMode::OVERWRITE:
			flags |= std::ios_base::trunc;
			break;
		case FileMode::READ_ONLY:
			// 只读模式不允许写入
			return false;
		default:
			flags |= std::ios_base::trunc;
			break;
	}
	
	std::ofstream ofs(path, flags);
	if (!ofs.is_open()) {
		return false;
	}
	
	ofs << content;
	ofs.close();
	
	return ofs.good() || !ofs.bad();
}

// ============ 写入便捷函数 ============

// 便捷函数：写入二进制文件
static bool writefs_binary(
	const std::string &path,
	const std::string &content,
	FileMode mode = FileMode::OVERWRITE
) {
	return writefs(path, content, mode, FileType::BINARY);
}

// 便捷函数：追加文本到文件
static bool writefs_append(
	const std::string &path,
	const std::string &content
) {
	return writefs(path, content, FileMode::APPEND, FileType::TEXT);
}

// 便捷函数：追加二进制到文件
static bool writefs_append_binary(
	const std::string &path,
	const std::string &content
) {
	return writefs(path, content, FileMode::APPEND, FileType::BINARY);
}

// ============ 读取便捷函数 ============

// 便捷函数：读取整个文件（文本模式）
static std::string readfs_text(const std::string &path) {
	std::ifstream ifs(path);
	if (!ifs.is_open()) {
		return "";
	}
	std::string content(
		(std::istreambuf_iterator<char>(ifs)),
		std::istreambuf_iterator<char>()
	);
	return content;
}

// 便捷函数：读取整个文件（二进制模式）
static std::string readfs_binary(const std::string &path) {
	std::ifstream ifs(path, std::ios::binary);
	if (!ifs.is_open()) {
		return "";
	}
	std::string content(
		(std::istreambuf_iterator<char>(ifs)),
		std::istreambuf_iterator<char>()
	);
	return content;
}

// 便捷函数：读取文件第一行
static std::string readfs_line(const std::string &path) {
	std::ifstream ifs(path);
	if (!ifs.is_open()) {
		return "";
	}
	std::string line;
	std::getline(ifs, line);
	return line;
}

// 便捷函数：读取所有行到vector
static std::vector<std::string> readfs_lines(const std::string &path) {
	std::vector<std::string> lines;
	std::ifstream ifs(path);
	if (!ifs.is_open()) {
		return lines;
	}
	std::string line;
	while (std::getline(ifs, line)) {
		lines.push_back(line);
	}
	return lines;
}

// ============ 高级读写函数（带默认值生成器）============

// 便捷函数：二进制模式读取或创建
static std::string readfs_binary_or_create(
	const std::string &path,
	std::function<std::string()> generator,
	FileMode mode = FileMode::OVERWRITE
) {
	return readfs(path, generator, mode, FileType::BINARY, true);
}

// 便捷函数：追加模式
static std::string readfs_append(
	const std::string &path,
	std::function<std::string()> generator
) {
	return readfs(path, generator, FileMode::APPEND, FileType::TEXT, true);
}

// 便捷函数：读取全文（如果不存在则创建）
static std::string readfs_full(
	const std::string &path,
	std::function<std::string()> generator = []() { return ""; }
) {
	return readfs(path, generator, FileMode::READ_ONLY, FileType::TEXT, true);
}

// ============ 文件操作工具函数 ============

// 检查文件是否存在
static bool file_exists(const std::string &path) {
	std::ifstream ifs(path);
	return ifs.good();
}

// 获取文件大小（字节）
static size_t file_size(const std::string &path) {
	std::ifstream ifs(path, std::ios::binary | std::ios::ate);
	if (!ifs.is_open()) {
		return 0;
	}
	return static_cast<size_t>(ifs.tellg());
}

// 删除文件
static bool file_delete(const std::string &path) {
	return std::remove(path.c_str()) == 0;
}

// 清空文件内容
static bool file_clear(const std::string &path) {
	std::ofstream ofs(path, std::ios::trunc);
	return ofs.good();
}


