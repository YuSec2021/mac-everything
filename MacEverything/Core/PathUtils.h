#pragma once
#include <string>
#include <cstdlib>
#include <sys/utsname.h>
#include <sys/types.h>
#include <sys/sysctl.h>

/// Pure C++ path/OS utilities — no ObjC/Foundation dependency.
namespace PathUtils {

/// Get "~/Library/Caches/com.maceverything.app" expanded.
inline std::string getDefaultCachePath() {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/Library/Caches/com.maceverything.app";
}

/// Get "~/Library/Logs/MacEverything" expanded.
inline std::string getDefaultLogPath() {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/Library/Logs/MacEverything";
}

/// Get macOS version string via sysctl (e.g. "15.3.0").
/// Uses ProductVersion from kern.osproductversion (macOS 10.13.4+).
inline std::string getOSVersionString() {
    // Try kern.osproductversion first (available macOS 10.13.4+)
    char buf[64] = {};
    size_t len = sizeof(buf);
    if (sysctlbyname("kern.osproductversion", buf, &len, nullptr, 0) == 0) {
        return std::string(buf);
    }
    // Fallback to uname
    struct utsname info;
    if (uname(&info) == 0) {
        return std::string(info.release);
    }
    return "unknown";
}

/// Returns true if `path` is macOS's APFS Data volume mountpoint
/// (`/System/Volumes/Data`) or any path beneath it.
/// On macOS Catalina+, the Data volume shares `st_dev` with `/` while its
/// user-visible directories are already unified into `/` via APFS firmlinks,
/// so scanning or watching `/System/Volumes/Data` duplicates records alongside
/// `/Users`, `/Applications`, `/Library`, etc.
inline bool isAPFSDataVolumePath(const std::string& path) {
    static constexpr const char kDataVolume[] = "/System/Volumes/Data";
    static constexpr size_t kLen = sizeof(kDataVolume) - 1;
    if (path.size() < kLen) return false;
    if (path.compare(0, kLen, kDataVolume) != 0) return false;
    return path.size() == kLen || path[kLen] == '/';
}

} // namespace PathUtils
