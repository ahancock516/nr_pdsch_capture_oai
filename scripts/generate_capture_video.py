#!/usr/bin/env python3
"""
Render each capture in a PDSCH dataset as one video frame and encode an MP4.

Usage:
    python generate_capture_video.py
    python generate_capture_video.py path/to/pdsch_dataset.bin
    python generate_capture_video.py path/to/pdsch_dataset.bin --output capture_video.mp4
    python generate_capture_video.py path/to/pdsch_dataset.bin --fps 5 --start 0 --count 200
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(__file__))
from plot_capture import PDSCHDataset, plot_capture

DEFAULT_DATASET = "plugins/nr_pdsch_capture/data/pdsch_dataset.bin"
DEFAULT_FPS     = 2.0
FRAME_PATTERN   = "frame_%06d.png"


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate an MP4 video using one frame per PDSCH capture."
    )
    parser.add_argument(
        "dataset_path",
        nargs="?",
        default=DEFAULT_DATASET,
        help=f"Path to pdsch_dataset.bin (default: {DEFAULT_DATASET})",
    )
    parser.add_argument(
        "-o", "--output",
        help="Output MP4 path (default: dataset path with .mp4 suffix)",
    )
    parser.add_argument(
        "--fps",
        type=float,
        default=DEFAULT_FPS,
        help="Frames per second (default: 2.0 = 0.5 s per frame)",
    )
    parser.add_argument(
        "--start",
        type=int,
        default=0,
        help="First capture index to render (default: 0)",
    )
    parser.add_argument(
        "--count",
        type=int,
        default=None,
        help="Number of captures to render (default: all)",
    )
    return parser.parse_args(argv)


def find_ffmpeg():
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        raise RuntimeError("ffmpeg not found in PATH — install it with: sudo apt install ffmpeg")
    return ffmpeg


def render_frames(dataset, start, count, frame_dir):
    total = count if count is not None else len(dataset) - start
    total = min(total, len(dataset) - start)
    if total <= 0:
        raise ValueError(f"No captures to render (dataset has {len(dataset)} records, start={start})")

    print(f"Rendering {total} frames (captures {start}–{start + total - 1}) ...")
    for i in range(total):
        cap        = dataset[start + i]
        frame_path = str(frame_dir / (FRAME_PATTERN % i))
        plot_capture(cap, frame_path, verbose=False)
        if total <= 10 or i == total - 1 or (i + 1) % 50 == 0:
            print(f"  frame {i + 1}/{total}  (capture #{start + i})")

    return total


def encode_video(frame_dir, output_path, fps):
    ffmpeg      = find_ffmpeg()
    frame_pattern = str(frame_dir / FRAME_PATTERN)
    cmd = [
        ffmpeg, "-y",
        "-framerate", str(fps),
        "-i",         frame_pattern,
        "-vf",        "pad=ceil(iw/2)*2:ceil(ih/2)*2",
        "-c:v",       "libx264",
        "-pix_fmt",   "yuv420p",
        str(output_path),
    ]
    subprocess.run(cmd, check=True)


def main(argv=None):
    find_ffmpeg()
    args = parse_args(argv)

    if args.fps <= 0:
        raise ValueError(f"FPS must be positive, got {args.fps}")

    dataset_path = Path(args.dataset_path).resolve()
    if not dataset_path.is_file():
        raise FileNotFoundError(f"Dataset not found: {dataset_path}")

    output_path = (
        Path(args.output).resolve()
        if args.output
        else dataset_path.with_suffix(".mp4")
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)

    dataset = PDSCHDataset(str(dataset_path))
    print(f"{dataset}")

    with tempfile.TemporaryDirectory(prefix="pdsch_capture_video_") as tmp:
        frame_dir = Path(tmp)
        total     = render_frames(dataset, args.start, args.count, frame_dir)
        encode_video(frame_dir, output_path, args.fps)

    print(f"Encoded {total} frames → {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
