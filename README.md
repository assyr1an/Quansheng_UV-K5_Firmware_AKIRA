# AKIRA

**Custom firmware for the Quansheng UV-K5, built for household emergency preparedness.**

Authenticated, encrypted radio-to-radio messaging that works with no network, no
infrastructure and no third party — plus the monitoring features needed to know what is on
the air around you.

> **Status: 0.9 — feature complete, not yet validated over the air.**
> Everything below is built, reviewed and CI-green, and runs on real hardware. But no message
> has yet crossed between two radios: no ACK has returned, no duplicate has been suppressed,
> no retry has fired. `1.0.0` is reserved for when the 29-test bench list passes on a pair.

## What AKIRA adds

**Messenger protocol v2** — a complete replacement for the v1 wire format.

* **RFC 8439 ChaCha20-Poly1305** on every frame, with the header as additional authenticated
  data, so the version, type, sender and counter are authenticated alongside the payload.
* **No key derivation at all.** A 256-bit master key is provisioned over USB from the host's
  CSPRNG. v1 expanded a 16-byte secret with FNV-1, which collapsed the key to at most 2⁶⁴
  values regardless of input.
* **Deterministic nonces** — `sender_id ‖ counter ‖ type`. Nonces need uniqueness, not
  unpredictability, which removes any dependence on the radio's very weak hardware RNG.
* **Replay and duplicate suppression** — a per-sender counter window. A duplicate is
  re-acknowledged but never re-displayed; an older frame is dropped silently.
* **Authenticated ACKs** that name the exact message they acknowledge, so a stale ACK cannot
  confirm a different transaction.
* **Automatic retry** by byte-identical retransmission, consuming no additional nonce.
* **A different FSK sync word from v1**, so the two protocols are invisible to each other at
  the hardware layer.

**Security and monitoring**

* **Two-gesture panic wipe** — one press clears every plaintext buffer; a confirmed second
  press destroys the master key in EEPROM as well as RAM. It works from the main screen *and*
  from inside the spectrum analyser.
* **On-radio key fingerprint** (`KeyID`) so two operators can confirm they hold the same key
  with no computer present.
* **Priority-channel scanning** with a configurable interval, **scan-hit auto-store**, a
  **bounded activity log**, and a **16-entry message ring** with paging.
* **Host tooling** — EEPROM backup and verification, firmware flashing, key provisioning,
  channel programming, a self-test, and a safety gate that refuses to let a write proceed if
  factory RF calibration has changed.

**Removed on purpose:** AirCopy (it clones the encryption key over the air, and would clone the
per-radio sender identity into nonce reuse) and the v1 `EncKey` / `MsgEnc` menus (they edited a
secret protocol v2 does not read).

## Lineage and licence

AKIRA is a fork of [kamilsss655/uv-k5-firmware-custom](https://github.com/kamilsss655/uv-k5-firmware-custom)
(the "nunu" firmware), which is itself a fork of
[Egzumer](https://github.com/egzumer/uv-k5-firmware-custom), derived in turn from
[joaquimorg](https://github.com/joaquimorg) and
[DualTachyon](https://github.com/DualTachyon)'s open re-implementation of the stock firmware.

Licensed under the Apache License 2.0. Copyright notices in inherited files belong to their
original authors and are unchanged; files original to AKIRA carry their own. **The vast
majority of this radio's behaviour — the RF driver, the UI, the spectrum analyser, the scanner
— is upstream work, and AKIRA would not exist without it.**

Forked at upstream v.20.5. Note that the mesh "NUNU Protocol" advertised by upstream landed in
v.21.0 and is **not** present in this tree — AKIRA's messenger is a flat broadcast group.

> [!WARNING]
> By using the firmware provided, users are responsible for ensuring compliance with all local laws and regulations governing the use of such technology. The author of the firmware shall not be held liable for any misuse or unlawful activities conducted by the user. It is the user's sole responsibility to use the firmware in a legal and responsible manner. By proceeding to use the firmware, users agree to abide by all applicable laws and regulations.
The firmware provided does not come with any form of warranty, express or implied. Users understand that there is a risk that the firmware may potentially brick their radio device. The author of the firmware shall not be held responsible for any damage caused to the user's radio device as a result of using the firmware. By proceeding to use the firmware, users agree to do so at their own risk and understand that the author will not be liable for any adverse consequences that may arise. It is the user's responsibility to proceed with caution and understand the potential risks involved in using the firmware.

<sub>Upstream documentation follows. Some of it describes features AKIRA has changed or removed;
where the two disagree, this file's AKIRA sections are correct for this tree.</sub>

---

## Table of Contents
* [Videos](#videos)
* [Main Features](#main-features)
* [Manual](#manual)
* [Radio Performance](#radio-performance)
* [User Customization](#user-customization)
* [Compiler](#compiler)
* [Building](#building)
* [Credits](#credits)
* [How to contribute](#how-to-contribute)
* [License](#license)

---
## Videos:

Video showing FM reception in spectrum channel mode:

https://github.com/kamilsss655/uv-k5-firmware-custom/assets/8842065/a30c1031-a4c9-4021-8c44-c7532e354d27

Video showing AM reception in spectrum channel mode:

https://github.com/kamilsss655/uv-k5-firmware-custom/assets/8842065/638a9985-b808-4cbf-9b98-628df64dba46

Video showing FM reception in spectrum channel mode:

https://github.com/kamilsss655/uv-k5-firmware-custom/assets/8842065/a106f5cb-8794-4149-9cbb-2f672c0c5b39

Video showing AM reception in VFO mode:

[![Video showing AM reception in VFO mode](https://img.youtube.com/vi/GOZYdKd8wng/0.jpg)](https://www.youtube.com/shorts/GOZYdKd8wng)

Video showing SSB reception with a CB antenna mounted on the roof of a car:

https://github.com/kamilsss655/uv-k5-firmware-custom/assets/8842065/d0e931ba-90ca-4644-b48e-4aace1d1842c

Video of the NUNU Protocol enabling message hopping (available in v.21.0):

https://github.com/user-attachments/assets/c9113fb5-de3a-47fe-88d5-c8b49d40b273

https://github.com/kamilsss655/uv-k5-firmware-custom/assets/8842065/5392ce36-9308-4a54-a92c-a82474b4b0d5

Video of the NUNU Protocol with 4 radios:

https://github.com/kamilsss655/uv-k5-firmware-custom/assets/8842065/078dca54-b9aa-4930-bf19-0529c1c1791b

## Main features:
* many of OneOfEleven mods:
   * AM fix, huge improvement in reception quality
   * long press buttons functions replicating F+ action
   * fast scanning
   * channel name editing in the menu
   * channel name + frequency display option
   * shortcut for scan-list assignment (long press `5 NOAA`)
   * scan-list toggle (long press `* Scan` while scanning)
   * configurable button function selectable from menu
   * battery percentage/voltage on status bar, selectable from menu
   * longer backlight times
   * mic bar
   * RSSI s-meter
   * more frequency steps
   * squelch more sensitive
* fagci spectrum analyzer (**F+5** to turn on)
* some other mods introduced by egzumer:
   * SSB demodulation (adopted from fagci)
   * backlight dimming
   * battery voltage calibration from menu
   * better battery percentage calculation, selectable for 1600mAh or 2200mAh
   * more configurable button functions
   * long press MENU as another configurable button
   * better DCS/CTCSS scanning in the menu (`* SCAN` while in RX DCS/CTCSS menu item)
   * Piotr022 s-meter style
   * restore initial freq/channel when scanning stopped with EXIT, remember last found transmission with MENU button
   * reordered and renamed menu entries
   * LCD interference crash fix
* joaquimorg messenger

 ## Manual

 Please take a look at the [Wiki](https://github.com/kamilsss655/uv-k5-firmware-custom/wiki) section for instructions, tips and more:

* [Radio operation](https://github.com/kamilsss655/uv-k5-firmware-custom/wiki/10-%E2%80%90-Radio-operation)

* [Menu](https://github.com/kamilsss655/uv-k5-firmware-custom/wiki/20-%E2%80%90-Menu)

* [Button functions](https://github.com/kamilsss655/uv-k5-firmware-custom/wiki/30-%E2%80%90-Button-functions)

* [Spectrum analyzer](https://github.com/kamilsss655/uv-k5-firmware-custom/wiki/40-%E2%80%90-Spectrum-analyzer)

* [Messenger](https://github.com/kamilsss655/uv-k5-firmware-custom/wiki/42--%E2%80%90-Messenger)

* [Mesh network](https://github.com/kamilsss655/uv-k5-firmware-custom/wiki/43-%E2%80%90-Mesh-network)

* [Encryption](https://github.com/kamilsss655/uv-k5-firmware-custom/wiki/44-%E2%80%90-Encryption)

* [FM broadcast radio receiver](https://github.com/kamilsss655/uv-k5-firmware-custom/wiki/50-%E2%80%90--FM-broadcast-radio-receiver)

* [Flashing the firmware](https://github.com/kamilsss655/uv-k5-firmware-custom/wiki/60-%E2%80%90-Flashing-the-firmware)

* [Differences from egzumer firmware](https://github.com/kamilsss655/uv-k5-firmware-custom/wiki/70-%E2%80%90-Differences-from-egzumer-firmware)

* [Tips & Tricks and Known Issues](https://github.com/kamilsss655/uv-k5-firmware-custom/wiki/80-%E2%80%90-Tips-&-Tricks-and-Known-Issues)


## Radio performance

Please note that the Quansheng UV-Kx radios are not professional quality transceivers, their
performance is strictly limited. The RX front end has no track-tuned band pass filtering
at all, and so are wide band/wide open to any and all signals over a large frequency range.

Using the radio in high intensity RF environments will most likely make reception anything but
easy (AM mode will suffer far more than FM ever will), the receiver simply doesn't have a
great dynamic range, which results in distorted AM audio with stronger RX'ed signals.
There is nothing more anyone can do in firmware/software to improve that, once the RX gain
adjustment I do (AM fix) reaches the hardwares limit, your AM RX audio will be all but
non-existent (just like Quansheng's firmware).
On the other hand, FM RX audio will/should be fine.

But, they are nice toys for the price, fun to play with.

## User customization

You can customize the firmware by enabling/disabling various compile options, this allows
us to remove certain firmware features in order to make room in the flash for others.
You'll find the options at the top of "Makefile" ('0' = disable, '1' = enable) ..

```
ENABLE_CLANG                       := 0     **experimental, builds with clang instead of gcc (LTO will be disabled if you enable this)
ENABLE_SWD                         := 0       only needed if using CPU's SWD port (debugging/programming)
ENABLE_OVERLAY                     := 0       cpu FLASH stuff, not needed
ENABLE_LTO                         := 1     **experimental, reduces size of compiled firmware but might break EEPROM reads (OVERLAY will be disabled if you enable this)

ENABLE_UART                        := 1       without this you can't configure radio via PC !
ENABLE_AIRCOPY                     := 0       easier to just enter frequency with butts
ENABLE_FMRADIO                     := 1       WBFM VHF broadcast band receiver
ENABLE_NOAA                        := 0       everything NOAA (only of any use in the USA)
ENABLE_VOICE                       := 0       want to hear voices ?
ENABLE_VOX                         := 1
ENABLE_ALARM                       := 0       TX alarms
ENABLE_TX1750                      := 0       side key 1750Hz TX tone (older style repeater access)
ENABLE_PWRON_PASSWORD              := 0       power-on password stuff
ENABLE_DTMF_CALLING                := 0       DTMF calling fuctionality, sending calls, receiving calls, group calls, contacts list etc.

ENABLE_BIG_FREQ                    := 1       big font frequencies (like original QS firmware)
ENABLE_SMALL_BOLD                  := 1       bold channel name/no. (when name + freq channel display mode)
ENABLE_KEEP_MEM_NAME               := 1       maintain channel name when (re)saving memory channel
ENABLE_WIDE_RX                     := 1       full 18MHz to 1300MHz RX (though front-end/PA not designed for full range)
ENABLE_TX_WHEN_AM                  := 0       allow TX (always FM) when RX is set to AM
ENABLE_F_CAL_MENU                  := 0       enable the radios hidden frequency calibration menu
ENABLE_SHOW_CHARGE_LEVEL           := 0       show the charge level when the radio is on charge
ENABLE_REVERSE_BAT_SYMBOL          := 0       mirror the battery symbol on the status bar (+ pole on the right)
ENABLE_NO_CODE_SCAN_TIMEOUT        := 1       disable 32-sec CTCSS/DCS scan timeout (press exit butt instead of time-out to end scan)
ENABLE_SQUELCH_MORE_SENSITIVE      := 0       make squelch levels a little bit more sensitive - this has been reported to cause radio freeze in presence of strong signals
ENABLE_FASTER_CHANNEL_SCAN         := 1       increases the channel scan speed, but the squelch is also made more twitchy
ENABLE_RSSI_BAR                    := 1       enable a dBm/Sn RSSI bar graph level in place of the little antenna symbols
ENABLE_AUDIO_BAR                   := 1       experimental, display an audio bar level when TX'ing
ENABLE_COPY_CHAN_TO_VFO            := 1       copy current channel into the other VFO. Long press `1 BAND` when in channel mode
ENABLE_SPECTRUM                    := 1       fagci spectrum analyzer, activated with `F` + `5 NOAA`
ENABLE_REDUCE_LOW_POWER            := 1       makes low power settings even lower (L=0.45W M=3W H=4.7W (f=147Mhz))
ENABLE_BYP_RAW_DEMODULATORS        := 0       additional BYP (bypass?) and RAW demodulation options, proved not to be very useful, but it is there if you want to experiment
ENABLE_BLMIN_TMP_OFF               := 0       additional function for configurable buttons that toggles `BLMin` on and off wihout saving it to the EEPROM
ENABLE_SCAN_RANGES                 := 1       scan range mode for frequency scanning, see wiki for instructions (radio operation -> frequency scanning)
ENABLE_SPECTRUM_COPY_VFO           := 1       pressing MENU in spectrum mode will exit and copy current peak frequency, bw, step, modulation to current VFO, also entering spectrum will carry these settings from VFO (full integration)
ENABLE_SPECTRUM_SHOW_CHANNEL_NAME  := 1       shows channel number and channel name of the peak frequency in spectrum
ENABLE_ADJUSTABLE_RX_GAIN_SETTINGS := 1       keeps the rx gain settings set in spectrum mode after exit (otherwise these are always overwritten to default value), this makes much more sense considering that we have a radio with user adjustable gain so why not use it to adjust to current radio conditions, maximum gain allows to greatly increase reception in scan memory channels mode (in this configuration default gain settings are only set at boot and when exiting AM modulation mode to set it to sane value after am fix)
ENABLE_SPECTRUM_CHANNEL_SCAN       := 1       this enables spectrum channel scan mode (enter by going into memory mode and press F+5, this allows SUPER fast channel scanning (4.5x faster than regular scanning), regular scan of 200 memory channels takes roughly 18 seconds, spectrum memory scan takes roughly 4 seconds, if you have less channels stored i.e 50 - the spectrum memory scan will take only **1 second**
ENABLE_MESSENGER                   := 1       enable messenger
ENABLE_MESSENGER_FSK_MUTE          := 1       mutes speaker once it detects fsk sync word (might cause unintentional mutes during ctcss rx)
ENABLE_MESSENGER_NOTIFICATION      := 1       enable messenger delivery notification
ENABLE_MESSENGER_UART              := 0       enable sending messages via serial with SMS:content command (unreliable)
ENABLE_ENCRYPTION                  := 1       enable ChaCha20 256 bit encryption for messenger
```


## Compiler

arm-none-eabi GCC version 10.3.1 is recommended, which is the current version on Ubuntu 22.04.03 LTS.
Other versions may generate a flash file that is too big.
You can get an appropriate version from: https://developer.arm.com/downloads/-/gnu-rm

clang may be used but isn't fully supported. Resulting binaries may also be bigger.
You can get it from: https://releases.llvm.org/download.html

## Building

If you have docker installed you can use [compile-with-docker.bat](./compile-with-docker.bat) (Windows) or [compile-with-docker.sh](./compile-with-docker.sh) (Linux/Mac), the output files are created in `compiled-firmware` folder. This method gives significantly smaller binaries, I've seen differences up to 1kb, so it can fit more functionalities this way. The challenge can be (or not) installing docker itself.


1. To compile:
```
make docker
```
2. To compile and flash:
```
make run
```

## Credits

Many thanks to various people on Telegram for putting up with me during this effort and helping:

* [OneOfEleven](https://github.com/OneOfEleven)
* [DualTachyon](https://github.com/DualTachyon)
* [Egzumer](https://github.com/egzumer)
* [Joaquimorg](https://github.com/joaquimorg)
* [Mikhail](https://github.com/fagci)
* [Andrej](https://github.com/Tunas1337)
* [Manuel](https://github.com/manujedi)
* @wagner
* @Lohtse Shar
* @Davide
* @Ismo OH2FTG
* @d1ced95
* [@Matoz](https://github.com/spm81)
* and others I forget

<a href="https://github.com/kamilsss655/uv-k5-firmware-custom/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=kamilsss655/uv-k5-firmware-custom" />
</a>

## How to contribute

Thank you for your interest in contributing to this project! Here are some of the many ways in which you can help:

* Report bugs
* Suggest improvements
* Audit the source code
* Submit PRs to fix bugs or implement features
* Test new releases and updates
* Record videos and post on other platforms
* Spread the word about this project
* Write and edit the documentation
* Donate to the project using the "Sponsor" button
   
## License

Original work Copyright 2023 Dual Tachyon
https://github.com/DualTachyon

Modified work Copyright 2024 kamilsss655
https://github.com/kamilsss655

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
