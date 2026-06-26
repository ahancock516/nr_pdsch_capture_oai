#!/usr/bin/env python3
"""
Plot a single PDSCH capture. Layout mirrors nr_pusch_capture plot_capture.py (2x2).

Usage:
    python plot_capture.py [dataset_path] [capture_index] [output_path]
"""

import os
import sys
import struct
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

DEFAULT_DATASET = "plugins/nr_pdsch_capture/data/pdsch_dataset.bin"
MOD_NAMES = {2: "QPSK", 4: "16-QAM", 6: "64-QAM", 8: "256-QAM"}

HDR_FMT = "<4sBIHBHHHHBBHBBBBBI"
HDR_SIZE = struct.calcsize(HDR_FMT)
HDR_FIELDS = [
    "magic", "version", "capture_index",
    "frame", "slot", "rnti",
    "bwp_start", "rb_start", "nb_rbs",
    "start_symbol", "nb_symbols",
    "dmrs_symb_pos", "dmrs_config_type", "n_dmrs_cdm_groups",
    "qam_mod_order", "nb_layers", "nb_rx",
    "nb_re_per_sym",
]


class PDSCHDataset:
    def __init__(self, path):
        self.path = path
        self._index = []
        self._scan()

    def _scan(self):
        with open(self.path, "rb") as f:
            while True:
                offset = f.tell()
                raw = f.read(HDR_SIZE)
                if len(raw) < HDR_SIZE:
                    break
                vals = struct.unpack(HDR_FMT, raw)
                h = dict(zip(HDR_FIELDS, vals))
                if h["magic"] != b"PDSC":
                    break
                iq_bytes = h["nb_rx"] * h["nb_symbols"] * h["nb_re_per_sym"] * 4
                self._index.append(offset)
                f.seek(iq_bytes, 1)

    def __len__(self):
        return len(self._index)

    def __repr__(self):
        return f"PDSCHDataset('{self.path}', {len(self)} records)"

    def __getitem__(self, idx):
        with open(self.path, "rb") as f:
            f.seek(self._index[idx])
            raw = f.read(HDR_SIZE)
            h = dict(zip(HDR_FIELDS, struct.unpack(HDR_FMT, raw)))
            nb_rx, nb_sym, nb_re = h["nb_rx"], h["nb_symbols"], h["nb_re_per_sym"]
            iq_raw = f.read(nb_rx * nb_sym * nb_re * 4)

        iq_i16 = np.frombuffer(iq_raw, dtype=np.int16).reshape(nb_rx, nb_sym, nb_re, 2)
        iq = iq_i16[..., 0].astype(np.float32) + 1j * iq_i16[..., 1].astype(np.float32)
        return {"meta": h, "iq": iq}


def _dmrs_symbol_mask(meta):
    num_symbols  = meta["nb_symbols"]
    start_symbol = meta["start_symbol"]
    dmrs_mask    = meta["dmrs_symb_pos"]
    return np.array(
        [((dmrs_mask >> (start_symbol + s)) & 0x1) == 1 for s in range(num_symbols)],
        dtype=bool,
    )


def _dmrs_mod12_profile(iq, is_dmrs):
    """Normalized power at each subcarrier mod 12 within DMRS symbols."""
    dmrs_idx = np.where(is_dmrs)[0]
    if not len(dmrs_idx):
        return None
    dmrs_power = np.mean(np.abs(iq[dmrs_idx]) ** 2, axis=0)  # (nb_re,)
    profile = np.zeros(12)
    counts  = np.zeros(12)
    for re, p in enumerate(dmrs_power):
        profile[re % 12] += p
        counts[re % 12]  += 1
    counts  = np.maximum(counts, 1)
    profile /= counts
    peak = profile.max()
    if peak > 0:
        profile /= peak
    return profile


def _format_list(values):
    return ", ".join(str(int(v)) for v in values) if len(values) else "-"


def plot_capture(cap, output_path="capture_analysis.png", verbose=True):
    meta = cap["meta"]
    iq   = cap["iq"][0]     # (nb_sym, nb_re) — rx antenna 0

    num_symbols   = meta["nb_symbols"]
    nb_re_per_sym = meta["nb_re_per_sym"]
    start_symbol  = meta["start_symbol"]
    mod_name      = MOD_NAMES.get(meta["qam_mod_order"], f"Qm={meta['qam_mod_order']}")
    is_dmrs       = _dmrs_symbol_mask(meta)
    dmrs_symbols  = [start_symbol + s for s in range(num_symbols) if is_dmrs[s]]
    dmrs_profile  = _dmrs_mod12_profile(iq, is_dmrs)

    fig, axes = plt.subplots(2, 2, figsize=(15, 10), constrained_layout=True)
    fig.suptitle(
        f"PDSCH Capture #{meta['capture_index']} | "
        f"Frame {meta['frame']} Slot {meta['slot']} | "
        f"RNTI 0x{meta['rnti']:04X} | {mod_name} | {meta['nb_rbs']} PRBs",
        fontsize=14, fontweight="bold",
    )

    # (0,0) Flattened IQ waveform
    ax = axes[0, 0]
    iq_flat    = iq.reshape(-1)
    sample_idx = np.arange(iq_flat.size)
    ax.plot(sample_idx, iq_flat.real, color="steelblue", linewidth=0.7, label="I")
    ax.plot(sample_idx, iq_flat.imag, color="coral",     linewidth=0.7, label="Q")
    for s in range(1, num_symbols):
        ax.axvline(s * nb_re_per_sym, color="gray", linewidth=0.5, alpha=0.5)
    ax.set_title("Flattened IQ Waveform")
    ax.set_xlabel("Sample index")
    ax.set_ylabel("Amplitude")
    ax.grid(True, alpha=0.2)
    ax.legend(loc="upper right", fontsize=8)

    # (0,1) Resource grid power
    ax = axes[0, 1]
    power_db = 10.0 * np.log10(np.maximum(np.abs(iq) ** 2, 1e-12))
    image = ax.imshow(power_db.T, aspect="auto", origin="lower", cmap="viridis")
    fig.colorbar(image, ax=ax, pad=0.02, label="Power [dB]")
    for s in np.where(is_dmrs)[0]:
        ax.axvline(s, color="red", linewidth=1.2, alpha=0.7)
    ax.set_title("Resource Grid Power")
    ax.set_xlabel("Relative OFDM symbol")
    ax.set_ylabel("Allocated subcarrier")
    ax.set_xticks(range(num_symbols))
    ax.set_xticklabels([str(start_symbol + s) for s in range(num_symbols)], fontsize=8)

    # (1,0) DMRS comb profile
    ax = axes[1, 0]
    if dmrs_profile is None:
        ax.text(0.5, 0.5, "No DMRS symbols", ha="center", va="center")
        ax.set_axis_off()
    else:
        bins   = np.arange(12)
        colors = ["lightgray"] * 12
        # For DMRS Type 1: active bins are even (0,2,4,6,8,10)
        # For DMRS Type 2: active bins are 0,1,6,7 or 2,3,8,9
        # Colour the top-power bins red
        threshold = 0.5
        for i, p in enumerate(dmrs_profile):
            if p >= threshold:
                colors[i] = "tab:red"
        ax.bar(bins, dmrs_profile, color=colors, edgecolor="black", linewidth=0.4)
        ax.set_xticks(bins)
        ax.set_xlabel("RE index mod 12")
        ax.set_ylabel("Normalized DMRS power")
        ax.set_title("DMRS Comb Profile")
        ax.grid(True, axis="y", alpha=0.2)

    # (1,1) Per-symbol power + metadata text box
    ax = axes[1, 1]
    rel_symbols     = np.arange(num_symbols)
    colors_bar      = ["tab:red" if is_dmrs[s] else "steelblue" for s in rel_symbols]
    symbol_power_db = 10.0 * np.log10(
        np.maximum(np.mean(np.abs(iq) ** 2, axis=1), 1e-12)
    )
    ax.bar(rel_symbols, symbol_power_db, color=colors_bar, alpha=0.85)
    ax.set_title("Per-Symbol Power")
    ax.set_xlabel("Relative OFDM symbol")
    ax.set_ylabel("Average power [dB]")
    ax.set_xticks(rel_symbols)
    ax.set_xticklabels([str(start_symbol + s) for s in rel_symbols], fontsize=8)
    ax.grid(True, axis="y", alpha=0.2)

    info_lines = [
        f"DMRS symbols: {_format_list(dmrs_symbols)}",
        f"DMRS type: {meta['dmrs_config_type']}",
        f"CDM groups: {meta['n_dmrs_cdm_groups']}",
        f"Layers: {meta['nb_layers']}",
        f"PRBs (start): {meta['nb_rbs']} (RB {meta['rb_start']})",
        f"BWP start: {meta['bwp_start']}",
        f"RE per symbol: {meta['nb_re_per_sym']}",
    ]
    ax.text(
        1.02, 0.98,
        "\n".join(info_lines),
        transform=ax.transAxes,
        va="top", ha="left", fontsize=9,
        bbox={"boxstyle": "round", "facecolor": "white", "alpha": 0.9},
    )

    fig.savefig(output_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    if verbose:
        print(f"Saved {output_path}")


if __name__ == "__main__":
    dataset_path  = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DATASET
    capture_index = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    output_path   = sys.argv[3] if len(sys.argv) > 3 else \
        os.path.join(os.path.expanduser("~/Desktop"), f"pdsch_capture_{capture_index:04d}.png")

    dataset = PDSCHDataset(dataset_path)
    print(dataset)
    cap = dataset[capture_index]
    m   = cap["meta"]
    print(f"Plotting capture {capture_index}: frame={m['frame']}, slot={m['slot']}, "
          f"RNTI=0x{m['rnti']:04X}, {MOD_NAMES.get(m['qam_mod_order'], '?')}, "
          f"{m['nb_rbs']} PRBs, {m['nb_symbols']} symbols")
    plot_capture(cap, output_path)
