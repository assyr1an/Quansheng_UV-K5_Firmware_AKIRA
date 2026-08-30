## What this changes

<!-- One paragraph. What, and why. Link the issue if one exists. -->

## Checklist

- [ ] Builds clean in the container (`-Werror`, so it either does or doesn't)
- [ ] **Flash delta measured and stated below** (`arm-none-eabi-size firmware`, before → after)
- [ ] Does not touch the wire format, crypto construction, or EEPROM identity layout
      (frozen until 1.0.0 — or an issue is linked where this was agreed)
- [ ] If `helper/v2frame.c`, `helper/poly1305.c` or `external/chacha/chacha.c` changed:
      `bash tools/v2-vectors-test/run.sh` passes, output included
- [ ] Adds no code path that keys the transmitter from the host
- [ ] Tested on hardware (state radio model + what you observed), or explicitly marked build-only

## Size

```
text before:
text after:
delta:
```
