#include "core/file_open.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdio>

namespace sfv {

namespace {

#ifdef _WIN32
// The UTF-8 text as UTF-16; false when it is not valid UTF-8.
bool WideFromUtf8(const std::string& text, std::wstring* out)
{
    out->clear();
    if (text.empty()) return true;
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return false;
    out->resize(static_cast<size_t>(n));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out->data(), n);
    return true;
}
#endif

} // namespace

FILE* OpenFile(const std::string& path, const char* mode)
{
#ifdef _WIN32
    std::wstring wide, wideMode;
    if (WideFromUtf8(path, &wide)) {
        for (const char* m = mode; *m; m++) wideMode += static_cast<wchar_t>(*m);
        return _wfopen(wide.c_str(), wideMode.c_str());
    }
#endif
    return std::fopen(path.c_str(), mode);
}

bool RenameFile(const std::string& from, const std::string& to)
{
#ifdef _WIN32
    std::wstring a, b;
    if (WideFromUtf8(from, &a) && WideFromUtf8(to, &b)) return MoveFileExW(a.c_str(), b.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

} // namespace sfv
