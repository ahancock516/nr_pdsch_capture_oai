#!/usr/bin/env python3
"""
read_dataset.py — reads the binary PDSCH capture dataset written by
nr_pdsch_capture.c (format v1).

Each record:
  [pdsch_capture_hdr_t]
  [IQ block]  nb_rx * nb_symbols * nb_re_per_sym  complex64 (c16_t pairs)
"""

import struct
import numpy as np
from pathlib import Path

MAGIC = b"PDSC"

HDR_FMT = "<4sBIHBHHHHBBHBBBBBI"
HDR_FIELDS = [
    "magic", "version", "capture_index",
    "frame", "slot", "rnti",
    "bwp_start", "rb_start", "nb_rbs",
    "start_symbol", "nb_symbols",
    "dmrs_symb_pos", "dmrs_config_type", "n_dmrs_cdm_groups",
    "qam_mod_order", "nb_layers", "nb_rx",
    "nb_re_per_sym",
]
HDR_SIZE = struct.calcsize(HDR_FMT)


def _parse_hdr(raw: bytes) -> dict:
    vals = struct.unpack(HDR_FMT, raw)
    return dict(zip(HDR_FIELDS, vals))


def read_records(path: str):
    """Yields (header_dict, iq_array) for each record."""
    with open(path, "rb") as f:
        while True:
            raw = f.read(HDR_SIZE)
            if not raw:
                break
            if len(raw) < HDR_SIZE:
                print(f"[WARN] truncated header at offset {f.tell() - len(raw)}")
                break
            hdr = _parse_hdr(raw)
            if hdr["magic"] != MAGIC:
                print(f"[WARN] bad magic {hdr['magic']} — stream desynced")
                break

            nb_rx   = hdr["nb_rx"]
            nb_syms = hdr["nb_symbols"]
            nb_re   = hdr["nb_re_per_sym"]

            iq_count = nb_rx * nb_syms * nb_re
            iq_raw   = f.read(iq_count * 4)   # c16_t = 2 x int16 = 4 bytes

            if len(iq_raw) < iq_count * 4:
                print(f"[WARN] truncated payload at capture {hdr['capture_index']}")
                break

            iq_i16 = np.frombuffer(iq_raw, dtype=np.int16).reshape(nb_rx, nb_syms, nb_re, 2)
            iq = iq_i16[..., 0].astype(np.float32) + 1j * iq_i16[..., 1].astype(np.float32)

            yield hdr, iq


def print_summary(path: str):
    n = 0
    for hdr, iq in read_records(path):
        n += 1
        if n == 1 or n % 500 == 0:
            print(f"  #{hdr['capture_index']:5d}  f={hdr['frame']:4d} s={hdr['slot']:2d} "
                  f"rnti=0x{hdr['rnti']:04x}  rb={hdr['rb_start']}+{hdr['nb_rbs']}  "
                  f"syms={hdr['nb_symbols']}  qam={hdr['qam_mod_order']}  "
                  f"nl={hdr['nb_layers']}  iq={iq.shape}")
    print(f"Total records: {n}")


if __name__ == "__main__":
    import sys
    path = sys.argv[1] if len(sys.argv) > 1 else "data/pdsch_dataset.bin"
    print(f"Reading {path} ...")
    print_summary(path)
