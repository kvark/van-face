#!/usr/bin/env python3
"""Extract evenly-spaced rotation frames from a Vangers shop video (mechNN.avi),
composite onto black, quantize to Pebble's 64-color palette, and write them as
numbered PNGs suitable for use as Pebble `png` resources.

The original Vangers shop videos store the mech on a partially-transparent
backdrop using palette indices that are pure black RGB but with alpha. We
flatten that onto a black background so the mech reads against Pebble's dark
default. Each output frame is paletted (mode P) with the pebble64 palette so
the Pebble PNG converter doesn't have to re-quantize and lose detail.

Why not APNG?
  PebbleOS exposes gbitmap_sequence_* for animated PNG playback, but the
  upstream APNG decoder is fragile against PIL/ffmpeg/apngasm output (silent
  "gbitmap_sequence failed to update bitmap" runtime errors). The reliable
  path is to ship N individual PNG resources and swap bitmaps in the
  watchface's tick handler. See pebble-apng-pitfalls memory.

Usage:
  build_mech_frames.py <video> <out_dir> [--frames N --width W --height H]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageEnhance


# pebble64: every channel is 2 bits, so allowed values are {0, 85, 170, 255}.
PEBBLE64_PALETTE = [
    v for i in range(64)
    for v in (((i >> 4) & 0x3) * 85, ((i >> 2) & 0x3) * 85, (i & 0x3) * 85)
]
_PADDED_PALETTE = PEBBLE64_PALETTE + [0] * (256 * 3 - len(PEBBLE64_PALETTE))


def extract_frames(video: Path, tmp: Path, n_frames: int, width: int, height: int,
                   zoom: float) -> list[Path]:
    step = max(1, 60 // n_frames)
    # Source videos are 340×255 (landscape). Scale to fit the target size with
    # `zoom` applied as an extra magnification, then center-crop the overshoot.
    # zoom=1.0 → exact aspect-fit (output 200×150 from source 340×255).
    # zoom>1.0 → mech bigger in frame, edges of the turntable arc clipped.
    scaled_w = round(width * zoom)
    scaled_h = round(height * zoom)
    vf = (
        "format=rgba,split[fg][bg];[bg]drawbox=c=black:t=fill[bgb];"
        f"[bgb][fg]overlay=format=auto,select='not(mod(n,{step}))',"
        f"scale={scaled_w}:{scaled_h}:flags=lanczos:force_original_aspect_ratio=increase,"
        f"crop={width}:{height}:(iw-{width})/2:(ih-{height})/2"
    )
    subprocess.run(
        ["ffmpeg", "-y", "-i", str(video), "-vf", vf, "-vsync", "vfr",
         str(tmp / "frame_%02d.png")],
        check=True, capture_output=True,
    )
    return sorted(tmp.glob("frame_*.png"))


def quantize_to_pebble64(im: Image.Image, brightness: float, dither: bool) -> Image.Image:
    rgb = im.convert("RGB")
    if brightness != 1.0:
        rgb = ImageEnhance.Brightness(rgb).enhance(brightness)
    palette_image = Image.new("P", (1, 1))
    palette_image.putpalette(_PADDED_PALETTE)
    # Floyd-Steinberg against the sparse pebble64 grid pushes pixels toward the
    # darker entries (most palette steps are darker than mid-grey). NONE keeps
    # the mech crisp.
    method = Image.Dither.FLOYDSTEINBERG if dither else Image.Dither.NONE
    return rgb.quantize(palette=palette_image, dither=method)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("video", type=Path, help="e.g. mech06.avi (Iron Shadow)")
    ap.add_argument("out_dir", type=Path, help="output dir for mech_NN.png")
    ap.add_argument("--frames", type=int, default=8)
    ap.add_argument("--width", type=int, default=200)
    ap.add_argument("--height", type=int, default=150)
    ap.add_argument("--prefix", default="mech")
    ap.add_argument("--zoom", type=float, default=1.25,
                    help="scale factor applied before center-crop; >1 makes the mech "
                         "bigger in frame at the cost of cropping turntable edges")
    ap.add_argument("--brightness", type=float, default=1.4,
                    help="multiplier applied before quantization; >1 lifts the mid-tones "
                         "that pebble64 would otherwise crush to black")
    ap.add_argument("--dither", action="store_true",
                    help="enable Floyd-Steinberg dithering (default: off, cleaner look)")
    args = ap.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        extracted = extract_frames(args.video, Path(tmp), args.frames, args.width, args.height, args.zoom)
        # ffmpeg may overshoot by one when the selection filter rounds; take exactly N.
        for i, src in enumerate(extracted[: args.frames]):
            quant = quantize_to_pebble64(Image.open(src), args.brightness, args.dither)
            out = args.out_dir / f"{args.prefix}_{i + 1:02d}.png"
            quant.save(out)
            print(f"wrote {out} ({out.stat().st_size} bytes)", file=sys.stderr)


if __name__ == "__main__":
    main()
