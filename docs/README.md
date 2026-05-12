# Proxmark3 — Developer Documentation

This directory is an **architecture-oriented** tour of the Proxmark3 (Iceman fork) codebase, written for someone new to the project who wants to understand *what is here, how the pieces fit together, and where to look* — not how to use the tool day to day.

> User-facing manuals, cheat sheets, install guides, and per-tag notes live in `../doc/` (singular). This `docs/` directory is the developer architecture guide. The two are complementary.

## Read in this order

1. [`01-overview.md`](01-overview.md) — what the Proxmark3 *is*, the three-tier hardware/software story (client ↔ firmware ↔ FPGA), and the repo layout at a glance.
2. [`02-components.md`](02-components.md) — every top-level directory, what lives there, and how it's built.
3. [`03-architecture.md`](03-architecture.md) — the runtime architecture: bootrom vs. application firmware, FPGA images, BigBuf, the ARM main loop, the client REPL.
4. [`04-communication.md`](04-communication.md) — how the host client talks to the device: USB-CDC / BT / FPC, the `PacketCommandNG` / `PacketResponseNG` frame format, `reply_ng` and `CommandReceived`.
5. [`05-firmware.md`](05-firmware.md) — ARM firmware deep dive: `armsrc/`, the `PacketReceived()` command dispatcher, per-protocol modules, standalone modes.
6. [`06-fpga.md`](06-fpga.md) — the Verilog side: how `fpga_pm3_top.v` is structured, what bitstreams exist, how the ARM loads and configures them, how ARM↔FPGA share pins (SSP/SSC).
7. [`07-client.md`](07-client.md) — the `client/` C codebase: the command tree, `cliparser`, Lua/Python scripting, dependencies.
8. [`08-build.md`](08-build.md) — the build system: top-level recursive Makefile, platform/HAL selection, where artifacts land, flashing.
9. [`09-examples.md`](09-examples.md) — **worked examples**: real CLI sessions, exact wire bytes for `hw ping`, the full firmware↔FPGA↔RF dance for `hf 14a reader`, a dummy "hello world" command (`hw echo42`) end-to-end, a dummy standalone mode, Lua script example, build/flash cheat sheet.

## What this is *not*

- Not a user manual — see `../README.md` and `../doc/`.
- Not exhaustive API reference — the code is the reference; this is a map.
- Not authoritative on RFID protocols — for tag-specific notes see `../doc/*_notes.md`.
