#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include "aisf_platform/utils/utils.h"

namespace AisfPlat::FileUtils {
std::string RealPath(const std::string& path)
{
    SafeString absPath(__FUNCTION__, __LINE__, PATH_MAX);
    if (realpath(path.c_str(), absPath.Get()) == nullptr) {
        return "";
    }

    return absPath.Data();
}

bool WriteFile(const std::string& path, const char* content, size_t size, const char* openMode, int permMode)
{
    auto handle = fopen(RealPath(path).c_str(), openMode);
    CHECK_TRUE_LOG_ERROR_RETURN(handle == nullptr, false, "file %s open file, err: %s", path.c_str(), strerror(errno));

    if (content != nullptr) {
        auto ret = fwrite(content, 1, size, handle);
        CHECK_TRUE_LOG_ERROR(ret != size, "write %s expect:%u, actual: %u", path.c_str(), size, ret);
    }

    (void)fclose(handle);
    (void)chmod(path.c_str(), permMode);

    return true;
}

bool WriteTextFile(const std::string& path, const char* content, size_t size, int mode)
{
    return WriteFile(path, content, size, "w", mode);
}

}  // namespace AisfPlat::FileUtils