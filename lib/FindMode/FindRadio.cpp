#include "FindRadio.h"

#if CROSSPOINT_FIND_MODE

#include <Arduino.h>
#include <Logging.h>
#include <NimBLEDevice.h>

namespace find_mode {

namespace {

static constexpr uint32_t POLL_MS = 10;
static constexpr uint32_t SETTLE_MS = 20;
static constexpr uint32_t STOP_WAIT_MS = 200;

// Scan interval equal to the window keeps the receiver on without gaps.
static constexpr uint16_t SCAN_INTERVAL_MS = 100;
static constexpr uint16_t SCAN_WINDOW_MS = 100;

// Advertising interval in 0.625 ms units: 160 = 100 ms.
static constexpr uint16_t FOUND_INTERVAL_UNITS = 160;
static constexpr int8_t FOUND_TX_POWER_DBM = 9;

// Runs on the NimBLE host task; the scan loop below polls `heard`.
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

bool radioOn() { return NimBLEDevice::isInitialized() || NimBLEDevice::init(""); }

// Stops the scan and waits for NimBLE to confirm, so no scan event is still in
// flight when the stack is torn down.
void stopScan(NimBLEScan* scan) {
  scan->stop();
  const uint32_t startedAt = millis();
  while (scan->isScanning() && millis() - startedAt < STOP_WAIT_MS) delay(POLL_MS);
}

}  // namespace

RadioResult listenForCode(const Code& code, const uint32_t windowMs, const ButtonCheck buttonPressed) {
  if (!radioOn()) return RadioResult::RadioFailed;

  CodeListener listener(code);
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&listener, /*wantDuplicates=*/false);
  scan->setActiveScan(false);
  scan->setInterval(SCAN_INTERVAL_MS);
  scan->setWindow(SCAN_WINDOW_MS);
  scan->setMaxResults(0);  // results go to the callback only, nothing kept on the heap

  // Duration 0 scans until stopped: this loop owns the window, so NimBLE's own
  // scan-end event never races the teardown.
  RadioResult result = RadioResult::Quiet;
  LOG_DBG("FIND", "Scan start");
  if (!scan->start(0, /*isContinue=*/false, /*restart=*/true)) {
    result = RadioResult::RadioFailed;
  } else {
    const uint32_t startedAt = millis();
    while (millis() - startedAt < windowMs) {
      if (listener.heard) {
        result = RadioResult::HeardCode;
        break;
      }
      if (buttonPressed()) {
        result = RadioResult::ButtonPressed;
        break;
      }
      delay(POLL_MS);
    }
    if (result == RadioResult::Quiet && listener.heard) result = RadioResult::HeardCode;
    stopScan(scan);
  }
  LOG_DBG("FIND", "Scan end: %d", static_cast<int>(result));

  // `listener` lives on this stack frame: detach it before returning.
  scan->setScanCallbacks(nullptr);
  return result;
}

RadioResult announceFound(const uint32_t durationMs, const ButtonCheck buttonPressed) {
  if (!radioOn()) return RadioResult::RadioFailed;

  NimBLEDevice::setPower(FOUND_TX_POWER_DBM);

  NimBLEAdvertisementData data;
  data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  data.setName(FOUND_NAME);

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setAdvertisementData(data);
  advertising->setConnectableMode(BLE_GAP_CONN_MODE_NON);
  advertising->setMinInterval(FOUND_INTERVAL_UNITS);
  advertising->setMaxInterval(FOUND_INTERVAL_UNITS);
  if (!advertising->start()) return RadioResult::RadioFailed;

  RadioResult result = RadioResult::Quiet;
  const uint32_t startedAt = millis();
  while (millis() - startedAt < durationMs) {
    if (buttonPressed()) {
      result = RadioResult::ButtonPressed;
      break;
    }
    delay(POLL_MS);
  }
  advertising->stop();
  return result;
}

void radioOff() {
  if (!NimBLEDevice::isInitialized()) return;
  LOG_DBG("FIND", "Radio off");
  delay(SETTLE_MS);
  NimBLEDevice::deinit(/*clearAll=*/true);
  // A stop that raced another event leaves the stack up: one more try.
  if (NimBLEDevice::isInitialized()) {
    delay(SETTLE_MS);
    NimBLEDevice::deinit(/*clearAll=*/true);
  }
  LOG_DBG("FIND", "Radio off done");
}

}  // namespace find_mode

#endif  // CROSSPOINT_FIND_MODE
