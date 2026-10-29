#pragma once
// A small JSON reader for the update info file (update_info.h): objects,
// arrays, strings (with \uXXXX escapes), numbers, true, false, null.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sfv {

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object; // in file order

    // The member `key` of an object; nullptr when absent or not an object.
    const JsonValue* Find(std::string_view key) const;
    // The member as a string; `fallback` when absent or not a string.
    std::string Str(std::string_view key, const std::string& fallback = "") const;
    double Num(std::string_view key, double fallback = 0) const;
};

bool ParseJson(std::string_view text, JsonValue* out, std::string* error);

} // namespace sfv
