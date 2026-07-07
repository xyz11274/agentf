#pragma once

namespace AisfPlat::FileUtils {
// 使用bypass接口，将指定路径转换为绝对路径
std::string RealPath(const std::string& path);

// 写数据到文件中，默认文件权限为640
bool WriteFile(const std::string& path, const char* content, size_t size, const char* openMode,
               int permMode = DEFAULT_FILE_MODE);

// 写文本文件，默认文件权限为640
bool WriteTextFile(const std::string& path, const char* content, size_t size, int mode = DEFAULT_FILE_MODE);
}  // namespace AisfPlat::FileUtils