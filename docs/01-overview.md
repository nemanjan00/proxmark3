# 01 — Project Overview

## What is the Proxmark3?

The Proxmark3 is a general-purpose **RFID research tool**. It can read, write, sniff, replay, simulate, brute-force, and clone a very large range of LF (125 kHz) and HF (13.56 MHz) RFID/NFC tags (HID Prox, EM410x, T55xx, MIFARE Classic/Ultralight/DESFire, iCLASS, ISO14443-A/B, ISO15693, FeliCa, LEGIC, Hitag, etc.).

This repository is the **Iceman / RfidResearchGroup fork**, the de-facto community version of the original Jonathan Westhues firmware.

## The three tiers

A running Proxmark3 system is three things at once:

```
+--------------------+      USB-CDC / BT-UART / FPC      +-----------------+      SSP / SSC      +------+
|   Host client      |  <----- PacketCommandNG ----->    |  ARM firmware   |  <-- config/data --> | FPGA |
|   (./pm3, CLI)     |       PacketResponseNG            |  (AT91SAM7S)    |   ADC stream         |      |
|   client/src/*.c   |       on a serial port            |  armsrc/*.c     |                      | RF   |
+--------------------+                                   +-----------------+                      | front|
                                                                 |                                +------+
                                                                 v
                                                          coil drivers, ADC,
                                                          LEDs, buttons, flash
```

1. **Host client** — a C program (`./pm3` wrapper → `proxmark3` binary) running on Linux/macOS/Windows/Android. Big interactive REPL with hundreds of commands, plus Lua and Python scripting. Owns the slow heavy lifting: protocol orchestration, dictionaries, key cracking, file I/O, presentation.

2. **ARM firmware** — runs on an Atmel AT91SAM7S (ARM7TDMI, 256 KB flash, 64 KB RAM) inside the Proxmark. Receives commands over USB-CDC and drives the RF front-end. Hard-real-time work that the host can't do because of latency lives here. Split into a small **bootrom** (always present, can re-flash the rest) and a large **fullimage** (the actual application + embedded FPGA bitstreams).

3. **FPGA** — a Xilinx Spartan-II / Spartan-3 (size depends on board variant) sitting between the ARM and the analog front-end. Handles bit-level RF work that's too fast for the ARM: carrier generation, modulation/demodulation, edge detection, filtering. The ARM loads a bitstream into it on demand depending on what tag protocol it needs to talk.

Almost every Proxmark operation crosses all three boundaries. Reading a MIFARE card: client sends `hf mf rdbl 0 A FFFFFFFFFFFF` → firmware receives `CMD_HF_MIFARE_READBL` → firmware loads the HF/14443a FPGA image → FPGA demodulates the air → firmware runs the crypto1 protocol → bytes go back up to the client.

## Repository layout at a glance

| Directory | Tier | What it is |
|---|---|---|
| `client/`          | host    | The desktop CLI: `proxmark3` binary, all `cmd*.c` command handlers, Lua/Python bindings, dictionaries, traces. |
| `armsrc/`          | firmware | ARM application firmware (the big one). Per-protocol C files (`iso14443a.c`, `lfops.c`, `iclass.c`, …) + `appmain.c` dispatcher. |
| `armsrc/Standalone/` | firmware | "Standalone mode" modules — small apps that run on the device with no host attached. One is selected at firmware build time. |
| `bootrom/`         | firmware | Tiny bootloader. Lives in the first sectors of flash, can re-flash the rest over USB. |
| `fpga/`            | FPGA    | Verilog sources + pre-built `.bit` bitstreams for each board variant (RDV4, generic, iCopy-X, Ultimate). |
| `common/`          | host+fw | Code shared between client and firmware: CRC, LZ4, lfdemod, crapto1, hitag2, generators. |
| `common_arm/`      | firmware | Code shared between `bootrom` and `armsrc` (USB-CDC, clocks, ticks, flashmem, linker bits, HAL Makefile). |
| `common_fpga/`     | FPGA    | Shared `fpga.h` definitions (config word bits, mode IDs) used by both ARM and Verilog conceptually. |
| `include/`         | all     | Public headers shared everywhere (`pm3_cmd.h` is *the* command-ID enum, `protocols.h`, per-tag command structs). |
| `bootrom/`, `armsrc/`, `recovery/` | firmware | Each builds an ELF + bin; `recovery/` is the bootrom+fullimage combined into a JTAG-flashable image. |
| `driver/`          | host    | Windows USB-CDC `.inf` driver. |
| `tools/`           | misc    | Auxiliary host tools: `fpga_compress` (used during build), `mfc_card_only`, `hitag2crack`, `mfd_aes_brute`, JTAG scripts, helper Python/Perl scripts. |
| `doc/`             | docs    | The existing user-facing documentation (manuals, cheat sheets, per-tag notes, standalone-mode docs, datasheets). |
| `docs/`            | docs    | **You are here.** Developer architecture documentation. |
| `traces/`          | data    | Captured air traces shipped for tests and examples. |
| `recovery/`        | build   | Produces the combined bootloader+fullimage `.elf` for JTAG recovery. |

## Supported boards

The codebase targets a small family of boards via the `PLATFORM` build variable (`common_arm/Makefile.hal`):

- `PM3RDV4` — Proxmark3 RDV4 (default, the current flagship; has external SPI flash, smartcard slot, FPC BT add-on header).
- `PM3GENERIC` — generic clones (RDV1/2/3, kkmoon, PiSwords, etc.).
- `PM3ICOPYX` — iCopy-X with a Spartan-3 FPGA.
- `PM3ULTIMATE` — Proxmark3 Ultimate (XC2S50).

The chosen platform changes which FPGA bitstream is bundled, which pins the firmware drives, and whether features like external flash and BT add-on are compiled in.
