# nr_pdsch_capture

OAI nrUE plugin that captures frequency-domain IQ from the PDSCH receive path for ML and neural receiver research.

---

## Overview

The plugin hooks into `nr_ue_pdsch_procedures()` after channel estimation, before equalization. It captures the raw frequency-domain IQ (`rxdataF`) for every accepted PDSCH slot and writes a labelled binary dataset. The hook point mirrors the gNB-side [nr_pusch_capture](https://github.com/ahancock516/nr_pusch_capture_oai) plugin — raw IQ at the same layer, downlink instead of uplink.

**Three quality filters gate each capture:**

1. **DMRS window** — at least one DMRS symbol must fall within the slot allocation.
2. **DMRS comb power ratio** — active RE power / quiet RE power ≥ 1.5× (skipped when no inter-CDM-group quiet bins are available, e.g. DMRS Type 1 with 1 CDM group).
3. **Duplicate hash** — FNV1a-64 of the IQ payload; repeated captures are discarded.

Captures accumulate in a timestamped subfolder under `data/` so successive sessions never overwrite each other.

---

## Repository Structure

```
plugins/nr_pdsch_capture/
├── CMakeLists.txt
├── capture_config.txt
├── src/
│   └── nr_pdsch_capture.c
├── scripts/
│   ├── plot_capture.py
│   ├── read_dataset.py
│   └── generate_capture_video.py
└── data/
    └── YYYYMMDD_HHMMSS/
        ├── pdsch_dataset.bin
        ├── pdsch_capture_NNNN.png
        └── pdsch_dataset.mp4
```

---

## Integration with OAI

Clone this repo into the OAI source tree at `plugins/nr_pdsch_capture/`:

```bash
cd openairinterface5g
git clone https://github.com/ahancock516/nr_pdsch_capture_oai plugins/nr_pdsch_capture
```

Add the following to the OAI root `CMakeLists.txt` (after the `dfts` library target):

```cmake
# PDSCH capture plugin
add_subdirectory(${CMAKE_SOURCE_DIR}/plugins/nr_pdsch_capture
                 ${CMAKE_BINARY_DIR}/nr_pdsch_capture)

target_compile_definitions(SCHED_NR_UE_LIB PRIVATE
    PDSCH_CAPTURE_SO_PATH="${CMAKE_BINARY_DIR}/libpdsch_capture.so"
)
```

The plugin also requires a hook in `openair1/SCHED_NR_UE/phy_procedures_nr_ue.c`. See [INTEGRATION.md](INTEGRATION.md) for the exact patch.

---

## Build

```bash
cd openairinterface5g/cmake_targets/ran_build/build
cmake ..
make nr-uesoftmodem pdsch_capture -j$(nproc)
```

The plugin loads at runtime via `dlopen` — no install step required. The `.so` path is embedded at compile time via `PDSCH_CAPTURE_SO_PATH`.

---

## Configuration

Edit `plugins/nr_pdsch_capture/capture_config.txt`:

```
# output_file path is set at build time to <repo>/data/pdsch_dataset.bin.
# To override, add: output_file /your/custom/path.bin

max_captures  5000
```

Each session creates a fresh timestamped folder:
`data/YYYYMMDD_HHMMSS/pdsch_dataset.bin`

---

## Dataset Format

Each record is a fixed header followed by a raw IQ block:

```
[pdsch_capture_hdr_t]  33 bytes
[IQ block]             nb_rx × nb_symbols × nb_re_per_sym × 4 bytes (c16_t)
```

Header fields (little-endian, packed):

| Field               | Type     | Description                        |
|---------------------|----------|------------------------------------|
| `magic`             | `u8[4]`  | `"PDSC"`                           |
| `version`           | `u8`     | Format version (1)                 |
| `capture_index`     | `u32`    | Session-scoped capture counter     |
| `frame`             | `u16`    | OAI frame number                   |
| `slot`              | `u8`     | Slot index within frame            |
| `rnti`              | `u16`    | C-RNTI or SI-RNTI                  |
| `bwp_start`         | `u16`    | BWP start RB                       |
| `rb_start`          | `u16`    | Allocation start RB                |
| `nb_rbs`            | `u16`    | Number of allocated RBs            |
| `start_symbol`      | `u8`     | First OFDM symbol                  |
| `nb_symbols`        | `u8`     | Number of OFDM symbols             |
| `dmrs_symb_pos`     | `u16`    | DMRS symbol bitmap                 |
| `dmrs_config_type`  | `u8`     | DMRS type (1 or 2)                 |
| `n_dmrs_cdm_groups` | `u8`     | Number of CDM groups               |
| `qam_mod_order`     | `u8`     | Modulation order (2/4/6/8)         |
| `nb_layers`         | `u8`     | Number of spatial layers           |
| `nb_rx`             | `u8`     | Number of receive antennas         |
| `nb_re_per_sym`     | `u32`    | Allocated REs per symbol           |

---

## Analysis Scripts

**Plot a single capture:**
```bash
python3 scripts/plot_capture.py data/YYYYMMDD_HHMMSS/pdsch_dataset.bin [index]
```
Produces a 2×2 figure (IQ waveform, resource grid power, DMRS comb profile, per-symbol power) saved alongside the dataset.

**Inspect dataset contents:**
```bash
python3 scripts/read_dataset.py data/YYYYMMDD_HHMMSS/pdsch_dataset.bin
```

**Generate an MP4 video:**
```bash
python3 scripts/generate_capture_video.py data/YYYYMMDD_HHMMSS/pdsch_dataset.bin \
    --fps 4 --start 0 --count 500
```
Output video is saved alongside the dataset as `pdsch_dataset.mp4`.

---

## Notes

- The hook must be placed **post channel-estimation**. A pre-CE hook adds file I/O on the real-time receive path and causes HARQ timing violations in SA mode.
- `rxdataF` is unchanged by channel estimation (CE writes only to `pdsch_dl_ch_estimates`), so the captured IQ is identical to what a pre-CE hook would produce.
- In phy-test mode (`--phy-test`), `dmrs_ports` is 0; the plugin defaults to port 0.
