#pragma once

#include <windows.h>
#include <shellapi.h>
#include <string>
#include <utility>
#include <vector>

namespace shell {

// Own the shell drop handle so every path through the handler releases it.
class DroppedFiles {
    HDROP handle;

public:
    explicit DroppedFiles(HDROP drop) : handle(drop) {}
    ~DroppedFiles() { DragFinish(handle); }
    DroppedFiles(const DroppedFiles&) = delete;
    DroppedFiles& operator=(const DroppedFiles&) = delete;

    std::vector<std::wstring> paths() const {
        std::vector<std::wstring> files;
        const UINT count = DragQueryFileW(handle, 0xffffffffu, nullptr, 0);
        files.reserve(count);
        for (UINT i = 0; i < count; ++i) {
            const UINT length = DragQueryFileW(handle, i, nullptr, 0);
            if (!length) continue;
            std::wstring path(length + 1, L'\0');
            if (DragQueryFileW(handle, i, path.data(), length + 1) != length) continue;
            path.resize(length);
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
                files.push_back(std::move(path));
        }
        return files;
    }
};
}  // namespace shell
