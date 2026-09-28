"""Check find mode end to end on a test build, with nobody touching the device.

    python3 scripts/findmode/run_suite.py --port /dev/cu.usbmodem1101 --out /tmp/findmode-suite

Needs a find-mode test build (CROSSPOINT_FIND_MODE_TEST_HOOKS, a 1 minute
interval and the probe code, see scripts/findmode/README.md), `pip install
pyserial bleak bless pillow`, and Bluetooth permission for the terminal. The
device must be awake when the suite starts (right after flashing is fine).

The suite drives the device over its USB serial port (test commands, the
journal) and plays the owner's phone with emit.py while monitor.py watches for
CP-FIND. It prints one PASS/FAIL line per check with the number measured, and
writes suite.log, the screenshots and report.txt into --out.

The last scenario (low battery) leaves the device asleep until its power
button is pressed: the suite asks for that one press and checks the wake.
"""

import argparse
import glob
import os
import queue
import re
import subprocess
import sys
import threading
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PROBE_CODE = "c0de0001-f1d0-4b1e-9a5e-000000000001"

# JournalEvent and RadioResult values, as in lib/FindMode/FindJournal.h and FindRadio.h.
EV_BOOT, EV_BATTERY, EV_LISTEN_END, EV_FOUND_SCREEN, EV_BROADCAST_END, EV_HANDOVER, EV_SLEEP, EV_INJECT = range(1, 9)
EV_BROADCAST_QUIET = 11
HEARD, NOTHING_HEARD, TIME_UP, BUTTON_PRESSED, RADIO_FAILED, PHONE_GONE = range(6)
ESP_RST_PANIC = 4
ESP_RST_DEEPSLEEP = 8

# Pass thresholds. Each check prints the measured value next to its limit.
QUIET_WAKE_MAX_MS = 900
HEARD_WAKE_MAX_MS = 900
INTERVAL_S = 60
INTERVAL_TOLERANCE_S = 4
# Found mode stops this long after the phone goes quiet (PHONE_GONE_MS).
PHONE_GONE_S = 15
# Measured on the device, from the phone's last packet to the broadcast's end,
# so macOS advertising for ~10 s after emit.py stops leaves it unchanged.
PHONE_GONE_TOLERANCE_MS = 200
HANG_GUARD_MAX_S = 7


class Device:
    """The device's USB serial port, which disappears at every sleep.

    A reader thread reconnects whenever the port comes back and queues each
    text line (and each screenshot) with the time it arrived.
    """

    def __init__(self, port_glob, log):
        self.port_glob = port_glob
        self.log = log
        self.lines = queue.Queue()
        self.screens = queue.Queue()
        self.port = None
        self.lock = threading.Lock()
        self.running = True
        threading.Thread(target=self._read_forever, daemon=True).start()

    def _read_forever(self):
        buffer = b""
        while self.running:
            ports = glob.glob(self.port_glob)
            if not ports:
                with self.lock:
                    self.port = None
                time.sleep(0.05)
                continue
            try:
                port = serial.Serial(ports[0], 115200, timeout=0.2)
                with self.lock:
                    self.port = port
                self.log("PORT UP")
                buffer = b""
                while self.running:
                    buffer += port.read(4096)
                    while b"\n" in buffer:
                        raw, buffer = buffer.split(b"\n", 1)
                        line = raw.decode(errors="replace").strip()
                        if line.startswith("SCREENSHOT_START:"):
                            size = int(line.split(":")[1])
                            while len(buffer) < size:
                                buffer += port.read(size - len(buffer))
                            self.screens.put(buffer[:size])
                            buffer = buffer[size:]
                            continue
                        if line and "[MEM]" not in line:
                            self.log("  " + line)
                            self.lines.put((time.time(), line))
            except (serial.SerialException, OSError):
                with self.lock:
                    self.port = None
                self.log("PORT GONE")
                time.sleep(0.05)

    def send(self, command, wait_s=30):
        """Sends CMD:<command> once the port is up. False when it never came up."""
        end = time.time() + wait_s
        while time.time() < end:
            with self.lock:
                port = self.port
            if port is not None:
                try:
                    port.write(f"CMD:{command}\n".encode())
                    self.log(f"> CMD:{command}")
                    return True
                except (serial.SerialException, OSError):
                    pass
            time.sleep(0.05)
        return False

    def wait_for(self, pattern, timeout_s, since=0.0):
        """The first line matching `pattern` within `timeout_s`, or None.

        Lines that arrived before `since` are skipped: a wait for something the
        device has yet to do must not match an older line still queued.
        """
        regex = re.compile(pattern)
        end = time.time() + timeout_s
        while time.time() < end:
            try:
                arrived, line = self.lines.get(timeout=max(0.05, end - time.time()))
            except queue.Empty:
                break
            if arrived >= since and regex.search(line):
                return line
        return None

    def drain(self):
        while not self.lines.empty():
            self.lines.get_nowait()

    def journal(self):
        """Asks for the journal and returns [(rtc_ms, event, detail, value)], or None."""
        self.drain()
        if not self.send("FIND_JOURNAL"):
            return None
        if self.wait_for(r"^FIND_JOURNAL_START", 10) is None:
            return None
        entries = []
        end = time.time() + 10
        while time.time() < end:
            line = self.wait_for(r"^(FINDJ |FIND_JOURNAL_END)", 5)
            if line is None or line.startswith("FIND_JOURNAL_END"):
                return entries
            _, rtc_ms, event, detail, value = line.split()
            entries.append((int(rtc_ms), int(event), int(detail), int(value)))
        return None

    def state(self):
        """Asks for the state and returns it as a dict, or None."""
        self.drain()
        if not self.send("FIND_STATE"):
            return None
        line = self.wait_for(r"^FIND_STATE ", 10)
        if line is None:
            return None
        return {k: int(v) for k, v in (pair.split("=") for pair in line.split()[1:])}

    def port_is_up(self):
        with self.lock:
            return self.port is not None


class Suite:
    def __init__(self, args):
        self.args = args
        os.makedirs(args.out, exist_ok=True)
        self.log_file = open(os.path.join(args.out, "suite.log"), "w")
        self.results = []
        self.device = Device(args.port, self.log)
        self.emitter = None
        self.watcher = None

    # --- output -----------------------------------------------------------------

    def log(self, text):
        stamped = f"{time.strftime('%X')} {text}"
        self.log_file.write(stamped + "\n")
        self.log_file.flush()
        if self.args.verbose or not text.startswith("  "):
            print(stamped, flush=True)

    def check(self, name, passed, measured):
        verdict = "PASS" if passed else "FAIL"
        self.results.append((verdict, name, measured))
        self.log(f"{verdict} {name}: {measured}")

    # --- the owner's phone, played by this computer --------------------------------

    def start_emitter(self):
        if self.emitter is None:
            self.emitter = subprocess.Popen(
                [self.args.ble_python, os.path.join(HERE, "emit.py"), PROBE_CODE, "--seconds", "3600"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.log("emitter on")

    def stop_emitter(self):
        if self.emitter is not None:
            self.emitter.terminate()
            self.emitter.wait()
            self.emitter = None
            self.log("emitter off")

    def start_watcher(self):
        self.watcher_log = os.path.join(self.args.out, "monitor.log")
        self.watcher = subprocess.Popen(
            [self.args.ble_python, "-u", os.path.join(HERE, "monitor.py"), "--seconds", "7200"],
            stdout=open(self.watcher_log, "w"), stderr=subprocess.STDOUT)

    def found_addresses(self):
        """One address per found session, in order. macOS sometimes pauses
        reporting a device mid-broadcast, so monitor.py can log a second "FOUND
        started" for the same session: repeats of the previous address merge."""
        sessions = []
        with open(self.watcher_log) as f:
            for line in f:
                if "FOUND started" in line:
                    address = line.split()[-1]
                    if not sessions or sessions[-1] != address:
                        sessions.append(address)
        return sessions

    # --- device steps ----------------------------------------------------------------

    def sleep_device(self):
        """Puts the device to sleep. FIND_SLEEP is a loop() command: a device in
        the middle of a broadcast first gets a virtual press to boot normally."""
        for _ in range(2):
            sent_at = time.time()
            self.device.send("FIND_SLEEP")
            if self.device.wait_for(r"FIND_SLEEP_OK", 5, since=sent_at):
                return True
            self.press()
        return False

    def wait_for_found_mode(self, timeout_s):
        """Waits for a wake that hears the code; the device then stays up for the broadcast."""
        # "Code heard" prints before the port reopens and is often missed; the
        # FIND_AWAKE heartbeat only keeps coming while a broadcast runs (a quiet
        # wake is over in under a second).
        since = time.time()
        end = since + timeout_s
        while time.time() < end:
            if self.device.wait_for(r"Code heard, broadcasting", 0.5, since=since):
                return True
            first = self.device.wait_for(r"^FIND_AWAKE ", max(0.1, end - time.time()), since=since)
            if first is None:
                return False
            if self.device.wait_for(r"^FIND_AWAKE ", 3, since=time.time()):
                return True
        return False

    def wait_for_boot(self, since, timeout_s):
        """A normal boot (after a crash, a press or the power button) since `since`.

        "[MAIN] Device:" prints only in setup(); "Entering activity" also prints on
        the way to sleep.
        """
        return self.device.wait_for(r"\[MAIN\] Device:", timeout_s, since=since) is not None

    def press(self):
        """A virtual power press, repeated until the device hands over to a normal boot."""
        pressed_at = time.time()
        for _ in range(20):
            self.device.send("FIND_PRESS")
            if self.device.wait_for(r"Found by the owner, booting|Power button while listening", 1.0, since=pressed_at):
                return self.wait_for_boot(pressed_at, 30)
        return False

    def clear_journal(self):
        self.device.send("FIND_JOURNAL_CLEAR")
        return self.device.wait_for(r"FIND_JOURNAL_CLEARED", 10) is not None

    # --- scenarios ---------------------------------------------------------------------

    def scenario_quiet_and_heard(self):
        """Quiet wakes, the interval, then a wake that hears the code."""
        self.clear_journal()
        self.check("device answers FIND_STATE", self.device.state() is not None, "FIND_STATE line")
        self.check("sleep on command", self.sleep_device(), "FIND_SLEEP_OK")

        time.sleep(2 * INTERVAL_S + 10)  # two quiet wakes
        self.start_emitter()
        self.check("wake hears the code", self.wait_for_found_mode(INTERVAL_S + 20), "Code heard")

        journal = self.device.journal() or []
        listens = [e for e in journal if e[1] == EV_LISTEN_END]
        quiet = [e[3] for e in listens if e[2] == NOTHING_HEARD]
        heard = [e[3] for e in listens if e[2] == HEARD]
        self.check("two quiet wakes recorded", len(quiet) >= 2, f"{len(quiet)} quiet wakes")
        if quiet:
            self.check(f"quiet wake done by {QUIET_WAKE_MAX_MS} ms", max(quiet) <= QUIET_WAKE_MAX_MS,
                       f"slowest {max(quiet)} ms after boot")
        if heard:
            self.check(f"code heard by {HEARD_WAKE_MAX_MS} ms", heard[-1] <= HEARD_WAKE_MAX_MS,
                       f"{heard[-1]} ms after boot")
        boots = [e[0] for e in journal if e[1] == EV_BOOT and e[3] == 1]
        gaps = [(b - a) / 1000 for a, b in zip(boots, boots[1:])]
        if gaps:
            worst = max(abs(g - INTERVAL_S) for g in gaps)
            self.check(f"timer wakes {INTERVAL_S} +/- {INTERVAL_TOLERANCE_S} s apart", worst <= INTERVAL_TOLERANCE_S,
                       "gaps " + ", ".join(f"{g:.1f}" for g in gaps) + " s")
        screens = [e for e in journal if e[1] == EV_FOUND_SCREEN]
        self.check("found screen drawn once", len(screens) == 1, f"{len(screens)} draws")

    def scenario_press_and_mute(self):
        """A press in found mode hands over and mutes; the mute holds, then re-arms."""
        self.check("press in found mode boots", self.press(), "Found by the owner + normal boot")
        state = self.device.state() or {}
        self.check("press mutes the code", state.get("muted") == 1, f"muted={state.get('muted')}")

        self.clear_journal()
        self.check("sleep with the phone still on", self.sleep_device(), "FIND_SLEEP_OK")
        time.sleep(INTERVAL_S + 10)  # a muted wake: hears the code, stays silent
        self.stop_emitter()
        time.sleep(INTERVAL_S)  # a quiet wake: re-arms
        self.start_emitter()
        self.check("re-armed wake broadcasts again", self.wait_for_found_mode(INTERVAL_S + 20), "Code heard")

        journal = self.device.journal() or []
        results = [e[2] for e in journal if e[1] == EV_LISTEN_END]
        broadcasts = [e for e in journal if e[1] == EV_BROADCAST_END]
        self.check("muted wake stays silent", results[:1] == [HEARD] and len(broadcasts) == 0,
                   f"listen results {results}, {len(broadcasts)} broadcasts before the re-armed one")
        self.check("quiet wake re-arms", NOTHING_HEARD in results, f"listen results {results}")

    def scenario_phone_gone_and_address(self):
        """Found mode lasts while the phone calls and stops soon after it goes quiet;
        each search has a new address."""
        # A quiet wake first, so a mute left by an earlier press is cleared.
        self.stop_emitter()
        self.check("sleep for the phone-gone run", self.sleep_device(), "FIND_SLEEP_OK")
        time.sleep(INTERVAL_S + 10)
        self.start_emitter()
        self.check("found session starts", self.wait_for_found_mode(INTERVAL_S + 20), "Code heard")
        time.sleep(INTERVAL_S + 10)  # past the old fixed 60 s broadcast
        still_up = self.device.wait_for(r"^FIND_AWAKE ", 5, since=time.time()) is not None
        self.check("broadcast continues while the phone calls", still_up, "heartbeat after 70 s")
        stopped_at = time.time()
        self.stop_emitter()
        ended = self.device.wait_for(r"Radio stopped", PHONE_GONE_S + 30, since=stopped_at)
        self.check("broadcast ends after emit.py stops", ended is not None, f"{time.time() - stopped_at:.1f} s")
        time.sleep(3)
        self.start_emitter()
        self.check("next session starts", self.wait_for_found_mode(INTERVAL_S + 20), "Code heard")
        journal = self.device.journal() or []
        gone = [e for e in journal if e[1] == EV_BROADCAST_END and e[2] == PHONE_GONE]
        self.check("broadcast ended as PhoneGone", bool(gone), f"{len(gone)} PhoneGone ends")
        quiet = [e[3] for e in journal if e[1] == EV_BROADCAST_QUIET]
        self.check(f"it ends {PHONE_GONE_S} s after the last packet heard",
                   bool(quiet) and abs(quiet[-1] - PHONE_GONE_S * 1000) <= PHONE_GONE_TOLERANCE_MS,
                   f"{quiet[-1]} ms" if quiet else "no BroadcastQuiet entry")
        # The watcher needs a moment to see the session that just started.
        addresses = self.found_addresses()
        for _ in range(20):
            if len(addresses) >= 2:
                break
            time.sleep(0.5)
            addresses = self.found_addresses()
        self.check("a new address per found session", len(addresses) >= 2 and len(set(addresses)) == len(addresses),
                   f"{len(set(addresses))} distinct of {len(addresses)} sessions")
        self.press()

    def inject_and_sleep(self, injection):
        self.device.send(f"FIND_INJECT {injection}")
        ok = self.device.wait_for(r"FIND_INJECT_OK", 10) is not None
        return ok and self.sleep_device()

    def scenario_radio_failure_keeps_mute(self):
        state = self.device.state() or {}
        self.check("muted before the radio failure", state.get("muted") == 1, f"muted={state.get('muted')}")
        self.stop_emitter()
        self.clear_journal()
        self.check("inject radiofail", self.inject_and_sleep("radiofail"), "FIND_INJECT_OK + sleep")
        time.sleep(INTERVAL_S + 10)
        self.start_emitter()
        # The failed wake must not clear the mute: the next wake hears the code and stays silent.
        time.sleep(INTERVAL_S + 10)
        self.stop_emitter()
        time.sleep(INTERVAL_S)  # a real quiet wake clears it
        self.start_emitter()
        self.check("found again after a real quiet wake", self.wait_for_found_mode(INTERVAL_S + 20), "Code heard")
        journal = self.device.journal() or []
        results = [e[2] for e in journal if e[1] == EV_LISTEN_END]
        self.check("radio failure keeps the mute", results[:2] == [RADIO_FAILED, HEARD], f"last listens {results}")
        self.press()

    def scenario_crashes_switch_off(self):
        self.device.send("FIND_RETRY")  # start from a zero crash count
        self.device.wait_for(r"FIND_RETRY_OK", 10)
        for n in range(1, 4):
            slept_at = time.time()
            self.check(f"inject crash {n}", self.inject_and_sleep("crash"), "FIND_INJECT_OK + sleep")
            booted = self.wait_for_boot(slept_at, INTERVAL_S + 40)
            state = self.device.state() or {}
            self.check(f"crash {n} counted", booted and state.get("crashes") == n, f"crashes={state.get('crashes')}")
        state = self.device.state() or {}
        self.check("three crashes switch the mode off", state.get("switchedOff") == 1 and state.get("armed") == 0,
                   f"switchedOff={state.get('switchedOff')} armed={state.get('armed')}")
        self.device.send("FIND_RETRY")
        self.device.wait_for(r"FIND_RETRY_OK", 10)
        state = self.device.state() or {}
        self.check("retry re-arms", state.get("armed") == 1 and state.get("crashes") == 0,
                   f"armed={state.get('armed')} crashes={state.get('crashes')}")

    def scenario_hang_guard(self):
        slept_at = time.time()
        self.check("inject hang", self.inject_and_sleep("hang"), "FIND_INJECT_OK + sleep")
        booted = self.wait_for_boot(slept_at, INTERVAL_S + 40)
        journal = self.device.journal() or []
        inject = [e[0] for e in journal if e[1] == EV_INJECT]
        panic_boot = [e[0] for e in journal if e[1] == EV_BOOT and e[2] == ESP_RST_PANIC]
        if booted and inject and panic_boot:
            waited = (panic_boot[-1] - inject[-1]) / 1000
            self.check(f"guard aborts a hang within {HANG_GUARD_MAX_S} s", waited <= HANG_GUARD_MAX_S, f"{waited:.1f} s")
        else:
            self.check("guard aborts a hang", False, f"booted={booted} inject={len(inject)} panic={len(panic_boot)}")
        state = self.device.state() or {}
        self.check("hang counted as a crash", state.get("crashes") == 1, f"crashes={state.get('crashes')}")
        self.device.send("FIND_RETRY")
        self.device.wait_for(r"FIND_RETRY_OK", 10)

    def scenario_found_screens(self):
        """The found screen in each language and orientation, saved as PNG."""
        try:
            from PIL import Image
        except ImportError:
            self.check("screenshots", False, "Pillow missing")
            return
        languages = language_ids(["en", "es"])
        for code, language in languages.items():
            for orientation, name in enumerate(["portrait", "landscape-cw", "portrait-inverted", "landscape-ccw"]):
                self.device.send(f"FIND_PREVIEW {language} {orientation}")
                drawn = self.device.wait_for(r"FIND_PREVIEW_OK", 15) is not None
                self.device.send("SCREENSHOT")
                try:
                    raw = self.device.screens.get(timeout=15)
                except queue.Empty:
                    raw = None
                path = os.path.join(self.args.out, f"found-{code}-{name}.png")
                if drawn and raw:
                    image = Image.frombytes("1", (800, 480), raw).transpose(Image.ROTATE_270)
                    image.save(path)
                self.check(f"found screen {code} {name}", drawn and raw is not None, path)

    def scenario_low_battery_and_button(self):
        """Below the minimum the device sleeps until the power button: one real press."""
        self.stop_emitter()
        slept_at = time.time()
        self.check("inject battery 5", self.inject_and_sleep("battery 5"), "FIND_INJECT_OK + sleep")
        time.sleep(INTERVAL_S + 10)
        self.log(f"ACTION: wait {2 * INTERVAL_S} s, then press the power button once (devkit: GPIO3 to GND, ~1 s)")
        time.sleep(2 * INTERVAL_S)
        self.log("ACTION: press the power button now")
        booted = self.wait_for_boot(slept_at + INTERVAL_S + 10, 600)
        journal = self.device.journal() or []
        stops = [e for e in journal if e[1] == EV_SLEEP and e[2] == 1 and e[3] == 0]
        boots = [e for e in journal if e[1] == EV_BOOT]
        last_boot_timer = boots[-1][3] if boots else None
        self.check("low battery sleeps until the button", bool(stops), f"{len(stops)} sleep-until-button entries")
        self.check("no timer wake while stopped", booted and last_boot_timer == 0,
                   f"last boot was a {'timer' if last_boot_timer else 'button'} wake")

    # --- run ---------------------------------------------------------------------------

    def run(self):
        self.log(f"waiting for the device on {self.args.port}")
        end = time.time() + 30
        while not self.device.port_is_up() and time.time() < end:
            time.sleep(0.2)
        self.start_watcher()
        scenarios = [
            self.scenario_found_screens,
            self.scenario_quiet_and_heard,
            self.scenario_press_and_mute,
            self.scenario_phone_gone_and_address,
            self.scenario_radio_failure_keeps_mute,
            self.scenario_crashes_switch_off,
            self.scenario_hang_guard,
        ]
        if not self.args.skip_button:
            scenarios.append(self.scenario_low_battery_and_button)
        only = set(self.args.only.split(",")) if self.args.only else None
        try:
            for scenario in scenarios:
                if only and scenario.__name__.replace("scenario_", "") not in only:
                    continue
                self.log(f"== {scenario.__name__.replace('scenario_', '')}")
                scenario()
        finally:
            self.stop_emitter()
            if self.watcher:
                self.watcher.terminate()
            self.device.running = False
        passed = sum(1 for r in self.results if r[0] == "PASS")
        with open(os.path.join(self.args.out, "report.txt"), "w") as f:
            for verdict, name, measured in self.results:
                f.write(f"{verdict} {name}: {measured}\n")
            f.write(f"\n{passed}/{len(self.results)} checks passed\n")
        self.log(f"{passed}/{len(self.results)} checks passed; report in {self.args.out}/report.txt")
        return passed == len(self.results)


def language_ids(codes):
    """Language enum values by code, read from the generated lib/I18n/I18nKeys.h."""
    ids = {}
    with open(os.path.join(REPO, "lib", "I18n", "I18nKeys.h")) as f:
        for line in f:
            match = re.match(r"\s*\w+\s*=\s*(\d+),\s*//\s*([\w-]+)", line)
            if match and match.group(2) in codes:
                ids[match.group(2)] = int(match.group(1))
    return ids


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", default="/dev/cu.usbmodem*", help="serial port (glob allowed)")
    parser.add_argument("--out", default="findmode-suite", help="directory for the log, report and screenshots")
    parser.add_argument("--ble-python", default=sys.executable, help="Python that has bless and bleak installed")
    parser.add_argument("--only", help="comma-separated scenario names to run")
    parser.add_argument("--skip-button", action="store_true", help="skip the scenario that needs a power press")
    parser.add_argument("--verbose", action="store_true", help="echo every device line")
    args = parser.parse_args()
    sys.exit(0 if Suite(args).run() else 1)


if __name__ == "__main__":
    main()
