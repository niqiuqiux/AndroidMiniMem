#pragma once

#include <filesystem>
#include <system_error>

namespace utils {

// Install a freshly-written temp file over `targetPath` as atomically as the
// platform allows, WITHOUT ever leaving the caller with neither file.
//
// Fast path: a direct rename (atomic on NTFS when both paths sit on the same
// volume, which is guaranteed when the temp is a sibling of the target). If
// that fails — e.g. the target is held open by another process (AV scanner,
// sync client, a second app instance) — the existing target is first moved
// aside to a `.bak` sibling, the temp is installed, and on any failure the
// original is restored from the backup. The temp is removed on failure so a
// stale `.tmp` is never left behind.
//
// This replaces the earlier "remove(target) then rename(tmp, target)" pattern,
// which could delete the only good copy if the second rename also failed
// (losing e.g. all encrypted API keys or the whole session index).
//
// Returns true only when `targetPath` now holds the new content.
inline bool installTempFile(const std::filesystem::path& tmpPath,
                            const std::filesystem::path& targetPath) {
    std::error_code ec;
    std::filesystem::rename(tmpPath, targetPath, ec);
    if (!ec) {
        return true;
    }

    // Direct replace failed. Preserve the current target by moving it aside
    // before retrying, so a second failure can't destroy the only good copy.
    std::filesystem::path bakPath = targetPath;
    bakPath += ".bak";

    std::error_code bakEc;
    std::filesystem::remove(bakPath, bakEc); // clear any stale backup

    bool movedAside = false;
    bakEc.clear();
    if (std::filesystem::exists(targetPath, bakEc)) {
        bakEc.clear();
        std::filesystem::rename(targetPath, bakPath, bakEc);
        movedAside = !bakEc;
    }

    ec.clear();
    std::filesystem::rename(tmpPath, targetPath, ec);
    if (ec) {
        // Could not install the new file. Restore the original if we moved it
        // aside, and drop the temp. The previous target survives either way.
        if (movedAside) {
            std::error_code restoreEc;
            std::filesystem::rename(bakPath, targetPath, restoreEc);
        }
        std::error_code rmEc;
        std::filesystem::remove(tmpPath, rmEc);
        return false;
    }

    if (movedAside) {
        std::error_code rmEc;
        std::filesystem::remove(bakPath, rmEc); // success — drop the backup
    }
    return true;
}

} // namespace utils
