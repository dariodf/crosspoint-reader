"""Collect how long the reader takes to hear a phone, to size the listen window.

    python3 scripts/findmode/collect_detections.py --port '/dev/cu.usbmodem*' --count 20

Needs a find-mode test build (CROSSPOINT_FIND_MODE_TEST_HOOKS), ideally with
CROSSPOINT_FIND_MODE_TEST_BROADCAST_SECONDS=10 so it cycles every ~70 s, and a
phone advertising the find code the whole time (nRF Connect, LightBlue).

Every timer wake that hears the code starts a broadcast, which keeps the
device awake long enough to read its journal. For each wake the script prints
the detection latency: time from the scan starting to the code being heard.
Wakes that did not hear the phone count as misses. At the end it prints the
slowest detection and a listen window of 1.5 times it, plus the detection rate
(the listen window holds at 19 or more of 20).
"""

import argparse
import math
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_suite import EV_LISTEN_END, HEARD, NOTHING_HEARD, Device  # noqa: E402

EV_LISTEN_START = 9


def wakes_in(journal):
    """(listen_start_ms, listen_end_ms, result, end_rtc_ms) per wake, from ListenStart/ListenEnd pairs."""
    wakes = []
    start = None
    for rtc_ms, event, detail, value in journal:
        if event == EV_LISTEN_START:
            start = value
        elif event == EV_LISTEN_END and start is not None:
            wakes.append((start, value, detail, rtc_ms))
            start = None
    return wakes


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", default="/dev/cu.usbmodem*")
    parser.add_argument("--count", type=int, default=20, help="detections to collect")
    parser.add_argument("--timeout-min", type=int, default=60)
    parser.add_argument("--out", help="file to append results to")
    args = parser.parse_args()

    out = open(args.out, "a") if args.out else None

    def say(text):
        line = f"{time.strftime('%X')} {text}"
        print(line, flush=True)
        if out:
            out.write(line + "\n")
            out.flush()

    device = Device(args.port, lambda text: None)
    seen = set()
    first_dump = True
    latencies = []
    misses = 0
    end = time.time() + 60 * args.timeout_min
    say(f"collecting {args.count} detections; keep the phone advertising")
    while len(latencies) < args.count and time.time() < end:
        if device.wait_for(r"^FIND_AWAKE ", 30) is None:
            continue
        journal = device.journal()
        if not journal:
            continue
        wakes = wakes_in(journal)
        if first_dump:
            # The journal survives flashing and earlier runs: only the wake
            # under way now belongs to this run.
            seen.update(rtc_ms for _, _, _, rtc_ms in wakes[:-1])
            first_dump = False
        for start, stop, result, rtc_ms in wakes:
            if rtc_ms in seen or start == 0:
                continue
            seen.add(rtc_ms)
            if result == HEARD:
                latencies.append(stop - start)
                say(f"detection {len(latencies)}: heard {stop - start} ms after the scan started")
            elif result == NOTHING_HEARD:
                misses += 1
                say(f"miss: a full {stop - start} ms listen heard nothing")
        time.sleep(12)  # past this broadcast, so the next dump sees the next wake

    device.running = False
    if not latencies:
        say("no detections")
        return 1
    ordered = sorted(latencies)
    slowest = ordered[-1]
    rate = f"{len(latencies)}/{len(latencies) + misses}"
    say(f"detections {rate}; latency min {ordered[0]} ms, median {ordered[len(ordered) // 2]} ms, max {slowest} ms")
    say(f"suggested LISTEN_MS = {math.ceil(1.5 * slowest / 10) * 10} (1.5 x slowest)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
