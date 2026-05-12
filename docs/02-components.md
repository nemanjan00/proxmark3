# 02 — Components

A walkthrough of every top-level directory, in roughly build-order.

## `bootrom/` — the bootloader

- ~1–2 KB of ARM code that lives in the first sectors of the AT91SAM7S flash and survives `pm3-flash-fullimage`.
- Runs on cold boot. If the user is holding the device button when power comes up (or if the fullimage is missing/corrupt), it stays in bootloader mode and exposes a USB-CDC interface that accepts flash commands (`CMD_BL_*`).
- Otherwise it jumps to the application firmware entry point (`AppMain` in `armsrc/appmain.c`).
- Files: `bootrom.c`, `flash-reset.s`, `ram-reset.s`, `ldscript-flash`. The `version_pm3.c` is generated at build time.
- The host-side flasher (`pm3-flash`, `pm3-flash-all`, `pm3-flash-bootrom`, `pm3-flash-fullimage`) speaks to this over USB-CDC.

## `armsrc/` — the application firmware

The bulk of the on-device code. Single statically-linked ELF, ~250 KB. Entry point: `AppMain()` in `appmain.c` line 3312.

Key files:

- `appmain.c` — startup, the main loop (which calls `PacketReceived()`), the giant `switch (packet->cmd)` that dispatches every command ID, low-level utility commands (`CMD_VERSION`, `CMD_PING`, `CMD_STATUS`, `CMD_CAPABILITIES`, the LF tune/decay/measure helpers).
- `BigBuf.[ch]` — manages the single large RAM buffer (~40 KB on RDV4) that everything competes for: trace log, emulator memory, DMA staging for ADC samples. The "BigBuf" is the heart of the firmware's memory plan; almost every operation calls `BigBuf_Clear()` and asks for a chunk.
- `fpgaloader.[ch]` — decompresses an embedded FPGA bitstream and clocks it into the FPGA over GPIO, then exposes `FpgaWriteConfWord()` etc. to switch FPGA sub-modes.
- `cmd.[ch]` — implements `reply_ng()` / `reply_mix()` / `reply_old()`: how the firmware sends a `PacketResponseNG` back up to the host.
- `dbprint.[ch]` — `Dbprintf()` style debug logging that streams strings up over `PacketResponseNG`.
- Per-protocol C files: `iso14443a.c`, `iso14443b.c`, `iso15693.c`, `iclass.c`, `legicrf.c`, `felica.c`, `hitag2.c`, `hitagS.c`, `hitagu.c`, `em4x50.c`, `em4x70.c`, `lfops.c`, `lfsampling.c`, `epa.c`, `emvsim.c`, `seos.c`, `mifareutil.c`, `mifarecmd.c`, `mifaresim.c`, `mifaredesfire.c`, etc.
- SAM (Secure Access Module) glue for RDV4: `sam_common.c`, `sam_mfc.c`, `sam_seos.c`, `sam_sc.c`, `sam_picopass.c`.
- `Standalone/` — see below.

## `armsrc/Standalone/` — host-less modes

A "standalone mode" is a tiny app compiled *into* the firmware that runs when the user long-presses the device button with no client attached. It lets you walk around with just the Proxmark3 (and a battery) and capture/replay tags.

- Exactly one standalone is built in per firmware image. Selected at build time via `make … STANDALONE=LF_SAMYRUN` (or whatever).
- `dankarmulti` is the meta-mode that bundles several others into one build.
- Each mode is a single `.c` file (e.g. `lf_samyrun.c`, `hf_14asniff.c`, `hf_iceclass.c`, `hf_tcprst.c`) implementing `ModInfo()` and `RunMod()`.
- Listed in `Makefile.hal` (build-time list of supported modes per board) and `Makefile.inc` (sources per mode).
- See `armsrc/Standalone/readme.md` and `doc/standalone/`.

## `fpga/` — Verilog + bitstreams

- `fpga_pm3_top.v` — the **top module for the standard RDV4-family target**. Contains the SPI receiver that the ARM uses to load a config word, plus the LF/HF sub-module mux.
- `fpga_pm3_*.v` — variants for `pm3_ult` (Ultimate) and other boards.
- `fpga_icopyx_top.v`, `fpga_icopyx_hf.v`, `fpga_icopyx_lf.v` — iCopy-X variants (different Xilinx part).
- Sub-modules (one per RF mode):
  - LF: `lo_read.v`, `lo_edge_detect.v`, `lo_passthru.v`, `lo_adc.v`, `lf_edge_detect.v`.
  - HF: `hi_reader.v`, `hi_simulate.v`, `hi_iso14443a.v`, `hi_sniffer.v`, `hi_flite.v` (FeliCa), `hi_get_trace.v`.
  - Shared helpers: `clk_divider.v`, `mux2_*`, `mux8.v`, `mux16.v`, `min_max_tracker.v`, `lp20khz_1MSa_iir_filter.v`.
- Pre-compiled bitstreams (`*.bit`) are checked in — building Verilog requires Xilinx ISE which is not in the normal build path. Bitstreams are compressed with `tools/fpga_compress` and bundled into the firmware as data.
- UCF files (`xc2s30-5-vq100.ucf`, `xc2s50-5-tq144.ucf`, `xc3s100e-4-vq100.ucf`) describe FPGA pinouts per part/board.

For a given board there are typically **four** bitstreams shipped: `lf`, `hf`, `hf_15` (ISO15693), and `felica`. The ARM picks one based on what protocol you're running.

## `common/` — code shared host ↔ firmware

C that compiles on both sides:

- `crc.c`, `crc16.c`, `crc32.c`, `crc64.c` — CRCs used by frame format and by various tag protocols.
- `lfdemod.[ch]` — generic LF demodulators (ASK/Manchester, FSK, PSK).
- `iso15693tools.[ch]` — ISO15693 helpers (CRC, encoding).
- `commonutil.[ch]` — bytewise/printf utilities.
- `legic_prng.[ch]`, `hitag2/`, `crapto1/`, `cryptorf/` — tag-specific crypto/PRNG primitives.
- `lz4/` — vendored LZ4, used to compress FPGA bitstreams.
- `bruteforce.c`, `bucketsort.c`, `generator.c`, `cardhelper.c` — algorithms that both client and firmware sometimes want.
- `default_version_pm3.c` — fallback when `mkversion.sh` can't generate a real one.

## `common_arm/` — code shared bootrom ↔ armsrc

ARM-only, but not specific to either firmware image:

- `usb_cdc.[ch]` — USB CDC-ACM serial implementation on the AT91SAM7S UDP.
- `clocks.[ch]`, `ticks.[ch]` — PLL/PIT setup, busy-wait, timestamping.
- `flashmem.[ch]` — external SPI flash driver (RDV4 only).
- `Makefile.common` — shared compile/link rules.
- `Makefile.hal` — *the* HAL Makefile: defines `PLATFORM`, which bitstream paths to bundle, which features to enable.
- `ldscript.common` — shared linker fragment.

## `common_fpga/` — ARM↔FPGA shared definitions

- `fpga.h` — the bit layout of the 16-bit FPGA config word and the mode-ID constants. Both ARM C (in `fpgaloader.c`) and a developer reading Verilog (`fpga_pm3_top.v`) refer to this conceptually.

## `include/` — public headers

Headers used across multiple directories. Most important:

- `pm3_cmd.h` — **the** command-ID enum (`CMD_HF_MIFARE_READBL`, `CMD_LF_HID_WATCH`, …), plus the `PacketCommandNG` / `PacketResponseNG` wire-format structures. Both client and firmware include this.
- `common.h`, `proxmark3_arm.h` — board / CPU register definitions.
- `at91sam7s512.h` — Atmel SoC register map.
- `protocols.h` — protocol-level constants (ISO14443 command bytes, etc.).
- `iclass_cmd.h`, `mifare.h`, `iso14b.h`, `iso15.h`, `iso18.h`, `hitag.h`, `em4x50.h`, `em4x70.h`, `legic.h`, `desfire.h`, `seos_cmd.h`, `zx8211.h` — per-protocol command-payload structs (the `data.asBytes` of a `PacketCommandNG` is often one of these).
- `pmflash.h` — external flash layout (RDV4).
- `usart_defs.h` — USART baud / FPC link definitions.

## `client/` — host CLI

See [`07-client.md`](07-client.md) for detail. Highlights:

- `client/src/proxmark3.c` — `main()`, argument parsing, port discovery.
- `client/src/comms.[ch]` — the host side of the wire protocol: USB-CDC / TCP / BT serial.
- `client/src/cmdmain.c` — top-level command tree.
- `client/src/cmd*.c` — ~150 command modules, mirroring firmware capability (`cmdhf14a.c`, `cmdlfem4x05.c`, …).
- `client/src/uart/` — per-OS serial port implementation.
- `client/src/cliparser*.c` — getopt-style argument parsing used by every command.
- `client/src/scripting.c`, `pm3_luawrap.c`, `pm3_pywrap.c` — Lua and Python embedded interpreters.
- `client/deps/` — vendored libs: `amiibo`, `cliparser`, `cjson`, `hardnested`, `jansson`, `linenoise`, `mbedtls`, `reveng`, `tinycbor`, `whereami`.
- `client/luascripts/`, `client/pyscripts/` — shipped scripts callable from the REPL.
- `client/dictionaries/` — default key dictionaries (MIFARE, iCLASS, Hitag, T55xx).
- `client/resources/` — sim011/sim013/sim014 SIM module firmware, embedded data.
- `client/experimental_client_with_swig/` — experimental SWIG-based Python bindings.

## `tools/`

Host-side helpers that don't belong in `client/`:

- `fpga_compress/` — used during firmware build to LZ4-compress the FPGA `.bit` files before embedding.
- `mfc/` — MIFARE Classic helpers (`pm3_mfd2eml.py` etc., shipped alongside the install).
- `hitag2crack/`, `mfd_aes_brute/`, `mfulc_des_brute/`, `mfc/mfc_card_only`, `mfc/mfc_card_reader` — standalone offline crackers.
- `btaddon/` — firmware for the RDV4 Bluetooth add-on board.
- `jtag_openocd/` — OpenOCD configs for JTAG flashing/recovery.
- `simmodule/` — SIM module firmware sources.
- `deprecated-hid-flasher/` — old HID flasher (kept for reference).
- `mkversion.sh`, `build_all_firmwares.sh`, `pm3_tests.sh` — build/CI helpers.

## `recovery/`

Builds a combined `bootrom + fullimage` ELF flashable over JTAG, used when the bootloader itself is gone.

## `driver/`

Windows USB-CDC `.inf` file.

## `traces/`

Air-trace samples, used by client tests and as worked examples.

## `doc/` vs `docs/`

- `doc/` — user/operator-facing documentation that has accreted over many years. Per-tag notes, install instructions, cheat sheets, "Use of Proxmark" guides, standalone-mode docs, datasheets, images.
- `docs/` — this directory. Developer-facing architecture notes.
