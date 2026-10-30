#pragma once
// Opening files named in UTF-8. The mod keeps paths as UTF-8 strings; the C
// library's fopen reads a char path in the system code page, so a game folder
// or user folder outside it (Chinese, Cyrillic, ...) could not be opened.

#include <cstdio>
#include <string>

namespace sfv {

// fopen for a UTF-8 path (on Windows through _wfopen). A path that is not
// valid UTF-8 is opened as it is, in the system code page, as command-line
// arguments come to the tools.
FILE* OpenFile(const std::string& path, const char* mode);

// Renames a file named in UTF-8 (the same rule), replacing the target.
bool RenameFile(const std::string& from, const std::string& to);

} // namespace sfv
