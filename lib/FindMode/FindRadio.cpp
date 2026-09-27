#include "FindRadio.h"

#if CROSSPOINT_FIND_MODE

#include <Arduino.h>
#include <Logging.h>
#include <NimBLEDevice.h>

// Why the scan is timed by hand and torn down slowly
//
// NimBLE runs its own host task, which delivers scan results and scan events
// through a queue. NimBLEScan::start(duration) can end a scan by itself, but
// that "scan ended" event arrives on the host task at the moment the duration
// runs out. Our fast path tears the stack down right after the same window, so
// the event raced NimBLEDevice::deinit(): the host task ran a handler out of
// memory that deinit had already released, jumped to address 0, and the chip
// crashed (InstrFetchProhibited in NimBLEDevice::host_task, seen on the first
// devkit run).
//
// So the scan runs with no NimBLE duration and our loop decides when it ends;
// stopScan() waits until NimBLE reports the scan stopped, and stopRadio() gives
// the host task a moment to drain before deinit, then retries once, the same
// pattern as the SDK's BleKeyboardHost::end().

namespace find_mode {

namespace {

// How often the loops check for the code or a power button press.
static constexpr uint32_t CHECK_EVERY_MS = 10;
// Time for the host task to drain its queue before deinit.
static constexpr uint32_t TEARDOWN_SETTLE_MS = 20;
static constexpr uint32_t SCAN_STOP_TIMEOUT_MS = 200;

// NimBLE listens RECEIVER_ON_MS out of every RECEIVER_CYCLE_MS. Equal values
// keep the receiver on for the whole listening time.
static constexpr uint16_t RECEIVER_CYCLE_MS = 100;
static constexpr uint16_t RECEIVER_ON_MS = 100;

// In 0.625 ms units, as BLE defines it: 160 = one packet every 100 ms.
static constexpr uint16_t FOUND_ADVERTISING_INTERVAL = 160;
// A level every ESP32 BLE controller accepts.
static constexpr int8_t FOUND_TX_POWER_DBM = 9;

// onResult() runs on the NimBLE host task; listenForCode() polls `heard`.
class CodeListener : public NimBLEScanCallbacks {
 public:
  explicit CodeListener(const Code& code) : code(code) {}

  void onResult(const NimBLEAdvertisedDevice* device) override {
    const std::vector<uint8_t>& payload = device->getPayload();
    if (matchesCode(payload.data(), payload.size(), code)) heard = true;
  }

  volatile bool heard = false;

 private:
  const Code& code;
};

bool startRadio() { return NimBLEDevice::isInitialized() || NimBLEDevice::init(""); }

// Stops the scan and waits for NimBLE to confirm it, so no scan event is still
// queued when stopRadio() tears the stack down.
void stopScan(NimBLEScan* scan) {
  scan->stop();
  const uint32_t startedAt = millis();
  while (scan->isScanning() && millis() - startedAt < SCAN_STOP_TIMEOUT_MS) delay(CHECK_EVERY_MS);
}

}  // namespace

RadioResult listenForCode(const Code& code, const uint32_t listenMs, const PowerButtonCheck powerButtonPressed) {
  if (!startRadio()) return RadioResult::RadioFailed;

  CodeListener listener(code);
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&listener, /*wantDuplicates=*/false);
  scan->setActiveScan(false);  // passive: never send scan requests
  scan->setInterval(RECEIVER_CYCLE_MS);
  scan->setWindow(RECEIVER_ON_MS);
  scan->setMaxResults(0);  // results go to the callback only, nothing kept on the heap

  RadioResult result = RadioResult::NothingHeard;
  LOG_DBG("FIND", "Listen start");
  // Duration 0: scan until stopScan(). See the note at the top of this file.
  if (!scan->start(0, /*isContinue=*/false, /*restart=*/true)) {
    result = RadioResult::RadioFailed;
  } else {
    const uint32_t startedAt = millis();
    while (millis() - startedAt < listenMs) {
      if (listener.heard) {
        result = RadioResult::HeardCode;
        break;
      }
      if (powerButtonPressed()) {
        result = RadioResult::ButtonPressed;
        break;
      }
      delay(CHECK_EVERY_MS);
    }
    if (result == RadioResult::NothingHeard && listener.heard) result = RadioResult::HeardCode;
    stopScan(scan);
  }
  LOG_DBG("FIND", "Listen end: %d", static_cast<int>(result));

  // `listener` lives on this stack frame: detach it before returning.
  scan->setScanCallbacks(nullptr);
  return result;
}

RadioResult broadcastFound(const uint32_t broadcastMs, const PowerButtonCheck powerButtonPressed) {
  if (!startRadio()) return RadioResult::RadioFailed;

  NimBLEDevice::setPower(FOUND_TX_POWER_DBM);

  NimBLEAdvertisementData packet;
  packet.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  packet.setName(FOUND_NAME);

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setAdvertisementData(packet);
  advertising->setConnectableMode(BLE_GAP_CONN_MODE_NON);  // broadcast only, nobody can connect
  advertising->setMinInterval(FOUND_ADVERTISING_INTERVAL);
  advertising->setMaxInterval(FOUND_ADVERTISING_INTERVAL);
  if (!advertising->start()) return RadioResult::RadioFailed;

  RadioResult result = RadioResult::TimeUp;
  const uint32_t startedAt = millis();
  while (millis() - startedAt < broadcastMs) {
    if (powerButtonPressed()) {
      result = RadioResult::ButtonPressed;
      break;
    }
    delay(CHECK_EVERY_MS);
  }
  advertising->stop();
  return result;
}

void stopRadio() {
  if (!NimBLEDevice::isInitialized()) return;
  LOG_DBG("FIND", "Radio stop");
  delay(TEARDOWN_SETTLE_MS);
  NimBLEDevice::deinit(/*clearAll=*/true);
  // deinit leaves the stack up when its stop raced another event: one more try.
  if (NimBLEDevice::isInitialized()) {
    delay(TEARDOWN_SETTLE_MS);
    NimBLEDevice::deinit(/*clearAll=*/true);
  }
  LOG_DBG("FIND", "Radio stopped");
}

}  // namespace find_mode

#endif  // CROSSPOINT_FIND_MODE
