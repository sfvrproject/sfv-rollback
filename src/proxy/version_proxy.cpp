// version.dll proxy. StreetFighterV.exe imports three functions from
// VERSION.dll, but on Windows 11 the System32 copy is already loaded when the
// exe's imports are bound, so this proxy is never used there. Kept for older
// Windows; xinput_proxy.cpp is the loader that works.

#include "proxy/load_mod.h"

#define FORWARD(name) __pragma(comment(linker, "/export:" #name "=C:\\Windows\\System32\\version." #name))

FORWARD(GetFileVersionInfoA)
FORWARD(GetFileVersionInfoByHandle)
FORWARD(GetFileVersionInfoExA)
FORWARD(GetFileVersionInfoExW)
FORWARD(GetFileVersionInfoSizeA)
FORWARD(GetFileVersionInfoSizeExA)
FORWARD(GetFileVersionInfoSizeExW)
FORWARD(GetFileVersionInfoSizeW)
FORWARD(GetFileVersionInfoW)
FORWARD(VerFindFileA)
FORWARD(VerFindFileW)
FORWARD(VerInstallFileA)
FORWARD(VerInstallFileW)
FORWARD(VerLanguageNameA)
FORWARD(VerLanguageNameW)
FORWARD(VerQueryValueA)
FORWARD(VerQueryValueW)
