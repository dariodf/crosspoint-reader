"""Watch for a reader in found mode ("CP-FIND") and print each sighting.

    python3 scripts/findmode/monitor.py --seconds 600

Needs `pip install bleak`. Prints one line per second while CP-FIND is heard
(time and RSSI in dBm), and "FOUND started" / "FOUND stopped" lines around
each stretch of sightings, so a log shows when found mode began and ended.

On macOS, CoreBluetooth sometimes holds back repeated packets from the same device for a few
seconds. A gap longer than GONE_AFTER_S then shows as a stop followed by a
new start while the reader is still broadcasting; only the reader's own log
says when found mode really ended.
"""

import argparse
import asyncio
import time

from bleak import BleakScanner

FOUND_NAME = "CP-FIND"
# Seconds without a CP-FIND packet before the reader counts as gone.
GONE_AFTER_S = 3.0


async def watch(seconds: int) -> None:
    last_seen = 0.0
    last_printed = 0.0

    def on_advertisement(device, advertisement) -> None:
        nonlocal last_seen, last_printed
        if advertisement.local_name != FOUND_NAME:
            return
        now = time.time()
        if last_seen == 0.0 or now - last_seen > GONE_AFTER_S:
            print(f"{time.strftime('%X')} FOUND started {device.address}", flush=True)
        last_seen = now
        if now - last_printed >= 1.0:
            print(f"{time.strftime('%X')} {FOUND_NAME} rssi={advertisement.rssi}", flush=True)
            last_printed = now

    print(f"{time.strftime('%X')} watching for {FOUND_NAME} for {seconds} s", flush=True)
    async with BleakScanner(on_advertisement):
        end = time.time() + seconds
        while time.time() < end:
            await asyncio.sleep(0.5)
            if last_seen and time.time() - last_seen > GONE_AFTER_S:
                print(f"{time.strftime('%X')} FOUND stopped", flush=True)
                last_seen = 0.0
    print(f"{time.strftime('%X')} done", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--seconds", type=int, default=600)
    args = parser.parse_args()
    asyncio.run(watch(args.seconds))


if __name__ == "__main__":
    main()
