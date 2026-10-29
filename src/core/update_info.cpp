#include "core/update_info.h"

#include "core/json.h"

#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace sfv {

namespace {

// "0.1.0-prototype" -> {0, 1, 0}, "prototype".
void SplitVersion(const std::string& v, std::vector<long>* numbers, std::string* suffix)
{
    const size_t dash = v.find('-');
    const std::string core = v.substr(0, dash);
    *suffix = dash == std::string::npos ? "" : v.substr(dash + 1);
    size_t at = 0;
    while (at <= core.size()) {
        const size_t dot = core.find('.', at);
        numbers->push_back(std::strtol(core.substr(at, dot - at).c_str(), nullptr, 10));
        if (dot == std::string::npos) break;
        at = dot + 1;
    }
}

} // namespace

bool ParseUpdateInfo(std::string_view json, UpdateInfo* out, std::string* error)
{
    JsonValue doc;
    if (!ParseJson(json, &doc, error)) return false;
    auto fail = [&](const std::string& what) {
        if (error) *error = what;
        return false;
    };
    if (doc.type != JsonValue::Type::Object) return fail("not a JSON object");
    UpdateInfo info;
    info.product = doc.Str("product");
    info.version = doc.Str("version");
    info.published = doc.Str("published");
    info.notes = doc.Str("notes");
    info.download = doc.Str("download");
    if (info.product != "SFVR") return fail("not an SFVR update file");
    if (info.version.empty()) return fail("no version");
    const JsonValue* files = doc.Find("files");
    if (!files || files->type != JsonValue::Type::Array || files->array.empty()) return fail("no files");
    for (const JsonValue& f : files->array) {
        UpdateFile file;
        file.path = f.Str("path");
        file.url = f.Str("url");
        file.sha256 = f.Str("sha256");
        const double size = f.Num("size", -1);
        if (!SafeUpdatePath(file.path)) return fail("a file the updater may not write: " + file.path);
        if (file.url.empty()) return fail("no url for " + file.path);
        if (file.sha256.size() != 64) return fail("no sha256 for " + file.path);
        std::transform(file.sha256.begin(), file.sha256.end(), file.sha256.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (size < 0 || size > 256.0 * 1024 * 1024) return fail("bad size for " + file.path);
        file.size = static_cast<uint64_t>(size);
        info.files.push_back(std::move(file));
    }
    *out = std::move(info);
    return true;
}

int CompareVersions(const std::string& a, const std::string& b)
{
    std::vector<long> na, nb;
    std::string sa, sb;
    SplitVersion(a, &na, &sa);
    SplitVersion(b, &nb, &sb);
    for (size_t i = 0; i < std::max(na.size(), nb.size()); i++) {
        const long x = i < na.size() ? na[i] : 0, y = i < nb.size() ? nb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    if (sa.empty() != sb.empty()) return sa.empty() ? 1 : -1;
    return sa.compare(sb) < 0 ? -1 : sa == sb ? 0 : 1;
}

bool SafeUpdatePath(const std::string& path)
{
    if (path.empty() || path.size() > 64) return false;
    for (char c : path) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    if (path.find("..") != std::string::npos || path[0] == '.') return false;
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto ends = [&](const char* ext) {
        const size_t n = std::strlen(ext);
        return lower.size() > n && lower.compare(lower.size() - n, n, ext) == 0;
    };
    return ends(".dll") || ends(".pdb") || ends(".txt");
}

std::string ResolveUrl(const std::string& base, const std::string& url)
{
    if (url.find("://") != std::string::npos) return url;
    const size_t scheme = base.find("://");
    if (scheme == std::string::npos) return url;
    if (!url.empty() && url[0] == '/') {
        const size_t host = base.find('/', scheme + 3);
        return base.substr(0, host) + url;
    }
    const size_t slash = base.find_last_of('/');
    return (slash == std::string::npos || slash < scheme + 3 ? base + "/" : base.substr(0, slash + 1)) + url;
}

std::string Sha256Hex(const uint8_t* data, size_t size)
{
    uint8_t digest[32] = {};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<uint8_t*>(data), static_cast<ULONG>(size), digest,
               sizeof(digest));
    std::string hex;
    char two[3];
    for (uint8_t b : digest) {
        std::snprintf(two, sizeof(two), "%02x", b);
        hex += two;
    }
    return hex;
}

std::vector<uint8_t> Base64Decode(std::string_view text)
{
    std::vector<uint8_t> out;
    uint32_t bits = 0;
    int count = 0;
    for (char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else continue; // padding, line breaks
        bits = bits << 6 | uint32_t(v);
        if ((count += 6) >= 8) {
            count -= 8;
            out.push_back(static_cast<uint8_t>(bits >> count));
        }
    }
    return out;
}

bool VerifySignature(const uint8_t* data, size_t size, const uint8_t* signature, size_t signatureSize,
                     const uint8_t (&publicKey)[64])
{
    if (signatureSize != 64) return false;
    bool anyKey = false;
    for (uint8_t b : publicKey) anyKey |= b != 0;
    if (!anyKey) return false;
    // BCRYPT_ECCKEY_BLOB, then x and y.
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
    header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), publicKey, 64);
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!BCRYPT_SUCCESS(BCryptImportKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob,
                                            sizeof(blob), 0))) {
        return false;
    }
    uint8_t digest[32] = {};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<uint8_t*>(data), static_cast<ULONG>(size), digest,
               sizeof(digest));
    const NTSTATUS status =
        BCryptVerifySignature(key, nullptr, digest, sizeof(digest), const_cast<uint8_t*>(signature), 64, 0);
    BCryptDestroyKey(key);
    return BCRYPT_SUCCESS(status);
}

} // namespace sfv
