# 11 — Clock Domains, Peripherals & DSP Block Diagram

A hardware-level view of the ARM↔FPGA↔AFE system. Three concerns:

1. **Clocks** — where every edge in the system comes from.
2. **Peripherals** — what blocks on the ARM (PMC, PIO, SSC, SPI, ADC, USB, USART, TC) and the FPGA (SPI rx, mux fabric, sub-modules) actually do.
3. **DSP datapath** — the signal chain from antenna → ADC → FPGA correlators / FSK demod → SSC bytes → ARM RAM.

## 1. Clock domains

```mermaid
flowchart LR
    XTAL16[("16 MHz xtal<br/>MAINCK")] --> PMC
    XTAL1356[("13.56 MHz xtal<br/>ck_1356meg<br/>ck_1356megb")] -.-> FPGA_HF
    subgraph ARM["ARM AT91SAM7S"]
        PMC["PMC<br/>PLL ×12 ÷2 ÷2 = 48 MHz MCK<br/>+ PCK0 = PLL ÷4 = 24 MHz"]
        MCK[("MCK = 48 MHz<br/>CPU, peripheral bus")]
        PCK0[("PCK0 = 24 MHz<br/>output to FPGA pin")]
        SSC["SSC<br/>(serial sync controller)"]
        SPI["SPI<br/>(config word to FPGA)"]
        ADCp["(unused for RF —<br/>FPGA samples the analog ADC)"]
        TC["Timer/Counter TC0..2<br/>(SspClk count, busy waits)"]
        PMC --> MCK
        PMC --> PCK0
        MCK --> SSC
        MCK --> SPI
        MCK --> TC
    end

    PCK0 -->|pck0 pin| FPGA
    SPI -->|spck, ncs, mosi| FPGA
    SSC <-->|ssp_clk, ssp_din, ssp_dout, ssp_frame| FPGA
    
    subgraph FPGA["Xilinx FPGA"]
        FPGA_HF["hi_* sub-modules<br/>clocked @ 13.56 MHz (HF)"]
        FPGA_LF["lo_* sub-modules<br/>clocked @ 24 MHz pck0,<br/>divided by clk_divider"]
        FPGA_SPI["SPI rx<br/>clocked @ spck (asynchronous,<br/>posedge spck if ~ncs)"]
    end
    
    FPGA_HF --> AFE
    FPGA_LF --> AFE
    
    subgraph AFE["Analog front-end"]
        ADC[("8-bit ADC<br/>sample-clocked by FPGA's adc_clk")]
        COIL["coil drivers<br/>pwr_hi (13.56), pwr_lo (125k)<br/>pwr_oe1..4"]
        XCROSS["zero-cross detectors<br/>cross_hi, cross_lo"]
    end
    
    ADC -->|adc_d[7:0]| FPGA
    FPGA -->|adc_clk| ADC
    XCROSS -->|cross_hi, cross_lo| FPGA
    COIL <--> ANT(("antenna"))
```

### 1.1 ARM-side clock setup — `common_arm/clocks.c`

```c
// MAINCK = 16 MHz crystal (defined in include/proxmark3_arm.h:30)
// MCK    = MAINCK * 12 / 2 / 2 = 48 MHz

AT91C_BASE_PMC->PMC_PLLR =
    PMC_PLL_DIVISOR(2)         |   //  ÷2
    PMC_PLL_MULTIPLIER(12)     |   //  ×12 → 96 MHz PLL output
    PMC_PLL_FREQUENCY_RANGE(0) |
    PMC_PLL_USB_DIVISOR(1)     |
    PMC_PLL_COUNT_BEFORE_LOCK(0x3F);

// MCK = PLL / 2 = 48 MHz, source = PLL
AT91C_BASE_PMC->PMC_MCKR = AT91C_PMC_PRES_CLK_2 | AT91C_PMC_CSS_PLL_CLK;
```

So the chip ends up with **MCK = 48 MHz** driving the CPU and APB peripherals.

### 1.2 PCK0 — the FPGA's logic clock — `armsrc/appmain.c:3324`

```c
// The FPGA gets its clock from us via PCK0 output.
AT91C_BASE_PIOA->PIO_BSR = GPIO_PCK0;                     // peripheral B function
AT91C_BASE_PIOA->PIO_PDR = GPIO_PCK0;                     // disable PIO, let PMC drive
AT91C_BASE_PMC->PMC_SCER = AT91C_PMC_PCK0;                // enable PCK0 clock signal
// PCK0 = PLL / 4 = 96 MHz / 4 = 24 MHz
AT91C_BASE_PMC->PMC_PCKR[0] = AT91C_PMC_CSS_PLL_CLK | AT91C_PMC_PRES_CLK_4;
AT91C_BASE_PIOA->PIO_OER = GPIO_PCK0;                     // drive the pin
```

PCK0 is the only clock the ARM gives the FPGA. **The 13.56 MHz HF carrier is NOT generated from PCK0** — it has its own dedicated crystal feeding `ck_1356meg` (and a buffered copy `ck_1356megb`) straight into FPGA pins. This is critical: the HF carrier must be a clean oscillator, not a PLL-derived clock, to meet the ISO14443 / 15693 timing budgets.

### 1.3 The three clock domains inside the FPGA

| Domain          | Source pin            | Rate          | Used by                              |
|-----------------|-----------------------|---------------|--------------------------------------|
| **HF**          | `ck_1356meg(b)`       | 13.56 MHz     | `hi_reader`, `hi_simulate`, `hi_iso14443a`, `hi_sniffer`, `hi_flite`, `hi_get_trace` — drives the ADC clock too |
| **LF**          | `pck0`                | 24 MHz, then divided by `clk_divider` with an ARM-set `divisor` to produce the LF carrier (95 / 125 / 134 kHz, etc.) | `lo_read`, `lo_edge_detect`, `lo_passthru`, `lo_adc` |
| **SPI config**  | `spck` (ARM SPI clock)| burst, async to MCK | The 16-bit `shift_reg` in `fpga_pm3_top.v`. Captured on `posedge spck if ~ncs`, latched into `conf_word` on `posedge ncs`. |

The HF and LF domains never run simultaneously — only one major-mode is active at a time and the unused domain is held idle.

### 1.4 LF carrier generation — `fpga/clk_divider.v`

```verilog
module clk_divider(
    input  clk,                 // pck0 = 24 MHz
    input  [7:0] divisor,
    output [7:0] div_cnt,       // current count (used for fine phase decisions)
    output div_clk              // divided clock
);
    reg [7:0] div_cnt_ = 0;
    reg       div_clk_ = 0;
    always @(posedge clk) begin
        if (div_cnt_ == divisor) begin
            div_cnt_ <= 0;
            div_clk_ <= ~div_clk_;          // toggle on terminal count → period = 2*(divisor+1)
        end else
            div_cnt_ <= div_cnt_ + 1;
    end
endmodule
```

For 125 kHz: `pck0 / (2*(divisor+1)) = 125 000` → `divisor = 95`. That's `LF_DIVISOR_125` on the ARM side. The ARM uploads the divisor with the `FPGA_CMD_SET_DIVISOR` opcode (see `fpga/define.v`).

### 1.5 ADC clock — selected by the active sub-module

The ADC is a single 8-bit converter clocked by whatever the active sub-module routes to `adc_clk`. The `mux8 mux_adc_clk` in `fpga_pm3_top.v` picks one of the eight `muxN_adc_clk` lines based on `conf_word[8:6]` (the major mode):

- HF reader / sim / 14a / sniff / FeliCa: `adc_clk = ck_1356meg` → 13.56 Msps (oversampled relative to the 848 / 424 / 212 kHz subcarriers).
- LF reader: `adc_clk = ~pck_divclk` → samples on the falling edge of the divided clock, ~125 kHz × 2 effective rate gated by the 24 MHz master.
- LF edge-detect: `adc_clk = pck0` (24 Msps — used as the absolute timebase, not for sample integration).
- Mode OFF: `adc_clk` parked.

The `adc_noe` line is hardwired low (`assign adc_noe = 1'b0;` in the top module) so the ADC outputs are always enabled — the FPGA chooses when to actually look at them.

## 2. Peripherals — who talks to whom

```
                                ┌────────────────────────────────────────┐
                                │                ARM AT91SAM7S           │
                                │                                        │
                  USB host  ◀──▶│ UDP (USB Device Port)  ── usb_cdc.c    │
                                │     CDC-ACM serial                     │
                                │                                        │
                                │ PIT  ── ticks.c, GetTickCount()        │
                                │ TC0  ── SSP clock counter              │
                                │       (used as protocol timebase)      │
                                │ TC1  ── tear-off, jamming windows      │
                                │                                        │
   ┌──── miso/mosi/spck ───────▶│ SPI   ── flashmem.c (RDV4 SPI flash    │
   │     ncs0/ncs2              │             on NCS0/NCS2)              │
   │                            │       ── fpgaloader.c uses bit-banged  │
   │                            │             config-word writes; the    │
   │                            │             *bitstream* download is    │
   │                            │             also bit-banged on PIO     │
   │                            │             (not the SPI peripheral)   │
   │                            │                                        │
   │   spck/ncs/mosi  ─────────▶│ PIO-bitbang to FPGA conf SPI           │
   │                            │                                        │
   │                            │ SSC   ── high-speed sync serial        │
   │   ssp_clk◀                 │         to/from FPGA. PDC (DMA) into   │
   │   ssp_din◀                 │         BigBuf. fpgaloader.c:          │
   │   ssp_dout▶                │         FpgaSetupSsc()                 │
   │   ssp_frame◀               │                                        │
   │                            │                                        │
   │   USART0  ◀────────────────│ USART ── BT add-on / FPC link           │
   │                            │         (RDV4 only; usart.c)           │
   │                            │                                        │
   │   smartcard pins  ◀────────│ USART1 in ISO7816 mode ── SAM slot     │
   │                            │         (RDV4 only; sam_*.c)           │
   │                            │                                        │
   │   GPIO LEDs / button ◀────▶│ PIOA  ── LED_A..D, BUTTON_PRESS()       │
   │                            │                                        │
   │                            │ pck0  ─── 24 MHz output to FPGA        │
   └────────────────────────────┘
```

### 2.1 ARM peripheral map (RDV4)

| AT91 peripheral | What it does in Proxmark3                                                | Driver files |
|---|---|---|
| **PMC**             | PLL, MCK, PCK0 — boot setup                                          | `common_arm/clocks.c` |
| **PIO Controller A**| Every GPIO pin; LEDs, button, mux selects                             | macros in `include/proxmark3_arm.h`, `include/config_gpio.h` |
| **UDP (USB)**       | USB-CDC serial to host                                                | `common_arm/usb_cdc.[ch]` |
| **SSC**             | High-speed bidirectional sync serial **to/from the FPGA** — main data path | `armsrc/fpgaloader.c` (`FpgaSetupSsc`, PDC config), every protocol module |
| **PDC (per-peripheral DMA)** | DMA backing the SSC; lets data flow into BigBuf without the CPU | same |
| **SPI**             | RDV4 SPI flash on NCS0; smart-card detect on NCS2                     | `common_arm/flashmem.[ch]` |
| **USART0**          | Bluetooth/FPC link (RDV4)                                            | `armsrc/usart.[ch]`, `client` uses it transparently as serial |
| **USART1 (ISO7816 mode)** | RDV4 SAM smart-card slot                                       | `armsrc/sam_*.c` |
| **TC0**             | SSP clock count — used as a protocol timebase                         | `armsrc/ticks.c` (`StartCountSspClk`, `GetCountSspClk`) |
| **TC1 / TC2**       | Tear-off windows, jamming windows, generic delays                     | `armsrc/ticks.c` |
| **PIT**             | 1 ms periodic — `GetTickCount()`                                      | `common_arm/ticks.[ch]` |
| **AIC**             | Interrupt controller — minimal use; firmware is mostly polled         | implicit |
| **WDT**             | Watchdog — disabled / kicked from main loop                           | `common_arm/clocks.c`, `appmain.c` |

The ADC peripheral inside the ARM is **not** used for RF — RF samples go through the FPGA's `adc_d[7:0]` bus from an external ADC. The ARM's ADC is only used for low-frequency housekeeping (battery voltage etc., not present on all boards).

### 2.2 FPGA peripheral map

The FPGA itself is just configurable logic, but `fpga_pm3_top.v` exposes a fixed pin interface:

| Pin / bus           | Direction (FPGA's POV) | Source/sink (ARM side)         | Purpose |
|---|---|---|---|
| `ck_1356meg`, `ck_1356megb` | in           | 13.56 MHz crystal              | HF carrier reference |
| `pck0`              | in                    | ARM PMC                        | 24 MHz logic clock, LF carrier base |
| `spck`, `ncs`, `mosi` | in (config SPI)     | ARM PIO bit-bang               | 16-bit config-word writes |
| `miso`              | out                   | (unused for config, but pinned) | mostly tied 0 |
| `ssp_clk`, `ssp_frame` | out                | ARM SSC RX clock + sync        | strobe data into ARM |
| `ssp_din`           | out                   | ARM SSC RD                     | demodulated samples to ARM |
| `ssp_dout`          | in                    | ARM SSC TD                     | modulation bits / commands to FPGA |
| `adc_d[7:0]`        | in                    | external 8-bit ADC             | analog samples |
| `adc_clk`           | out                   | external ADC                   | sample clock |
| `adc_noe`           | out                   | external ADC                   | output enable (always asserted) |
| `pwr_hi`            | out                   | HF coil driver                 | 13.56 MHz carrier / modulation |
| `pwr_lo`            | out                   | LF coil driver                 | 125 / 134 kHz carrier |
| `pwr_oe1..4`        | out                   | coil driver enables / shaping  | mode-specific (full mod, shallow mod, RDV4 shallow mod) |
| `cross_hi`, `cross_lo` | in                 | zero-cross comparator          | LF/HF timing reference for sniff / passthru |
| `dbg`               | out                   | (test point)                   | per-sub-module debug signal |

### 2.3 The SSC ↔ FPGA word size

```c
// armsrc/fpgaloader.c:166
if (((fpga_mode & FPGA_MAJOR_MODE_MASK) == FPGA_MAJOR_MODE_HF_READER) &&
    (FpgaGetCurrent() == FPGA_BITSTREAM_HF || FpgaGetCurrent() == FPGA_BITSTREAM_HF_15)) {
    AT91C_BASE_SSC->SSC_RFMR = SSC_FRAME_MODE_BITS_IN_WORD(16) | AT91C_SSC_MSBF | ...; // 16-bit
} else {
    AT91C_BASE_SSC->SSC_RFMR = SSC_FRAME_MODE_BITS_IN_WORD(8)  | AT91C_SSC_MSBF | ...; // 8-bit
}
```

In HF reader mode, the FPGA shoves **16 bits per `ssp_frame` pulse** (an I/Q pair, 8 bits each). In every other mode it's 8-bit samples. That single line decides whether the ARM expects I/Q correlator output or a stream of plain amplitude / edge bytes.

## 3. DSP datapath in `hi_reader.v` — the block diagram

This is the most signal-processing-heavy module in the project. Walking it end-to-end for HF reader mode (`major_mode = HF_READER`, `subcarrier = 424 kHz`, `minor_mode = RECEIVE_IQ`):

```
                                                       ┌─────────────────────────────┐
   antenna ─── analog tank ─── envelope detector ────▶ │  external 8-bit ADC         │
                                                       │  sample rate = adc_clk      │
                                                       │           = 13.56 MHz       │
                                                       └──────────────┬──────────────┘
                                                                      │ adc_d[7:0]
                                                                      ▼
                                            ┌──────────────────────────────────────┐
                                            │  Hysteresis comparator               │
                                            │  (after_hysteresis)                  │
                                            │  Detect reader's own AM modulation   │
                                            │  → used in sniff for "reader bits"   │
                                            └──────────────┬───────────────────────┘
                                                           │ after_hysteresis
                                                           ▼
                                                    after_hysteresis_prev,
                                                    after_hysteresis_prev_prev
                                                    (3-deep delay line)
                                                                  
   subcarrier select  (conf_word[5:4])                            
        │                                                         
        ▼                                                         
   ┌────────────────────────────┐         ┌─────────────────────────────────────┐
   │  subcarrier reference gen  │         │   6-bit free-running counter        │
   │  (just tapping             │ ◀──┐    │   corr_i_cnt ← corr_i_cnt + 1       │
   │   corr_i_cnt[3], [4], [5]) │    │    │   on every negedge adc_clk          │
   │                            │    │    └──────────────┬──────────────────────┘
   │ subcarrier_I, subcarrier_Q │    │                   │
   └─────────────┬──────────────┘    └───────────────────┘ corr_i_cnt
                 │
                 │  (±1 in-phase / quadrature)
                 ▼
   adc_d ──▶ ┌──────────────────────────────────────┐
             │   I/Q correlators                    │
             │                                      │
             │   if (subcarrier_I) corr_i_accum += adc_d
             │   else               corr_i_accum -= adc_d   ──▶ corr_i_accum [signed 14b]
             │                                      │
             │   (same for Q)                       │  ──▶ corr_q_accum [signed 14b]
             │                                      │
             │   reset every 64 ADC samples         │
             │   (one full 13.56 MHz / 64 = 212 kHz "report period")
             └──────────────────────────────────────┘
                            │
                            │   on corr_i_cnt == 0  (every 64 samples)
                            ▼
   ┌────────────────────────────────────────────────────────────┐
   │  Amplitude block                                           │
   │    abs_ci  = |corr_i_accum|                                │
   │    abs_cq  = |corr_q_accum|                                │
   │    max_ci_cq    = max(abs_ci, abs_cq)                      │
   │    min_ci_cq_2  = min(abs_ci, abs_cq) / 2                  │
   │    corr_amplitude = max_ci_cq + min_ci_cq_2                │
   │                  ≈ √(I²+Q²)  (cheap polar approximation)   │
   └─────────────┬──────────────────────────────────────────────┘
                 │
                 │  also fed by, in WITH_HF_15 builds:
                 │
   ┌─────────────┴──────────────────────────────────────────────┐
   │  FSK matched filter  (only present if WITH_HF_15)          │
   │  Three correlators at period 16 / 28 / 32 samples,         │
   │  comparing avg against a 128-tap delay line  → fskout[1:0] │
   │     00 idle                                                │
   │     01 424 kHz subcarrier present                          │
   │     10 484 kHz subcarrier present                          │
   │     11 848 kHz transition                                  │
   └─────────────┬──────────────────────────────────────────────┘
                 │
                 ▼
   ┌────────────────────────────────────────────────────────────┐
   │  Output packer (selected by minor_mode)                    │
   │                                                            │
   │   RECEIVE_IQ:         corr_i_out = corr_i_accum[11:4]      │
   │                       corr_q_out = corr_q_accum[11:4]      │
   │                                                            │
   │   RECEIVE_AMPLITUDE:  corr_i_out = {2'b0, amp[13:8]}       │
   │                       corr_q_out = amp[7:0]                │
   │                                                            │
   │   RECEIVE_AMPLITUDE +                                      │
   │   2SC_424_484:        corr_i_out = {fskout, amp[13:8]}     │
   │                       corr_q_out = amp[7:0]                │
   │                                                            │
   │   SNIFF_AMPLITUDE:    pack amp + reader-bit history        │
   │                                                            │
   │   SNIFF_IQ:           pack I, Q, reader-bit history        │
   └─────────────┬──────────────────────────────────────────────┘
                 │ {corr_i_out[7:0], corr_q_out[7:0]} = 16 bits
                 ▼
   ┌────────────────────────────────────────────────────────────┐
   │  Bit serializer                                            │
   │                                                            │
   │   Every 4 adc_clk cycles shift one bit out of ssp_din      │
   │   from corr_i_out[7], MSB-first.                           │
   │                                                            │
   │   ssp_clk   = adc_clk / 4 = 3.39 MHz                       │
   │   ssp_frame = pulse high while corr_i_cnt ∈ [1,3]          │
   │              (marks the start of each 16-bit report)       │
   │   ssp_din   = corr_i_out[7]                                │
   └─────────────┬──────────────────────────────────────────────┘
                 │ ssp_clk + ssp_din + ssp_frame
                 ▼
                 ARM SSC peripheral (PDC DMA → BigBuf)
```

### 3.1 Timing budget

```
adc_clk     13.56 MHz   one ADC sample every 73.7 ns
corr_i_cnt  modulo 64    one I/Q report every 64 samples = 4.72 µs
ssp_clk     adc_clk / 4 = 3.39 MHz, MSB-first, 16 bits per frame
            → one 16-bit word every 16 × ssp_clk = 4.72 µs ✓

That's exactly one report period = 64 ADC samples — there is *no buffering*
between the correlator and the ARM beyond the few-bit serializer shift register.
Lose timing and you lose data; the SSC PDC has to be running.
```

### 3.2 Why correlators?

The tag responds with a load-modulated signal whose envelope contains a sub-carrier (typically 424 kHz or 848 kHz) being keyed by the actual data bits. The reader doesn't know the *phase* of that subcarrier, so the FPGA does a square-wave Costas-style correlation: multiply by an in-phase ±1 and a quadrature ±1 reference, integrate over the report period, then report `√(I²+Q²)` (approximated as `max + min/2` to avoid a real square-root). Amplitude is phase-invariant → robust against tag distance/orientation.

The optional FSK filter (15693 only) is the same idea but using **three** candidate periods to discriminate between the two simultaneous subcarriers present in ISO15693's "2 out of 4" coding.

### 3.3 What the ARM does with the bytes

```c
// armsrc/iso15693.c — receive loop (paraphrased)
while (timed_out == false) {
    if (AT91C_BASE_SSC->SSC_SR & AT91C_SSC_RXRDY) {
        uint16_t word = AT91C_BASE_SSC->SSC_RHR;  // 16-bit because HF reader mode
        uint8_t  amp  = word >> 8;                // corr_i_out
        uint8_t  q    = word & 0xFF;              // corr_q_out (= amp low when 2SC mode)
        uint8_t  fsk  = (word >> 14) & 0x3;       // fskout when 2SC mode (HF_15 only)

        // …Manchester / 1-out-of-4 / FSK state-machine decoding…
    }
}
```

In `hf 14a` reader mode the same 16-bit words contain `{I[7:0], Q[7:0]}` and the firmware does a magnitude-and-phase decision per bit. The DSP that matters most for noise immunity all happens **inside the FPGA**, before the ARM ever sees the data.

## 4. Walk-through: one ADC sample's journey during `hf 15 reader`

```
t = 0 ns       External ADC samples antenna voltage → 8-bit value, e.g. 0x9C
t = ~74 ns     value latched in corr_i_accum / corr_q_accum (added or subtracted
               depending on subcarrier_I / subcarrier_Q polarity)
               In parallel: avg128 delay-line shifts, FSK match16/28/32 update
               In parallel: hysteresis comparator toggles after_hysteresis

               …repeats for 64 ADC samples (4.72 µs total)…

t = 4.72 µs    corr_i_cnt == 0:
                 corr_amplitude    = max + min/2 of |I|,|Q|
                 fskout            = decision from FSK filter
                 corr_i_out, corr_q_out latched from the chosen output-packing
                 corr_i_accum, corr_q_accum reset to next sample

t = 4.72 µs +  ssp_frame pulses high, ssp_clk starts strobing.
   294 ns      Over the next 4.72 µs, 16 bits shift out of ssp_din at 3.39 MHz.
               PDC DMA on the ARM SSC writes one halfword into BigBuf.

t ≈ 9 µs       ARM main loop polls SSC; pulls the next 16-bit word; decodes it.
```

Every 4.72 µs the cycle repeats. Over a full ISO15693 reply (~1–4 ms), the FPGA produces ~200–800 reports, which the ARM decodes into one ATQB-equivalent / inventory response.

## 5. Where to look

| Concern | Files |
|---|---|
| ARM clock setup                                    | `common_arm/clocks.c`, `armsrc/appmain.c:3324`, `include/proxmark3_arm.h:30-67` |
| PCK0 to FPGA                                       | `armsrc/appmain.c:3324-3330` |
| FPGA logic-clock divider                           | `fpga/clk_divider.v` |
| FPGA top-level pin map / clock domains             | `fpga/fpga_pm3_top.v` |
| FPGA config-word opcodes & major/minor mode enum   | `fpga/define.v`, `armsrc/fpgaloader.h:108-160` |
| SSC setup (16/8-bit, RX from FPGA)                 | `armsrc/fpgaloader.c:145` (`FpgaSetupSsc`) |
| PDC (DMA) setup for SSC                            | `armsrc/fpgaloader.c:193` and around `AT91C_BASE_PDC_SSC` |
| HF reader DSP (correlators, FSK filter)            | `fpga/hi_reader.v` |
| LF datapath (clock divider + 1-bit sampling)       | `fpga/lo_read.v`, `fpga/lo_edge_detect.v`, `fpga/lo_passthru.v`, `fpga/lo_adc.v` |
| Filter helper (used by FeliCa)                     | `fpga/lp20khz_1MSa_iir_filter.v`, `fpga/min_max_tracker.v` |
| ARM-side SSC timebase (used as protocol clock)     | `armsrc/ticks.c` — `StartCountSspClk`, `GetCountSspClk` |
| GPIO pin map                                       | `include/config_gpio.h` |
| Atmel SoC register definitions                     | `include/at91sam7s512.h` |
