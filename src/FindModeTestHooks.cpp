#include "FindModeTestHooks.h"

#if CROSSPOINT_FIND_MODE_TEST_HOOKS

#include <Arduino.h>
#include <Logging.h>
#include <esp_attr.h>
#include <esp_rtc_time.h>

#include <cstdlib>
#include <cstring>

#include "FindModeRuntime.h"

namespace {

// Both survive deep sleep and crash resets. The journal's magic, and the
// injection's, reject what power loss leaves behind.
RTC_NOINIT_ATTR find_mode::Journal journal;

static constexpr uint32_t INJECTION_MAGIC = 0x1417EC7;
struct PendingInjection {
  uint32_t magic;
  uint8_t kind;
  uint8_t reserved;
  uint16_t battery;
};
RTC_NOINIT_ATTR PendingInjection pendingInjection;

// FIND_AWAKE is printed this often while the fast path runs. Its early log
// lines print before a computer has reopened the USB port; a found-mode
// broadcast lasts a minute, so the heartbeat always gets through.
static constexpr uint32_t HEARTBEAT_MS = 2000;
uint32_t lastHeartbeatAt = 0;

// Serial bytes collected between polls, until a newline completes a command.
char lineBuffer[64];
size_t lineLength = 0;

find_mode::Journal& validJournal() {
  if (!find_mode::journalIsValid(journal)) find_mode::journalReset(journal);
  return journal;
}

void printJournal() {
  const find_mode::Journal& j = validJournal();
  logSerial.printf("FIND_JOURNAL_START written=%lu\n", static_cast<unsigned long>(j.written));
  for (size_t i = 0; i < find_mode::journalSize(j); i++) {
    const find_mode::JournalEntry& e = find_mode::journalAt(j, i);
    logSerial.printf("FINDJ %lu %u %u %lu\n", static_cast<unsigned long>(e.rtcMs), e.event, e.detail,
                     static_cast<unsigned long>(e.value));
  }
  logSerial.printf("FIND_JOURNAL_END\n");
}

bool setInjection(const char* arguments) {
  FindTestInjection kind = FindTestInjection::None;
  uint16_t battery = 0;
  if (strncmp(arguments, "crash", 5) == 0) {
    kind = FindTestInjection::Crash;
  } else if (strncmp(arguments, "hang", 4) == 0) {
    kind = FindTestInjection::Hang;
  } else if (strncmp(arguments, "radiofail", 9) == 0) {
    kind = FindTestInjection::RadioFail;
  } else if (strncmp(arguments, "battery ", 8) == 0) {
    kind = FindTestInjection::Battery;
    battery = static_cast<uint16_t>(atoi(arguments + 8));
  } else {
    return false;
  }
  pendingInjection = {INJECTION_MAGIC, static_cast<uint8_t>(kind), 0, battery};
  logSerial.printf("FIND_INJECT_OK %u %u\n", static_cast<unsigned>(kind), battery);
  return true;
}

// Commands both loop() and the fast path understand. FIND_PRESS is the fast
// path's own, handled by the caller.
bool runCommand(const char* command) {
  if (strcmp(command, "FIND_JOURNAL") == 0) {
    printJournal();
  } else if (strcmp(command, "FIND_JOURNAL_CLEAR") == 0) {
    find_mode::journalReset(journal);
    logSerial.printf("FIND_JOURNAL_CLEARED\n");
  } else if (strcmp(command, "FIND_STATE") == 0) {
    findModePrintState();
  } else if (strncmp(command, "FIND_INJECT ", 12) == 0) {
    return setInjection(command + 12);
  } else if (strcmp(command, "FIND_RETRY") == 0) {
    findModeRetryAfterSwitchOff();
    logSerial.printf("FIND_RETRY_OK\n");
  } else {
    return false;
  }
  return true;
}

}  // namespace

void findTestRecord(const find_mode::JournalEvent event, const uint8_t detail, const uint32_t value) {
  find_mode::journalAdd(validJournal(), event, detail, value, static_cast<uint32_t>(esp_rtc_get_time_us() / 1000));
}

FindTestInjection findTestTakeInjection(uint16_t& battery) {
  if (pendingInjection.magic != INJECTION_MAGIC) return FindTestInjection::None;
  const auto kind = static_cast<FindTestInjection>(pendingInjection.kind);
  battery = pendingInjection.battery;
  pendingInjection.magic = 0;
  findTestRecord(find_mode::JournalEvent::Inject, static_cast<uint8_t>(kind), battery);
  return kind;
}

bool findTestVirtualPress() {
  if (lastHeartbeatAt == 0 || millis() - lastHeartbeatAt >= HEARTBEAT_MS) {
    lastHeartbeatAt = millis();
    logSerial.printf("FIND_AWAKE %lu\n", static_cast<unsigned long>(lastHeartbeatAt));
  }
  bool pressed = false;
  while (logSerial.available() > 0) {
    const char c = static_cast<char>(logSerial.read());
    if (c != '\n' && c != '\r') {
      if (lineLength < sizeof(lineBuffer) - 1) lineBuffer[lineLength++] = c;
      continue;
    }
    lineBuffer[lineLength] = '\0';
    lineLength = 0;
    if (strncmp(lineBuffer, "CMD:", 4) != 0) continue;
    const char* command = lineBuffer + 4;
    if (strcmp(command, "FIND_PRESS") == 0) {
      pressed = true;
    } else {
      runCommand(command);
    }
  }
  return pressed;
}

bool findTestCommand(const char* command) { return runCommand(command); }

#endif  // CROSSPOINT_FIND_MODE_TEST_HOOKS
