#include "mod/ue4.h"

#include "mod/log.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <vector>

namespace sfv::mod::ue4 {

namespace {

constexpr int kChunkSize = 16384;
constexpr uintptr_t kKnownNamesRva = 0x3A75490;
constexpr size_t kEntryText = 0x10; // FNameEntry: int32 index, HashNext*, then the text
const void* const* const* g_chunks = nullptr;
std::atomic<bool> g_ready{false};
// GUObjectArray's objects: a TArray<UObject*> (data, int32 count, int32 max)
// in the image. It is reallocated as it grows, so its data pointer is read
// for every walk; an address found once goes stale.
constexpr uintptr_t kKnownObjectsRva = 0x3978730;
const uint8_t* g_objectsArray = nullptr; // the TArray in the image
constexpr int kMaxObjects = 8 * 1024 * 1024;
// Fallback when the image is not the known build: one array found by scanning
// (LocateObjectsFrom), valid until the engine reallocates it.
const uint8_t* g_items = nullptr;
size_t g_itemStride = 0;
constexpr int kItemChunk = 64 * 1024;

template <typename T> bool Read(const void* p, T* out)
{
    if (!IsReadable(p, sizeof(T))) return false;
    std::memcpy(out, p, sizeof(T));
    return true;
}

std::string EntryText(const void* entry)
{
    if (!entry || !IsReadable(entry, kEntryText + 2)) return "";
    int32_t index = 0;
    std::memcpy(&index, entry, 4);
    const auto* text = static_cast<const char*>(entry) + kEntryText;
    std::string out;
    if (index & 1) { // wide entry
        for (int i = 0; i < 256 && IsReadable(text + 2 * i, 2); i++) {
            wchar_t c;
            std::memcpy(&c, text + 2 * i, 2);
            if (!c) break;
            out += c < 128 ? static_cast<char>(c) : '?';
        }
        return out;
    }
    for (int i = 0; i < 256 && IsReadable(text + i, 1) && text[i]; i++) out += text[i];
    return out;
}

bool LooksLikeNames(const void* candidate)
{
    // `candidate` is in the image's own data, readable: test the value it
    // holds cheaply before following it.
    const void* const* chunks = *static_cast<const void* const* const*>(candidate);
    const auto v = reinterpret_cast<uintptr_t>(chunks);
    if (v < 0x10000 || v >= 0x800000000000ull || (v & 7)) return false;
    const void* const* chunk0 = nullptr;
    if (!Read(chunks, &chunk0) || !chunk0) return false;
    const void* e0 = nullptr;
    const void* e1 = nullptr;
    if (!Read(chunk0, &e0) || !Read(chunk0 + 1, &e1)) return false;
    return EntryText(e0) == "None" && EntryText(e1) == "ByteProperty";
}

const void* ItemObject(const uint8_t* items, size_t stride, int i)
{
    const void* object = nullptr;
    Read(items + i * stride, &object);
    return object;
}

// The object array as it is now: items, their stride and count.
bool CurrentItems(const uint8_t** items, size_t* stride, int* count)
{
    if (g_objectsArray) {
        const uint8_t* data = nullptr;
        int32_t num = 0;
        if (Read(g_objectsArray, &data) && Read(g_objectsArray + 8, &num) && data && num > 0 && num <= kMaxObjects) {
            *items = data;
            *stride = 8;
            *count = num;
            return true;
        }
    }
    if (!g_items) return false;
    *items = g_items;
    *stride = g_itemStride;
    *count = kMaxObjects; // unknown: walks until a long run of misses
    return true;
}

bool CheckItems(const uint8_t* items, size_t stride, int count)
{
    for (int i = 0; i < count; i++) {
        const void* object = ItemObject(items, stride, i);
        int32_t index = -1;
        if (!object || !Read(static_cast<const uint8_t*>(object) + 0xC, &index) || index != i) return false;
    }
    return true;
}

} // namespace

bool LocateObjectsFrom(const void* knownObject)
{
    int32_t index = -1;
    if (!knownObject || !Read(static_cast<const uint8_t*>(knownObject) + 0xC, &index) || index < 8) return false;
    const uint64_t value = reinterpret_cast<uint64_t>(knownObject);
    std::vector<uint8_t> buffer(1u << 20);
    uintptr_t addr = 0x10000;
    MEMORY_BASIC_INFORMATION mbi;
    while (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
        const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE) {
            for (uintptr_t chunk = base; chunk < end; chunk += buffer.size()) {
                const size_t n = static_cast<size_t>(std::min<uintptr_t>(buffer.size(), end - chunk));
                SIZE_T got = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(chunk), buffer.data(), n, &got)) continue;
                for (size_t i = 0; i + 8 <= got; i += 8) {
                    uint64_t v;
                    std::memcpy(&v, &buffer[i], 8);
                    if (v != value) continue;
                    for (size_t stride : {size_t(0x18), size_t(0x10), size_t(0x20), size_t(0x8)}) {
                        const uint8_t* items = reinterpret_cast<const uint8_t*>(chunk + i) - static_cast<size_t>(index % kItemChunk) * stride;
                        if (!CheckItems(items, stride, 8)) continue;
                        g_itemStride = stride;
                        if (index < kItemChunk) {
                            // Prefer the chunk table if something points at this array.
                            g_items = items;
                        }
                        else {
                            g_items = items; // only chunk N known; FindObject sees that chunk
                        }
                        Log("ue4: objects at %p (item stride 0x%zx), found from %s", static_cast<const void*>(items), stride,
                            Describe(knownObject).c_str());
                        return true;
                    }
                }
            }
        }
        if (end <= addr) break;
        addr = end;
    }
    Log("ue4: no object array points at %s", Describe(knownObject).c_str());
    return false;
}

bool Init(const GameImage& image)
{
    // Where this build keeps it (the image is verified at load); the scan
    // below is for anything else and takes seconds.
    if (LooksLikeNames(reinterpret_cast<const void*>(image.base + kKnownNamesRva))) {
        g_chunks = *reinterpret_cast<const void* const* const* const*>(image.base + kKnownNamesRva);
        g_objectsArray = reinterpret_cast<const uint8_t*>(image.base + kKnownObjectsRva);
        g_ready = true;
        return true;
    }
    // Walk the image's writable sections for the GNames pointer.
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        const uintptr_t start = image.base + sec->VirtualAddress;
        for (uintptr_t p = start; p + 8 <= start + sec->Misc.VirtualSize; p += 8) {
            if (!g_chunks && LooksLikeNames(reinterpret_cast<const void*>(p))) {
                g_chunks = *reinterpret_cast<const void* const* const* const*>(p);
                Log("ue4: names at rva 0x%llX", static_cast<unsigned long long>(p - image.base));
            }
        }
    }
    if (!g_chunks) Log("ue4: name table not found");
    g_ready = g_chunks != nullptr;
    return g_ready;
}

bool Ready() { return g_ready; }

std::string NameText(int32_t index)
{
    if (!g_chunks || index < 0) return "";
    const void* const* chunk = nullptr;
    if (!Read(g_chunks + index / kChunkSize, &chunk) || !chunk) return "";
    const void* entry = nullptr;
    if (!Read(chunk + index % kChunkSize, &entry)) return "";
    return EntryText(entry);
}

std::string ObjectName(const void* object)
{
    int32_t name[2] = {};
    if (!object || !Read(static_cast<const uint8_t*>(object) + 0x18, &name)) return "?";
    std::string s = NameText(name[0]);
    if (name[1] > 0) s += "_" + std::to_string(name[1] - 1);
    return s;
}

const void* ClassOf(const void* object)
{
    const void* cls = nullptr;
    if (!object || !Read(static_cast<const uint8_t*>(object) + 0x10, &cls)) return nullptr;
    return cls;
}

const void* SuperOf(const void* structObject)
{
    // UStruct: UField (UObject 0x28 + Next 8) then SuperStruct at +0x30.
    const void* super = nullptr;
    if (!structObject || !Read(static_cast<const uint8_t*>(structObject) + 0x30, &super)) return nullptr;
    return super;
}

void* FindObject(const std::string& name, const std::string& className)
{
    void* found = nullptr;
    ForEachObject([&](const void* object) {
        if (found || ObjectName(object) != name) return;
        if (!className.empty() && ObjectName(ClassOf(object)) != className) return;
        found = const_cast<void*>(object);
    });
    return found;
}

bool EnsureObjects(uintptr_t imageBase)
{
    const uint8_t* items = nullptr;
    size_t stride = 0;
    int count = 0;
    if (CurrentItems(&items, &stride, &count)) return true;
    if (!imageBase || !g_ready) return false; // a module not installed yet
    const auto engine = *reinterpret_cast<uint8_t* const*>(imageBase + 0x3D878C8);
    const void* singleton = engine ? *reinterpret_cast<void* const*>(engine + 0x1E0) : nullptr;
    return LocateObjectsFrom(singleton);
}

void ForEachObject(const std::function<void(const void* object)>& visit)
{
    const uint8_t* items = nullptr;
    size_t stride = 0;
    int count = 0;
    if (!CurrentItems(&items, &stride, &count)) return;
    // Freed slots are null or hold a stale object; skip them. Without a known
    // count, a long run of them ends the walk.
    int misses = 0;
    for (int i = 0; i < count && misses < 65536; i++) {
        const void* object = ItemObject(items, stride, i);
        int32_t index = -1;
        if (!object || !Read(static_cast<const uint8_t*>(object) + 0xC, &index) || index != i) {
            misses++;
            continue;
        }
        misses = 0;
        visit(object);
    }
}

size_t ObjectSize(const void* object)
{
    const void* cls = ClassOf(object);
    int32_t size = 0;
    if (!cls || !Read(static_cast<const uint8_t*>(cls) + 0x40, &size) || size < 0x28 || size > (1 << 20)) return 0;
    return static_cast<size_t>(size);
}

const void* OuterOf(const void* object)
{
    const void* outer = nullptr;
    if (!object || !Read(static_cast<const uint8_t*>(object) + 0x20, &outer)) return nullptr;
    return outer;
}

std::string Describe(const void* object)
{
    if (!object) return "null";
    return ObjectName(ClassOf(object)) + " " + ObjectName(object);
}

} // namespace sfv::mod::ue4
