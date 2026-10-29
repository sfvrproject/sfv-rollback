#pragma once
// The update info file the project site publishes (sfvr-site, public/
// update.json), what the mod's updater reads (mod/updater.h):
//
//   { "product": "SFVR", "version": "0.2.0", "published": "2026-11-02",
//     "notes": "What changed, a few lines.",
//     "download": "releases/0.2.0/sfvr-0.2.0.zip",
//     "files": [ { "path": "sfv_rollback.dll", "size": 4012544,
//                  "sha256": "<64 hex digits>",
//                  "url": "releases/0.2.0/sfv_rollback.dll" }, ... ] }
//
// Next to it, update.json.sig: the base64 of an ECDSA P-256 signature (r and
// s, 32 bytes each) over update.json's exact bytes, SHA-256 hashed. Only the
// release script holds the private key; the mod has the public one built in
// (mod/update_key.h). URLs may be relative to update.json's.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sfv {

struct UpdateFile {
    std::string path;   // a plain file name in the game's Binaries\Win64 (SafeUpdatePath)
    uint64_t size = 0;
    std::string sha256; // lowercase hex
    std::string url;
};

struct UpdateInfo {
    std::string product;
    std::string version;
    std::string published;
    std::string notes;
    std::string download; // the full package, for first installs (the site's page)
    std::vector<UpdateFile> files;
};

bool ParseUpdateInfo(std::string_view json, UpdateInfo* out, std::string* error);

// Orders versions like "0.1.0-prototype" < "0.1.0" < "0.1.1" < "0.10.0":
// the dot-separated numbers first, then a version with a "-suffix" before
// the same one without, then the suffixes as text. <0, 0 or >0.
int CompareVersions(const std::string& a, const std::string& b);

// Whether an update may write this file: a plain name (no folders, no ".."),
// a .dll, .pdb or .txt, and never the player's settings (*.ini).
bool SafeUpdatePath(const std::string& path);

// `url` against `base` (the update info's own URL): absolute ones as they
// are, others relative to base's folder.
std::string ResolveUrl(const std::string& base, const std::string& url);

// SHA-256, as lowercase hex.
std::string Sha256Hex(const uint8_t* data, size_t size);
std::vector<uint8_t> Base64Decode(std::string_view text);
// Checks an ECDSA P-256 signature (64 bytes: r then s) over SHA-256(data)
// against a public key (64 bytes: x then y).
bool VerifySignature(const uint8_t* data, size_t size, const uint8_t* signature, size_t signatureSize,
                     const uint8_t (&publicKey)[64]);

} // namespace sfv
