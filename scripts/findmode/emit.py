"""Advertise a find-mode code from a computer, standing in for the owner's phone.

    python3 scripts/findmode/emit.py c0de0001-f1d0-4b1e-9a5e-000000000001 --seconds 600

Needs `pip install bless` and Bluetooth permission for the terminal
(System Settings > Privacy & Security > Bluetooth).

On macOS (CoreBluetooth), a 128-bit service UUID fits into the advertisement
only without a local name: bless drops the UUIDs when the name is longer than 10 characters, and
flags + name + UUID exceed the 31-byte packet. So the name stays empty.
Verified with a phone scanner (nRF Connect): 02011A 1107 <UUID, byte-reversed>.
"""

import argparse
import asyncio

from bless import BlessServer, GATTAttributePermissions, GATTCharacteristicProperties


async def advertise(code: str, seconds: int) -> None:
    server = BlessServer(name="")
    await server.add_new_service(code)
    # CoreBluetooth advertises only services that have a characteristic.
    await server.add_new_characteristic(
        code,
        "c0de0002-f1d0-4b1e-9a5e-000000000001",
        GATTCharacteristicProperties.read,
        bytearray(b"x"),
        GATTAttributePermissions.readable,
    )
    await server.start(prioritize_local_name=False)
    print(f"advertising {code} for {seconds} s", flush=True)
    await asyncio.sleep(seconds)
    await server.stop()
    print("stopped", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("code", help="the find-mode code as UUID text")
    parser.add_argument("--seconds", type=int, default=600)
    args = parser.parse_args()
    asyncio.run(advertise(args.code, args.seconds))


if __name__ == "__main__":
    main()
