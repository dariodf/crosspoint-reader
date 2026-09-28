#include <gtest/gtest.h>

#include <cstdint>

#include "lib/FindMode/FindJournal.h"

namespace {

using find_mode::Journal;
using find_mode::JournalEvent;

Journal anEmptyJournal() {
  Journal journal;
  find_mode::journalReset(journal);
  return journal;
}

// Adds `count` wakes whose value is their order: 0, 1, 2, ...
void addWakes(Journal& journal, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) {
    find_mode::journalAdd(journal, JournalEvent::Boot, 0, i, 1000 * i);
  }
}

TEST(FindJournal, AResetJournalIsValidAndEmpty) {
  const Journal journal = anEmptyJournal();

  const bool emptyAndValid = find_mode::journalIsValid(journal) && find_mode::journalSize(journal) == 0;

  EXPECT_TRUE(emptyAndValid);
}

TEST(FindJournal, ZeroedRtcMemoryIsNotAJournal) {
  const Journal journal{};

  const bool valid = find_mode::journalIsValid(journal);

  EXPECT_FALSE(valid);
}

TEST(FindJournal, EntriesComeBackOldestFirst) {
  Journal journal = anEmptyJournal();
  addWakes(journal, 3);

  const uint32_t second = find_mode::journalAt(journal, 1).value;

  EXPECT_EQ(second, 1u);
}

TEST(FindJournal, AnEntryKeepsWhatWasAdded) {
  Journal journal = anEmptyJournal();
  find_mode::journalAdd(journal, JournalEvent::ListenEnd, 2, 838, 5000);

  const auto& entry = find_mode::journalAt(journal, 0);

  EXPECT_TRUE(entry.event == static_cast<uint8_t>(JournalEvent::ListenEnd) && entry.detail == 2 && entry.value == 838 &&
              entry.rtcMs == 5000);
}

TEST(FindJournal, AFullJournalHoldsItsCapacity) {
  Journal journal = anEmptyJournal();
  addWakes(journal, find_mode::JOURNAL_CAPACITY + 10);

  const size_t size = find_mode::journalSize(journal);

  EXPECT_EQ(size, find_mode::JOURNAL_CAPACITY);
}

TEST(FindJournal, AWrappedJournalDropsTheOldest) {
  Journal journal = anEmptyJournal();
  addWakes(journal, find_mode::JOURNAL_CAPACITY + 10);

  const uint32_t oldest = find_mode::journalAt(journal, 0).value;

  EXPECT_EQ(oldest, 10u);
}

TEST(FindJournal, AWrappedJournalEndsWithTheNewest) {
  Journal journal = anEmptyJournal();
  addWakes(journal, find_mode::JOURNAL_CAPACITY + 10);

  const uint32_t newest = find_mode::journalAt(journal, find_mode::JOURNAL_CAPACITY - 1).value;

  EXPECT_EQ(newest, find_mode::JOURNAL_CAPACITY + 9);
}

}  // namespace
