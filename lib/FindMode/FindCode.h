#pragma once

// Find mode: the secret code and how to spot it in a BLE advertisement.
//
// Find mode lets the owner locate a lost reader with a phone. While the reader
// sleeps it wakes every few minutes, listens for about a second, and sleeps
// again. The phone (nRF Connect's Advertiser) broadcasts a secret 128-bit
// "service UUID" that only this reader knows. When the reader hears it, it
// starts advertising "CP-FIND" quickly so the owner can follow the signal
// strength (RSSI) to it.
//
// This file holds the pure logic for that code: reading and writing its text
// form, and recognising it inside a raw advertisement. No hardware, so it runs
// in the host tests (test/find_code/).
//
// A BLE advertisement is up to 31 bytes made of back-to-back records:
//   [length][type][data...]   where length counts type + data
// Example captured from a phone advertising c0de0001-f1d0-4b1e-9a5e-000000000001:
//   02 01 1A                                   flags record
//   11 07 01 00 00 00 00 00 5E 9A ... 00 DE C0 complete list of 128-bit UUIDs
// On air the UUID bytes are reversed compared to the text: the text starts
// "c0de" and the air bytes end "DE C0".

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace find_mode {

// The code as 16 bytes in text order: code[0] is the first byte of
// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx".
static constexpr size_t CODE_BYTES = 16;
using Code = std::array<uint8_t, CODE_BYTES>;

// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" plus the terminator.
static constexpr size_t CODE_TEXT_BUFFER = 37;

// Parses the canonical UUID text, hex digits in either case. Leaves `out`
// untouched and returns false on any malformed input.
bool parseCode(std::string_view text, Code& out);

// Writes the canonical lowercase UUID text, the form shown on "Show code".
void formatCode(const Code& code, char (&out)[CODE_TEXT_BUFFER]);

// True when the advertising payload carries `code` in a complete (0x07) or
// incomplete (0x06) list of 128-bit service UUIDs. A record whose length runs
// past the payload, or a zero length, ends the walk: other devices' packets
// are untrusted input.
bool matchesCode(const uint8_t* payload, size_t length, const Code& code);

}  // namespace find_mode
