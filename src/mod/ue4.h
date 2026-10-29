#pragma once
// Minimal UE4 reflection: object names and classes, for logging and for
// finding game objects by class name.
//
// The name table (GNames) is found at run time: a pointer in the image's data
// to an array of chunk pointers whose first entries read "None" and
// "ByteProperty". The object array (GUObjectArray, a TArray<UObject*> the
// engine reallocates as it grows) is read from the image at every walk; for
// other builds it is found from one live object (LocateObjectsFrom).
// UObjectBase layout: +0xC index, +0x10 class, +0x18 name (index, number),
// +0x20 outer.

#include "mod/image.h"

#include <cstdint>
#include <functional>
#include <string>

namespace sfv::mod::ue4 {

bool Init(const GameImage& image);
// True once Init has found the name table (it runs on a background thread).
bool Ready();
// The name behind an FName index ("" if unknown).
std::string NameText(int32_t index);
// Object name with its number suffix, as UE4 shows it ("Foo_3").
std::string ObjectName(const void* object);
// "ClassName ObjectName", or "null" / "?".
std::string Describe(const void* object);
const void* ClassOf(const void* object);
const void* SuperOf(const void* structObject); // UStruct::SuperStruct
// The first live object with this name (number suffix included, "GameScene_0")
// whose class is named `className` (any class when empty). Slow: walks every
// object; for one-off lookups.
void* FindObject(const std::string& name, const std::string& className = "");
// Fallback when Init did not find the object array: from one live object,
// finds the item array that points at it (scans memory; seconds).
bool LocateObjectsFrom(const void* knownObject);
// Makes sure the object array is known (from the game singleton).
bool EnsureObjects(uintptr_t imageBase);
// Calls `visit` for every live object (in index order).
void ForEachObject(const std::function<void(const void* object)>& visit);
// The object's size from its class (UStruct::PropertiesSize, +0x40), or 0.
size_t ObjectSize(const void* object);
const void* OuterOf(const void* object);

} // namespace sfv::mod::ue4
