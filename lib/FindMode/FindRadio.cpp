#include "FindRadio.h"

#if CROSSPOINT_FIND_MODE

#include <Arduino.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <freertos/semphr.h>
#include <nimble/porting/nimble/include/nimble/nimble_port.h>

// Why the teardown drains NimBLE's queue first
//
// NimBLE runs its own host task, which works through an event queue. Stopping
// a scan or an advertisement arms NimBLE's host timer to fire at once, which
// queues an event. NimBLEDevice::deinit() then runs on our task and, while
// shutting the host down, zeroes that timer's callout. If the queued event has
// not run yet, the host task later calls its handler through a null pointer:
// PC 0 in NimBLEDevice::host_task (InstrFetchProhibited). An earlier version
// crashed exactly like that, when NimBLEScan::start(duration) ended the scan by
// itself right as deinit ran.
//
// So stopRadio() posts a marker event to the host queue and waits until the
// host task has run it. The queue is first in, first out, so every event queued
// before the marker (including the one from the stop) has run by then, and
// nothing re-arms the timer once scanning and advertising are both stopped.
// Only then does deinit tear the stack down.

namespace find_mode {

namespace {

// How often the loops check for the code or a power button press.
static constexpr uint32_t CHECK_EVERY_MS = 10;
// Upper bound on waiting for the host task to reach the marker event.
static constexpr uint32_t DRAIN_TIMEOUT_MS = 500;

// NimBLE listens RECEIVER_ON_MS out of every RECEIVER_CYCLE_MS. Equal values
// keep the receiver on for the whole listening time.
static constexpr uint16_t RECEIVER_CYCLE_MS = 100;
static constexpr uint16_t RECEIVER_ON_MS = 100;

// In 0.625 ms units, as BLE defines it: 160 = one packet every 100 ms.
static constexpr uint16_t FOUND_ADVERTISING_INTERVAL = 160;
// A level every ESP32 BLE controller accepts; also the controller default.
static constexpr int8_t FOUND_TX_POWER_DBM = 9;

// onResult() runs on the NimBLE host task; listenForCode() polls `heard`. A
// file-level object, so a result still being handled while the scan stops
// never writes into a finished stack frame.
class CodeListener : public NimBLEScanCallbacks {
 public:
  void listenFor(const Code& wanted) {
    code = wanted;
    heard = false;
  }

  void onResult(const NimBLEAdvertisedDevice* device) override {
    const std::vector<uint8_t>& payload = device->getPayload();
    if (matchesCode(payload.data(), payload.size(), code)) heard = true;
  }

  volatile bool heard = false;

 private:
  Code code{};
};

CodeListener listener;

bool startRadio() { return NimBLEDevice::isInitialized() || NimBLEDevice::init(""); }

// The marker event for drainHostQueue(): the host task runs it and releases
// the semaphore.
StaticSemaphore_t drainedStorage;
SemaphoreHandle_t drained = nullptr;
ble_npl_event drainMarker;

void onDrainMarker(ble_npl_event*) { xSemaphoreGive(drained); }

// Waits until the host task has run everything queued so far. See the note at
// the top of this file.
bool drainHostQueue() {
  if (drained == nullptr) drained = xSemaphoreCreateBinaryStatic(&drainedStorage);
  ble_npl_event_init(&drainMarker, onDrainMarker, nullptr);
  ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &drainMarker);
  return xSemaphoreTake(drained, pdMS_TO_TICKS(DRAIN_TIMEOUT_MS)) == pdTRUE;
}

bool scanRunning() { return NimBLEDevice::getScan()->isScanning(); }
bool advertisingRunning() { return NimBLEDevice::getAdvertising()->isAdvertising(); }

// The wait shared by listening and broadcasting. Checks every CHECK_EVERY_MS
// until `ms` pass and returns the first of: HeardCode once `heard` turns true
// (when given), ButtonPressed, RadioFailed when `radioRunning` reports the
// radio stopped by itself (a NimBLE host reset ends scans and advertising
// silently), or `timeUp` when the time runs out.
RadioResult pollFor(const uint32_t ms, const PowerButtonCheck powerButtonPressed, const volatile bool* heard,
                    bool (*radioRunning)(), const RadioResult timeUp) {
  const uint32_t startedAt = millis();
  while (millis() - startedAt < ms) {
    if (heard != nullptr && *heard) return RadioResult::HeardCode;
    if (powerButtonPressed()) return RadioResult::ButtonPressed;
    if (!radioRunning()) return RadioResult::RadioFailed;
    delay(CHECK_EVERY_MS);
  }
  return timeUp;
}

}  // namespace

RadioResult listenForCode(const Code& code, const uint32_t listenMs, const PowerButtonCheck powerButtonPressed) {
  if (!startRadio()) return RadioResult::RadioFailed;

  listener.listenFor(code);
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&listener, /*wantDuplicates=*/false);
  scan->setActiveScan(false);  // passive: never send scan requests
  scan->setInterval(RECEIVER_CYCLE_MS);
  scan->setWindow(RECEIVER_ON_MS);

  LOG_DBG("FIND", "Listen start");
  // Duration 0 scans until stop(): pollFor() owns the listening time.
  if (!scan->start(0, /*isContinue=*/false, /*restart=*/true)) return RadioResult::RadioFailed;
  RadioResult result = pollFor(listenMs, powerButtonPressed, &listener.heard, scanRunning, RadioResult::NothingHeard);
  if (result == RadioResult::NothingHeard && listener.heard) result = RadioResult::HeardCode;
  // Synchronous: the scan has stopped when this returns. The event it queues
  // is handled by stopRadio().
  scan->stop();
  LOG_DBG("FIND", "Listen end: %d", static_cast<int>(result));
  return result;
}

RadioResult broadcastFound(const uint32_t broadcastMs, const PowerButtonCheck powerButtonPressed) {
  if (!startRadio()) return RadioResult::RadioFailed;

  // Broadcast from the random address NimBLE creates at every start, not the
  // chip's permanent one: someone logging Bluetooth nearby cannot recognise the
  // same reader from one found session to the next. The phone finds it by name.
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
  NimBLEDevice::setPower(FOUND_TX_POWER_DBM);

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  // Non-connectable and non-scannable: a pure broadcast nobody can connect to
  // or query. Set before the packet, because both calls rewrite the flags.
  advertising->setConnectableMode(BLE_GAP_CONN_MODE_NON);
  advertising->setDiscoverableMode(BLE_GAP_DISC_MODE_NON);

  NimBLEAdvertisementData packet;
  packet.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  packet.setName(FOUND_NAME);
  advertising->setAdvertisementData(packet);
  advertising->setMinInterval(FOUND_ADVERTISING_INTERVAL);
  advertising->setMaxInterval(FOUND_ADVERTISING_INTERVAL);
  if (!advertising->start()) return RadioResult::RadioFailed;

  const RadioResult result = pollFor(broadcastMs, powerButtonPressed, nullptr, advertisingRunning, RadioResult::TimeUp);
  advertising->stop();
  return result;
}

void stopRadio() {
  if (!NimBLEDevice::isInitialized()) return;
  LOG_DBG("FIND", "Radio stop");
  if (!drainHostQueue()) LOG_ERR("FIND", "NimBLE host did not drain in %lu ms", DRAIN_TIMEOUT_MS);
  // deinit(true) also deletes the scan and advertising objects.
  NimBLEDevice::deinit(/*clearAll=*/true);
  LOG_DBG("FIND", "Radio stopped");
}

}  // namespace find_mode

#endif  // CROSSPOINT_FIND_MODE
