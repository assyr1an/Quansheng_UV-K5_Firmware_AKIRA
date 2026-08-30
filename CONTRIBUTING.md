# Contributing to AKIRA

Thanks for the interest. This is a small, security-focused firmware project with one
maintainer; the rules below exist so that reviews are fast and nothing unsafe lands.

## Ground rules

1. **The wire format is frozen until 1.0.0.** The messenger protocol
   ([`docs/PROTOCOL.md`](docs/PROTOCOL.md)) is implemented and host-verified but has not yet
   passed its bench validation on a pair of radios. Until it does, PRs that change the frame
   format, the crypto construction, or the EEPROM identity layout will not be merged —
   **open an issue first** and make the argument there.
2. **Nothing may key the transmitter from the host.** No UART command, no tool, no code path
   that initiates TX from a PC. PRs adding one will be closed regardless of usefulness — an
   automated trigger on a live transmitter is a hazard by design, not an oversight.
3. **Flash is the constraint.** The application budget is 61,440 bytes and 0.9 ships with
   **768 bytes free**. Every PR that adds code must state its measured size delta
   (`arm-none-eabi-size firmware`, before and after). "It's small" is not a measurement.
4. **Crypto changes carry proof.** Anything touching `helper/v2frame.c`, `helper/poly1305.c`
   or `external/chacha/chacha.c` must pass `bash tools/v2-vectors-test/run.sh` — it compiles
   the firmware's own crypto for the host and checks it byte-for-byte against known-answer
   vectors. Include the output in the PR.

## Building and testing

Everything is in [`docs/BUILDING.md`](docs/BUILDING.md). The short version:

```sh
docker build -t akira .
docker run --rm akira /bin/bash -c "cd /app && make && arm-none-eabi-size firmware"
```

The toolchain is pinned in the container; a green local build reproduces CI. Commit before
building — the version string on the radio comes from `git describe`.

If your change affects radio behaviour, say what you tested it on (radio model, firmware
version it upgraded from) and what you observed. "Compiles" and "works on my radio" are
different claims; make whichever one is true.

## Style

Match the surrounding code. The tree builds with `-Werror`; warnings are errors, so your PR
either builds clean or does not build. Comments explain *why*, not *what* — and if you fixed
something subtle, say what it cost you to find, so the next person doesn't pay it again.

## Reporting security issues

Please **do not** open a public issue for an exploitable weakness — see the reporting section
in [`docs/SECURITY.md`](docs/SECURITY.md). Findings against the threat model or the protocol
design (as opposed to exploitable bugs) are welcome as ordinary issues; adversarial reading
is exactly what a pre-1.0 crypto protocol needs.

## What is most useful right now

- Results from the [27-test bench list](docs/PROTOCOL.md#bench-validation--the-27-tests-between-09-and-100)
  on your own pair of radios — this is the single highest-value contribution possible.
- Review of `docs/PROTOCOL.md` and `docs/SECURITY.md` against the implementation.
- Testing on UV-K5(8) and UV-K6 hardware variants.
- Flash-size reductions with measured numbers.
