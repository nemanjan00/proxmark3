# 06 — FPGA

The Verilog side of the Proxmark3. The FPGA is the bridge between the analog RF front-end (coil drivers + 8-bit ADC) and the ARM's high-speed serial peripherals (SSC/SSP).

## Why is there an FPGA at all?

The ARM is too slow (and too irregularly timed, with interrupts and USB ticking) to do bit-level RF work directly. The FPGA does the **hard-real-time signal-processing layer**:

- Generates the 125 kHz LF carrier (divided down from a master clock) and the 13.56 MHz HF carrier.
- Modulates the carrier (OOK / ASK / PSK / load modulation, depending on protocol).
- Demodulates: edge-detects LF data, sub-carrier-demodulates HF, runs a 20 kHz IIR low-pass for FeliCa, etc.
- Frames demodulated bytes into the ARM's SSC peripheral via DMA.

The ARM keeps the protocol state machine; the FPGA keeps the timing.

## Bitstream family

For a standard Proxmark3 board, the build ships **four** bitstreams, all selected by `FPGA_config` in `common_fpga/fpga.h`:

| Enum                       | File (RDV4)              | Used for |
|----------------------------|--------------------------|----------|
| `FPGA_BITSTREAM_LF`        | `fpga_pm3_lf.bit`        | All 125 kHz operations |
| `FPGA_BITSTREAM_HF`        | `fpga_pm3_hf.bit`        | ISO14443-A/B, iCLASS, LEGIC, MIFARE, …   |
| `FPGA_BITSTREAM_HF_15`     | `fpga_pm3_hf_15.bit`     | ISO15693 (different sub-carrier handling) |
| `FPGA_BITSTREAM_HF_FELICA` | `fpga_pm3_felica.bit`    | Sony FeliCa / ISO18092 (212 kbps) |

Variants for other boards: `fpga_pm3_ult_*` (Ultimate, XC2S50), `fpga_icopyx_*` (iCopy-X, XC3S100E).

```
                        bundled inside fullimage.bin
                       ┌──────────────────────────────┐
                       │  LZ4-compressed bitstreams   │
                       │   • lf                       │
                       │   • hf                       │
                       │   • hf_15                    │
                       │   • felica                   │
                       └──────────┬───────────────────┘
                                  │ FpgaDownloadAndGo(target)
                                  ▼
                       decompress on-the-fly,
                       bitbang into FPGA
```

Bitstreams are checked in as `.bit` because building the Verilog needs Xilinx ISE (proprietary, EOL) — the normal C build path **does not invoke a Verilog toolchain**. Re-building bitstreams is a separate manual step done by maintainers; see `fpga/tests/` and `fpga/strip_date_time_from_binary.py`.

## Top-level Verilog module

`fpga/fpga_pm3_top.v` is the entry point for the RDV4-family target. Conceptually:

```
                            ┌─────────────────────────────────────────┐
                            │            fpga_pm3_top.v               │
                            │                                         │
   ARM SPI ────► ncs/spck   │   ┌───────────────┐                     │
   ARM SPI ────► mosi       │──▶│  SPI receiver │                     │
                            │   │  shift_reg    │                     │
                            │   └──────┬────────┘                     │
                            │          ▼                              │
                            │   conf_word [11:0]                      │
                            │          │                              │
                            │   ┌──────┴────────┐                     │
                            │   │ decode major  │  major_mode = [8:6] │
                            │   │ mode + flags  │  minor flags = [5:0]│
                            │   └──────┬────────┘                     │
                            │          │                              │
                            │   ┌──────┴───────────────────────────┐  │
                            │   │ instantiate ALL sub-modules:     │  │
                            │   │  lo_read, lo_edge_detect,        │  │
                            │   │  lo_passthru, lo_adc,            │  │
                            │   │  hi_reader, hi_simulate,         │  │
                            │   │  hi_iso14443a, hi_sniffer,       │  │
                            │   │  hi_flite, hi_get_trace          │  │
                            │   └──────┬───────────────────────────┘  │
                            │          │                              │
                            │   ┌──────▼────────┐                     │
                            │   │  mux8 fabric  │  conf_word[8:6]     │
                            │   │  routes one   │  selects which      │
                            │   │  sub-module's │  sub-module drives  │
                            │   │  outputs to   │  the I/O pins       │
                            │   │  the ARM/AFE  │                     │
                            │   └──┬───────────┬┘                     │
                            └──────┼───────────┼──────────────────────┘
                                   ▼           ▼
                              ARM SSC      Coil drivers,
                              (ssp_*)      ADC, PWR_OE
```

Every sub-module is *instantiated* in the bitstream — the mux just picks whose outputs are wired up. That's why a single bitstream covers e.g. "HF reader", "HF sim", and "HF sniff" without re-flashing the FPGA. Only protocol *families* (LF / HF / HF-15 / FeliCa) need a different bitstream because their analog paths and clocks differ.

## The 16-bit config word

The ARM writes a 16-bit word to the FPGA over SPI. The top 4 bits are an opcode; the rest are parameters. From `fpga/define.v`:

```
| 15 14 13 12 | 11 10 09 08 07 06 05 04 03 02 01 00 |
|   C C C C   |  M  M  M  P  P  P  P  P  P  P  P    |  (HF)
|   C C C C   |              D D D D D D D D        |  (set divisor)
|   C C C C   |              T T T T T T T T        |  (set edge-detect threshold)
|   C C C C   |                              E      |  (trace enable)
```

`C` = `FPGA_CMD_*` opcode:

| `FPGA_CMD_*`                       | Effect |
|-----------------------------------|--------|
| `SET_CONFREG` (=1)                | Set major mode + per-mode flags. |
| `SET_DIVISOR` (=2 in LF)          | LF: set carrier divisor (selects 125 kHz vs 134 kHz etc.). |
| `SET_EDGE_DETECT_THRESHOLD` (=3)  | LF/HF: ADC edge threshold for the demodulators. |
| `TRACE_ENABLE` (=2 in HF)         | Turn on the FPGA-side trace dump. |

`M` (3 bits) = `FPGA_MAJOR_MODE_*`:

```
LF (in the LF bitstream)            HF (in the HF / HF_15 / FeliCa bitstreams)
  000 LF_READER                       000 HF_READER
  001 LF_EDGE_DETECT (sim)            001 HF_SIMULATOR
  010 LF_PASSTHRU                     010 HF_ISO14443A
  011 LF_ADC                          011 HF_SNIFF
  111 OFF                             100 HF_ISO18092 (FeliCa)
                                      101 HF_GET_TRACE
                                      111 OFF
```

The `P` bits are per-mode flags (e.g. `FPGA_HF_READER_MODE_RECEIVE_AMPLITUDE`, `FPGA_LF_ADC_READER_FIELD`).

## ARM↔FPGA pin map

```
   ┌────────────┐                          ┌──────────────┐
   │   ARM      │  SPI (cfg word)          │              │
   │ AT91SAM7S  │ ─────── ncs ───────────▶ │              │
   │            │ ─────── spck ──────────▶ │              │
   │            │ ─────── mosi ──────────▶ │     FPGA     │
   │            │                          │              │
   │ SSC (data) │ ◀────── ssp_din  ─────── │              │
   │            │ ─────── ssp_dout ──────▶ │              │
   │            │ ◀────── ssp_clk  ─────── │              │
   │            │ ◀────── ssp_frame ────── │              │
   │            │                          │              │
   │   ADC      │ ◀────── adc_clk ──────── │              │
   │   start    │                          │              │
   └────────────┘   adc_d[7:0]──┐          └──────────────┘
                                │                ▲ │
                                │                │ │
                          ┌─────┴──────┐    pwr_lo/hi
                          │ AFE / ADC  │    pwr_oe1..4
                          │ coil drv   │ ◀──────┘
                          └─────┬──────┘
                                │ RF
                                ▼
                              antenna
```

`ssp_*` are the ARM's SSC peripheral lines used as a high-speed bit/byte pipe — that's how demodulated air bits get into the firmware and how transmit bits get out.

## How the firmware uses it

```c
// armsrc/fpgaloader.c
void FpgaDownloadAndGo(int bitstream_target);   // (re)load bitstream if needed
void FpgaWriteConfWord(uint16_t word);          // shift 16 bits over SPI to FPGA
void FpgaSetupSsc(uint16_t fpga_mode);          // ARM SSC config matching the FPGA mode
void FpgaSetupSscExt(...);                      // …with extra knobs

// typical call sequence inside a protocol module
FpgaDownloadAndGo(FPGA_BITSTREAM_HF);
FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER | FPGA_HF_READER_MODE_RECEIVE_AMPLITUDE);
FpgaSetupSsc(FPGA_MAJOR_MODE_HF_READER);
// ...drive protocol over SSC...
FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
```

The current bitstream is remembered so calling `FpgaDownloadAndGo(...)` with the same target is a noop (no re-flash). Switching from `HF` to `HF_15` to `FELICA` re-flashes the FPGA (~tens of ms); switching modes *within* a bitstream is just an SPI write.

## Where to look

- The mode/command enum (single source of truth, shared with ARM): `fpga/define.v`.
- ARM-side `FPGA_BITSTREAM_*` enum and bitstream metadata: `common_fpga/fpga.h`.
- ARM-side loader: `armsrc/fpgaloader.[ch]`.
- Top module (RDV4 family): `fpga/fpga_pm3_top.v`.
- Top module (Ultimate / iCopy-X): `fpga/fpga_pm3_ult_*.v`, `fpga/fpga_icopyx_top.v`.
- Sub-modules: one per major mode (`hi_reader.v`, `hi_iso14443a.v`, `hi_sniffer.v`, `hi_simulate.v`, `hi_flite.v`, `hi_get_trace.v`, `lo_read.v`, `lo_edge_detect.v`, `lo_passthru.v`, `lo_adc.v`).
- Helpers: `clk_divider.v` (carrier divisor), `lf_edge_detect.v`, `min_max_tracker.v`, `lp20khz_1MSa_iir_filter.v`, `mux*.v`.
- Pinouts per chip: `xc2s30-5-vq100.ucf`, `xc2s50-5-tq144.ucf`, `xc3s100e-4-vq100.ucf`.
- Compression tool used during firmware build: `tools/fpga_compress/`.
- Notes: `../doc/fpga_arm_notes.md`.
