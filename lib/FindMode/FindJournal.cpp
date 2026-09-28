#include "FindJournal.h"

namespace find_mode {

void journalReset(Journal& journal) {
  journal = {};
  journal.magic = JOURNAL_MAGIC;
}

bool journalIsValid(const Journal& journal) { return journal.magic == JOURNAL_MAGIC; }

void journalAdd(Journal& journal, const JournalEvent event, const uint8_t detail, const uint32_t value,
                const uint32_t rtcMs) {
  JournalEntry& entry = journal.entries[journal.written % JOURNAL_CAPACITY];
  entry = {rtcMs, static_cast<uint8_t>(event), detail, 0, value};
  journal.written++;
}

size_t journalSize(const Journal& journal) {
  return journal.written < JOURNAL_CAPACITY ? journal.written : JOURNAL_CAPACITY;
}

const JournalEntry& journalAt(const Journal& journal, const size_t index) {
  // Once the ring has wrapped, the oldest entry sits where the next one goes.
  const size_t oldest = journal.written < JOURNAL_CAPACITY ? 0 : journal.written % JOURNAL_CAPACITY;
  return journal.entries[(oldest + index) % JOURNAL_CAPACITY];
}

}  // namespace find_mode
