#include "FindCode.h"

namespace find_mode {

namespace {

static constexpr uint8_t UUID128_LIST_INCOMPLETE = 0x06;
static constexpr uint8_t UUID128_LIST_COMPLETE = 0x07;

// Hyphen positions in the canonical text.
constexpr bool isHyphenAt(const size_t i) { return i == 8 || i == 13 || i == 18 || i == 23; }

constexpr int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// `airUuid` is 16 bytes as received: little-endian, so the last byte of the
// text form arrives first.
bool airBytesMatchCode(const uint8_t* airUuid, const Code& code) {
  for (size_t i = 0; i < CODE_BYTES; i++) {
    if (airUuid[i] != code[CODE_BYTES - 1 - i]) return false;
  }
  return true;
}

}  // namespace

bool parseCode(const std::string_view text, Code& out) {
  if (text.size() != CODE_TEXT_BUFFER - 1) return false;

  // Each hex digit is half a byte (a nibble); two digits fill one byte.
  Code parsed{};
  size_t nibble = 0;
  for (size_t i = 0; i < text.size(); i++) {
    if (isHyphenAt(i)) {
      if (text[i] != '-') return false;
      continue;
    }
    const int value = hexValue(text[i]);
    if (value < 0) return false;
    parsed[nibble / 2] = static_cast<uint8_t>((parsed[nibble / 2] << 4) | value);
    nibble++;
  }
  out = parsed;
  return true;
}

void formatCode(const Code& code, char (&out)[CODE_TEXT_BUFFER]) {
  static constexpr char DIGITS[] = "0123456789abcdef";
  size_t nibble = 0;
  for (size_t i = 0; i < CODE_TEXT_BUFFER - 1; i++) {
    if (isHyphenAt(i)) {
      out[i] = '-';
      continue;
    }
    const uint8_t byte = code[nibble / 2];
    out[i] = DIGITS[(nibble % 2 == 0) ? (byte >> 4) : (byte & 0x0F)];
    nibble++;
  }
  out[CODE_TEXT_BUFFER - 1] = '\0';
}

bool matchesCode(const uint8_t* payload, const size_t length, const Code& code) {
  if (payload == nullptr) return false;

  // Each AD structure is [length][type][data...], where length counts type + data.
  size_t recordStart = 0;
  while (recordStart < length) {
    const size_t recordLength = payload[recordStart];
    if (recordLength == 0 || recordStart + 1 + recordLength > length) return false;

    const uint8_t type = payload[recordStart + 1];
    if (type == UUID128_LIST_COMPLETE || type == UUID128_LIST_INCOMPLETE) {
      // One record can list several UUIDs; a trailing partial one is ignored.
      const uint8_t* uuids = payload + recordStart + 2;
      const size_t uuidCount = (recordLength - 1) / CODE_BYTES;
      for (size_t n = 0; n < uuidCount; n++) {
        if (airBytesMatchCode(uuids + n * CODE_BYTES, code)) return true;
      }
    }
    recordStart += 1 + recordLength;
  }
  return false;
}

}  // namespace find_mode
