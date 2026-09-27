#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lib/FindMode/FindCode.h"

namespace {

using find_mode::Code;

constexpr std::string_view PROBE_CODE_TEXT = "c0de0001-f1d0-4b1e-9a5e-000000000001";

// Captured by nRF Connect on a Pixel while the Mac advertised the probe code with bless.
constexpr std::string_view PIXEL_PACKET = "02011A11070100000000005E9A1E4BD0F10100DEC0";

// The first probe packet: flags plus the name "CPFINDPROBE", no UUID.
constexpr std::string_view NAME_ONLY_PACKET = "02011A0C09435046494E4450524F4245";

std::vector<uint8_t> bytesFromHex(const std::string_view hex) {
  std::vector<uint8_t> bytes;
  bytes.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    bytes.push_back(static_cast<uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
  }
  return bytes;
}

Code probeCode() {
  Code code{};
  find_mode::parseCode(PROBE_CODE_TEXT, code);
  return code;
}

bool packetMatches(const std::vector<uint8_t>& packet, const Code& code) {
  return find_mode::matchesCode(packet.data(), packet.size(), code);
}

TEST(FindCodeParse, ReadsTheProbeCode) {
  Code code{};

  const bool parsed = find_mode::parseCode(PROBE_CODE_TEXT, code);

  EXPECT_TRUE(parsed && code[0] == 0xC0 && code[1] == 0xDE && code[15] == 0x01);
}

TEST(FindCodeParse, ReadsUppercaseDigits) {
  Code code{};

  const bool parsed = find_mode::parseCode("C0DE0001-F1D0-4B1E-9A5E-000000000001", code);

  EXPECT_TRUE(parsed && code == probeCode());
}

TEST(FindCodeParse, RejectsAMissingHyphen) {
  Code code{};

  const bool parsed = find_mode::parseCode("c0de0001f1d0-4b1e-9a5e-0000000000011", code);

  EXPECT_FALSE(parsed);
}

TEST(FindCodeParse, RejectsANonHexDigit) {
  Code code{};

  const bool parsed = find_mode::parseCode("c0de0001-f1d0-4b1e-9a5e-00000000000g", code);

  EXPECT_FALSE(parsed);
}

TEST(FindCodeParse, RejectsShortText) {
  Code code{};

  const bool parsed = find_mode::parseCode("c0de0001-f1d0-4b1e-9a5e", code);

  EXPECT_FALSE(parsed);
}

TEST(FindCodeFormat, WritesTheProbeCodeBack) {
  char text[find_mode::CODE_TEXT_BUFFER];

  find_mode::formatCode(probeCode(), text);

  EXPECT_EQ(std::string_view(text), PROBE_CODE_TEXT);
}

TEST(FindCodeMatch, ThePixelPacketCarriesTheProbeCode) {
  const auto packet = bytesFromHex(PIXEL_PACKET);

  const bool matched = packetMatches(packet, probeCode());

  EXPECT_TRUE(matched);
}

TEST(FindCodeMatch, TheProbeCodeInTextOrderIsNotAMatch) {
  // Same record, UUID bytes written in text order instead of air order.
  const auto packet = bytesFromHex("02011A1107C0DE0001F1D04B1E9A5E000000000001");

  const bool matched = packetMatches(packet, probeCode());

  EXPECT_FALSE(matched);
}

TEST(FindCodeMatch, AnotherCodeIsNotAMatch) {
  const auto packet = bytesFromHex(PIXEL_PACKET);
  Code other{};
  find_mode::parseCode("c0de0001-f1d0-4b1e-9a5e-000000000002", other);

  const bool matched = packetMatches(packet, other);

  EXPECT_FALSE(matched);
}

TEST(FindCodeMatch, ANamePacketIsNotAMatch) {
  const auto packet = bytesFromHex(NAME_ONLY_PACKET);

  const bool matched = packetMatches(packet, probeCode());

  EXPECT_FALSE(matched);
}

TEST(FindCodeMatch, AnIncompleteListMatches) {
  const auto packet = bytesFromHex("02011A11060100000000005E9A1E4BD0F10100DEC0");

  const bool matched = packetMatches(packet, probeCode());

  EXPECT_TRUE(matched);
}

TEST(FindCodeMatch, TheSecondCodeInAListMatches) {
  const auto packet = bytesFromHex(
      "02011A2107"
      "0200000000005E9A1E4BD0F10100DEC0"
      "0100000000005E9A1E4BD0F10100DEC0");

  const bool matched = packetMatches(packet, probeCode());

  EXPECT_TRUE(matched);
}

TEST(FindCodeMatch, ACodeAfterANameMatches) {
  const auto packet = bytesFromHex(
      "0409435046"
      "11070100000000005E9A1E4BD0F10100DEC0");

  const bool matched = packetMatches(packet, probeCode());

  EXPECT_TRUE(matched);
}

TEST(FindCodeMatch, ALengthPastTheEndIsNotAMatch) {
  // The UUID record claims 0x12 bytes but the packet ends one byte early.
  const auto packet = bytesFromHex("02011A12070100000000005E9A1E4BD0F10100DEC0");

  const bool matched = packetMatches(packet, probeCode());

  EXPECT_FALSE(matched);
}

TEST(FindCodeMatch, AZeroLengthStopsTheWalk) {
  const auto packet = bytesFromHex(
      "00"
      "11070100000000005E9A1E4BD0F10100DEC0");

  const bool matched = packetMatches(packet, probeCode());

  EXPECT_FALSE(matched);
}

TEST(FindCodeMatch, AShortUuidRecordIsNotAMatch) {
  // Fifteen UUID bytes: not a whole 128-bit UUID.
  const auto packet = bytesFromHex("10070100000000005E9A1E4BD0F10100DE");

  const bool matched = packetMatches(packet, probeCode());

  EXPECT_FALSE(matched);
}

TEST(FindCodeMatch, AnEmptyPayloadIsNotAMatch) {
  const std::vector<uint8_t> packet;

  const bool matched = packetMatches(packet, probeCode());

  EXPECT_FALSE(matched);
}

}  // namespace
