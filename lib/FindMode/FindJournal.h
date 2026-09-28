#pragma once

// Find mode test builds: a journal of what each wake did, kept in RTC memory.
//
// The fast path is over in under a second and the USB serial port disappears
// with every sleep, so its log lines rarely reach a computer. The journal
// records each step with a timestamp instead, survives deep sleep, and is
// printed on request (FIND_JOURNAL) whenever the reader is awake long enough.
// scripts/findmode/run_suite.py reads it to check timings and outcomes.
//
// A fixed ring: once full, each new entry replaces the oldest. Pure logic,
// covered by the host tests in test/find_code/.

#include <cstddef>
#include <cstdint>

namespace find_mode {

enum class JournalEvent : uint8_t {
  Boot = 1,      // detail: esp_reset_reason(); value: 1 when it was a timer wake
  Battery,       // value: percent, or BATTERY_UNKNOWN
  ListenEnd,     // detail: RadioResult; value: ms since boot
  FoundScreen,   // value: ms since boot when drawn
  BroadcastEnd,  // detail: RadioResult; value: broadcast length in ms
  Handover,      // the owner pressed power: normal boot follows
  Sleep,         // detail: 1 from the fast path, 0 from a normal sleep; value: timer seconds
  Inject,        // detail: TestInjection consumed by this wake
};

struct JournalEntry {
  uint32_t rtcMs;  // RTC clock, which keeps counting through deep sleep
  uint8_t event;
  uint8_t detail;
  uint16_t reserved;
  uint32_t value;
};
static_assert(sizeof(JournalEntry) == 12, "JournalEntry must have no padding");

static constexpr uint32_t JOURNAL_MAGIC = 0xF1ADF00D;
static constexpr size_t JOURNAL_CAPACITY = 48;

struct Journal {
  uint32_t magic;
  uint32_t written;  // entries ever added; the ring holds the last JOURNAL_CAPACITY
  JournalEntry entries[JOURNAL_CAPACITY];
};

// Empties the journal; also the way to start one in RTC memory after power loss.
void journalReset(Journal& journal);
bool journalIsValid(const Journal& journal);
void journalAdd(Journal& journal, JournalEvent event, uint8_t detail, uint32_t value, uint32_t rtcMs);
// Entries held, at most JOURNAL_CAPACITY.
size_t journalSize(const Journal& journal);
// index 0 is the oldest entry held.
const JournalEntry& journalAt(const Journal& journal, size_t index);

}  // namespace find_mode
