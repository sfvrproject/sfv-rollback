#include "core/json.h"

#include <cstdlib>

namespace sfv {

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    bool Document(JsonValue* out, std::string* error)
    {
        const bool ok = Value(*out, 0) && (Space(), at_ == s_.size());
        if (!ok && error) *error = (error_.empty() ? std::string("unexpected text") : error_) + " at offset " + std::to_string(at_);
        return ok;
    }

private:
    std::string_view s_;
    size_t at_ = 0;
    std::string error_;

    bool Fail(const char* what)
    {
        if (error_.empty()) error_ = what;
        return false;
    }
    void Space()
    {
        while (at_ < s_.size() && (s_[at_] == ' ' || s_[at_] == '\t' || s_[at_] == '\n' || s_[at_] == '\r')) at_++;
    }
    bool Eat(char c)
    {
        Space();
        if (at_ < s_.size() && s_[at_] == c) {
            at_++;
            return true;
        }
        return false;
    }
    bool Word(std::string_view w)
    {
        if (s_.substr(at_, w.size()) != w) return false;
        at_ += w.size();
        return true;
    }

    bool Value(JsonValue& v, int depth)
    {
        if (depth > 32) return Fail("nested too deeply");
        Space();
        if (at_ >= s_.size()) return Fail("unexpected end");
        const char c = s_[at_];
        if (c == '{') return Object(v, depth);
        if (c == '[') return Array(v, depth);
        if (c == '"') {
            v.type = JsonValue::Type::String;
            return String(v.string);
        }
        if (Word("true")) {
            v.type = JsonValue::Type::Bool;
            v.boolean = true;
            return true;
        }
        if (Word("false")) {
            v.type = JsonValue::Type::Bool;
            return true;
        }
        if (Word("null")) return true;
        return Number(v);
    }

    bool Object(JsonValue& v, int depth)
    {
        v.type = JsonValue::Type::Object;
        at_++;
        if (Eat('}')) return true;
        do {
            Space();
            std::string key;
            if (at_ >= s_.size() || s_[at_] != '"' || !String(key)) return Fail("expected a member name");
            if (!Eat(':')) return Fail("expected ':'");
            JsonValue member;
            if (!Value(member, depth + 1)) return false;
            v.object.emplace_back(std::move(key), std::move(member));
        } while (Eat(','));
        return Eat('}') || Fail("expected '}'");
    }

    bool Array(JsonValue& v, int depth)
    {
        v.type = JsonValue::Type::Array;
        at_++;
        if (Eat(']')) return true;
        do {
            JsonValue item;
            if (!Value(item, depth + 1)) return false;
            v.array.push_back(std::move(item));
        } while (Eat(','));
        return Eat(']') || Fail("expected ']'");
    }

    static void Utf8(std::string& out, unsigned cp)
    {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | cp >> 6);
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | cp >> 12);
            out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else {
            out += static_cast<char>(0xF0 | cp >> 18);
            out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
            out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool Hex4(unsigned* out)
    {
        if (at_ + 4 > s_.size()) return false;
        unsigned v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = s_[at_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return false;
        }
        *out = v;
        return true;
    }

    bool String(std::string& out)
    {
        at_++; // the opening quote
        while (at_ < s_.size()) {
            const char c = s_[at_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return Fail("control character in a string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= s_.size()) break;
            const char e = s_[at_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!Hex4(&cp)) return Fail("bad \\u escape");
                // A surrogate pair is one code point.
                if (cp >= 0xD800 && cp < 0xDC00 && Word("\\u")) {
                    unsigned low;
                    if (!Hex4(&low) || low < 0xDC00 || low >= 0xE000) return Fail("bad surrogate pair");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                Utf8(out, cp);
                break;
            }
            default:
                return Fail("bad escape");
            }
        }
        return Fail("unterminated string");
    }

    bool Number(JsonValue& v)
    {
        const size_t from = at_;
        if (at_ < s_.size() && s_[at_] == '-') at_++;
        while (at_ < s_.size() && ((s_[at_] >= '0' && s_[at_] <= '9') || s_[at_] == '.' || s_[at_] == 'e' ||
                                   s_[at_] == 'E' || s_[at_] == '+' || s_[at_] == '-')) {
            at_++;
        }
        if (at_ == from) return Fail("unexpected character");
        const std::string text(s_.substr(from, at_ - from));
        char* end = nullptr;
        v.type = JsonValue::Type::Number;
        v.number = std::strtod(text.c_str(), &end);
        return (end && *end == 0) || Fail("bad number");
    }
};

} // namespace

const JsonValue* JsonValue::Find(std::string_view key) const
{
    if (type != Type::Object) return nullptr;
    for (const auto& [k, v] : object) {
        if (k == key) return &v;
    }
    return nullptr;
}

std::string JsonValue::Str(std::string_view key, const std::string& fallback) const
{
    const JsonValue* v = Find(key);
    return v && v->type == Type::String ? v->string : fallback;
}

double JsonValue::Num(std::string_view key, double fallback) const
{
    const JsonValue* v = Find(key);
    return v && v->type == Type::Number ? v->number : fallback;
}

bool ParseJson(std::string_view text, JsonValue* out, std::string* error)
{
    JsonValue v;
    Parser p(text);
    if (!p.Document(&v, error)) return false;
    *out = std::move(v);
    return true;
}

} // namespace sfv
