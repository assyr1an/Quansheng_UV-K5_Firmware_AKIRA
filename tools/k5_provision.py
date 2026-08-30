"""
Provision a radio with its protocol v2 identity - key, sender_id, counter.

    # first radio: mint a new key and save it
    python k5_provision.py --port COM22 --new-key --keyfile v2-key.json

    # second radio: SAME key, a DIFFERENT sender_id (minted automatically)
    python k5_provision.py --port COM22 --keyfile v2-key.json

    # look, change nothing
    python k5_provision.py --port COM22 --show

WHY THE KEY IS MINTED HERE AND NOT ON THE RADIO
The radio has no entropy source worth trusting: its noise-derived bytes measure 52.6% bit bias and roughly 56 usable bits
per 96 - deficient, not catastrophic, but nowhere near enough to mint a 256-bit
key. The host has a real CSPRNG, so the key is generated here with
`secrets.token_bytes` and written over the cable. That is also why protocol v2
has no KDF at all: a key that is already 256 uniform bits has nothing to derive
(docs/PROTOCOL.md, Keys).

WHERE IT GOES - EEPROM 0x1D00, and nowhere else

    0x1D00  32  K_master
    0x1D20   4  sender_id   per radio, must differ between radios
    0x1D24   4  counter     reserved in blocks of 64 by the firmware

0x1D00 is the only gap in the whole EEPROM that a CHIRP upload cannot clobber:
the driver's PROG_SIZE is 0x1d00, so it reads all 0x2000 but writes only below
that (tools/chirp-driver/uvk5_egzumer.py:445). Verified writable on hardware
2026-08-27.

SENDER_ID MUST BE UNIQUE PER RADIO. The nonce is sender_id||counter||type; two
radios sharing a sender_id will reuse nonces, and ChaCha20 nonce reuse leaks the
XOR of two plaintexts and destroys the Poly1305 one-time key. This tool refuses
to issue a sender_id already recorded in the keyfile for a different radio.

The key crosses a UART link that is obfuscated, not encrypted. That is fine
here - the threat model is physical capture, not someone sitting on the
programming cable (docs/SECURITY.md, Limitations).
"""

import argparse
import json
import os
import secrets
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from k5_protocol import K5Link  # noqa: E402

KEY_ADDR = 0x1D00
KEY_LEN = 32
IDENTITY_ADDR = 0x1D20      # sender_id(4) + counter(4), one 8-byte page


def load_keyfile(path):
    if not os.path.exists(path):
        return None
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def save_keyfile(path, data):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    print("  keyfile written: " + path)
    print("  KEEP THIS FILE. It is the only copy of the key, and the second")
    print("  radio needs the identical key to talk to the first.")


def mask(hexstr, reveal):
    return hexstr if reveal else hexstr[:4] + "..." + hexstr[-4:] + "  (use --show-secret for the full key)"


def show(link, reveal=False):
    key = link.read_eeprom(KEY_ADDR, KEY_LEN)
    identity = link.read_eeprom(IDENTITY_ADDR, 8)
    sender_id, counter = struct.unpack("<II", identity)

    blank_key = all(b == 0xFF for b in key)
    print("  key      0x1D00  " +
          ("UNPROVISIONED (all 0xFF)" if blank_key else mask(key.hex(), reveal)))
    print("  sender   0x1D20  0x%08X%s" %
          (sender_id, "  UNPROVISIONED" if sender_id in (0xFFFFFFFF, 0) else ""))
    print("  counter  0x1D24  %d%s" %
          (counter, "  UNPROVISIONED" if counter == 0xFFFFFFFF else ""))
    return key, sender_id, counter


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--keyfile", default="v2-key.json",
                    help="where the shared key and the sender_ids already "
                         "issued are recorded")
    ap.add_argument("--new-key", action="store_true",
                    help="mint a fresh 256-bit key. Do this ONCE, for the "
                         "first radio; every other radio in the group reuses "
                         "the keyfile")
    ap.add_argument("--sender-id", type=lambda s: int(s, 0),
                    help="force a specific sender_id instead of minting one")
    ap.add_argument("--show-secret", action="store_true",
                    help="print the full key hex instead of a masked form")
    ap.add_argument("--show", action="store_true",
                    help="read the identity back and change nothing")
    ap.add_argument("--label", default="",
                    help="a name for this radio, recorded in the keyfile")
    ap.add_argument("--yes", action="store_true", help="skip the confirmation")
    args = ap.parse_args()

    try:
        import serial
    except ImportError:
        print("pyserial is required:  pip install pyserial")
        return 2

    with K5Link(args.port, serial) as link:
        info = link.hello()
        print("radio: %s\n" % info["version"])

        print("current state:")
        _cur_key, cur_sender, cur_counter = show(link, args.show_secret)

        if args.show:
            return 0

        store = load_keyfile(args.keyfile)

        # ---- decide the key ------------------------------------------------
        if args.new_key:
            if store is not None:
                print("\nREFUSING: %s already exists." % args.keyfile)
                print("--new-key would replace the group key and orphan every")
                print("radio already provisioned from it. Delete the file")
                print("deliberately if that is really what you want.")
                return 1
            key = secrets.token_bytes(KEY_LEN)
            store = {"key_hex": key.hex(), "radios": []}
            print("\nminted a new 256-bit key from the host CSPRNG")
        else:
            if store is None:
                print("\nno keyfile at %s." % args.keyfile)
                print("Use --new-key for the FIRST radio; every radio after")
                print("that must reuse the same keyfile or they cannot talk.")
                return 1
            key = bytes.fromhex(store["key_hex"])
            if len(key) != KEY_LEN:
                print("\nkeyfile holds a %d-byte key, expected %d"
                      % (len(key), KEY_LEN))
                return 1
            print("\nusing the existing key from %s" % args.keyfile)

        # ---- decide the sender_id ------------------------------------------
        issued = set(r["sender_id"] for r in store["radios"])
        if args.sender_id is not None:
            sender_id = args.sender_id
        elif cur_sender not in (0, 0xFFFFFFFF) and cur_sender in issued:
            # Re-provisioning a radio the keyfile already knows about must not
            # silently change its identity.
            sender_id = cur_sender
            print("keeping this radio's existing sender_id 0x%08X" % sender_id)
        else:
            while True:
                sender_id = struct.unpack("<I", secrets.token_bytes(4))[0]
                if sender_id not in issued and sender_id not in (0, 0xFFFFFFFF):
                    break

        if sender_id in issued and sender_id != cur_sender:
            print("\nREFUSING: sender_id 0x%08X is already issued to another"
                  % sender_id)
            print("radio in this keyfile. Two radios sharing a sender_id reuse")
            print("nonces, which breaks the encryption outright.")
            return 1
        if sender_id in (0, 0xFFFFFFFF):
            print("\nREFUSING: 0 and 0xFFFFFFFF are the firmware's "
                  "'unprovisioned' markers.")
            return 1

        # ---- counter --------------------------------------------------------
        # Start at 1, never 0, and never go backwards. If this radio already has
        # a counter, keep it: rewinding would repeat nonces already transmitted.
        counter = 1
        if cur_counter != 0xFFFFFFFF and cur_counter >= counter:
            counter = cur_counter
            print("preserving the existing counter (%d) - rewinding it would "
                  "repeat nonces already sent" % counter)

        print("\nabout to write:")
        print("  key       %s" % mask(key.hex(), args.show_secret))
        print("  sender_id 0x%08X" % sender_id)
        print("  counter   %d" % counter)
        if not args.yes:
            if input("\nproceed? [y/N] ").strip().lower() != "y":
                print("aborted, nothing written")
                return 1

        # ---- write ----------------------------------------------------------
        link.write_eeprom(KEY_ADDR, key)
        link.write_eeprom(IDENTITY_ADDR, struct.pack("<II", sender_id, counter))

        # ---- verify by reading back -----------------------------------------
        back_key = link.read_eeprom(KEY_ADDR, KEY_LEN)
        back_id, back_counter = struct.unpack(
            "<II", link.read_eeprom(IDENTITY_ADDR, 8))

        ok = (back_key == key and back_id == sender_id
              and back_counter == counter)
        print("\nread back:")
        print("  key       %s"
              % ("MATCH" if back_key == key else "MISMATCH " + back_key.hex()))
        print("  sender_id 0x%08X %s"
              % (back_id, "MATCH" if back_id == sender_id else "MISMATCH"))
        print("  counter   %d %s"
              % (back_counter, "MATCH" if back_counter == counter else "MISMATCH"))

        if not ok:
            print("\nPROVISIONING FAILED - the radio did not store what we sent.")
            return 1

        store["radios"] = [r for r in store["radios"]
                           if r["sender_id"] != sender_id]
        store["radios"].append({"sender_id": sender_id, "label": args.label,
                                "port": args.port})
        save_keyfile(args.keyfile, store)

        print("\nprovisioned. The firmware reloads its identity on this write")
        print("(app/uart.c CMD_051D -> BOARD_EEPROM_Init), so no reboot is")
        print("needed.")
        return 0


if __name__ == "__main__":
    sys.exit(main())
