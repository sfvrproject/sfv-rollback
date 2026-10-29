#include "core/match_code.h"

#include <cctype>
#include <vector>

namespace sfv {

namespace {

const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
const char kRoomAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // Weyvelength's
constexpr size_t kRoomCodeLength = 6;                             // the server's --room-code-length

bool DecodeRoom(const std::string& clean, MatchCode* out)
{
    std::string room;
    for (char c : clean) room += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (room.size() == kRoomCodeLength + 1 && room[0] == 'W') room.erase(0, 1);
    if (room.size() != kRoomCodeLength) return false;
    for (char c : room) {
        bool ok = false;
        for (const char* a = kRoomAlphabet; *a; a++) ok = ok || *a == c;
        if (!ok) return false;
    }
    *out = MatchCode{};
    out->kind = MatchCode::Kind::Room;
    out->room = room;
    return true;
}

int Decode32(char c)
{
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    for (int i = 0; i < 32; i++) {
        if (kAlphabet[i] == c) return i;
    }
    return -1;
}

std::vector<uint8_t> Payload(const MatchCode& code)
{
    std::vector<uint8_t> bytes;
    for (int i = 0; i < 6; i++) bytes.push_back(uint8_t(code.address >> (8 * i)));
    bytes.push_back(uint8_t(code.nonce));
    bytes.push_back(uint8_t(code.nonce >> 8));
    return bytes;
}

int Check(const std::vector<int>& digits)
{
    int sum = 0;
    for (size_t i = 0; i < digits.size(); i++) sum = (sum * 7 + digits[i] + int(i)) % 32;
    return sum;
}

} // namespace

std::string EncodeMatchCode(const MatchCode& code)
{
    if (code.kind == MatchCode::Kind::Room) return code.room;
    const std::vector<uint8_t> bytes = Payload(code);
    std::vector<int> digits;
    uint32_t acc = 0;
    int bits = 0;
    for (uint8_t b : bytes) {
        acc = (acc << 8) | b;
        bits += 8;
        while (bits >= 5) {
            digits.push_back((acc >> (bits - 5)) & 31);
            bits -= 5;
        }
    }
    if (bits > 0) digits.push_back((acc << (5 - bits)) & 31);
    digits.push_back(Check(digits));

    std::string out = "U";
    for (size_t i = 0; i < digits.size(); i++) {
        if (i % 4 == 0) out += '-';
        out += kAlphabet[digits[i]];
    }
    return out;
}

bool DecodeMatchCode(const std::string& text, MatchCode* out)
{
    std::string clean;
    for (char c : text) {
        if (c != '-' && c != ' ' && c != '\t' && c != '\r' && c != '\n') clean += c;
    }
    if (clean.size() < 3) return false;
    MatchCode code;
    code.kind = MatchCode::Kind::Udp;
    const char kind = static_cast<char>(std::toupper(static_cast<unsigned char>(clean[0])));
    // A UDP code is 15 characters without its dashes; a room code 6 (7 with W).
    if (kind != 'U' || clean.size() < 12) return DecodeRoom(clean, out);

    std::vector<int> digits;
    for (size_t i = 1; i < clean.size(); i++) {
        const int d = Decode32(clean[i]);
        if (d < 0) return false;
        digits.push_back(d);
    }
    const int check = digits.back();
    digits.pop_back();
    if (Check(digits) != check) return false;

    const size_t want = 8;
    std::vector<uint8_t> bytes;
    uint32_t acc = 0;
    int bits = 0;
    for (int d : digits) {
        acc = (acc << 5) | uint32_t(d);
        bits += 5;
        if (bits >= 8) {
            bytes.push_back(uint8_t(acc >> (bits - 8)));
            bits -= 8;
        }
    }
    if (bytes.size() != want) return false;

    for (int i = 0; i < 6; i++) code.address |= uint64_t(bytes[i]) << (8 * i);
    code.nonce = uint16_t(bytes[6] | (bytes[7] << 8));
    *out = code;
    return true;
}

} // namespace sfv
