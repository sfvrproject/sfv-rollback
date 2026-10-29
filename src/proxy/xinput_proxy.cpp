// xinput1_3.dll proxy, the mod's loader. StreetFighterV.exe imports
// XInputGetState and XInputSetState by ordinal (2 and 3) from XINPUT1_3.dll,
// which is not a KnownDLL and not preloaded by system DLLs, so a copy next to
// the exe is the one the loader binds. Every export forwards to System32 at its
// original ordinal, including the unnamed ordinals 100-103.

#include "proxy/load_mod.h"

#define FORWARD(name, ord) \
    __pragma(comment(linker, "/export:" #name "=C:\\Windows\\System32\\xinput1_3." #name ",@" #ord))
#define FORWARD_ORDINAL(ord) \
    __pragma(comment(linker, "/export:Ordinal" #ord "=C:\\Windows\\System32\\xinput1_3.#" #ord ",@" #ord ",NONAME"))

FORWARD(XInputGetState, 2)
FORWARD(XInputSetState, 3)
FORWARD(XInputGetCapabilities, 4)
FORWARD(XInputEnable, 5)
FORWARD(XInputGetDSoundAudioDeviceGuids, 6)
FORWARD(XInputGetBatteryInformation, 7)
FORWARD(XInputGetKeystroke, 8)
FORWARD_ORDINAL(100)
FORWARD_ORDINAL(101)
FORWARD_ORDINAL(102)
FORWARD_ORDINAL(103)
