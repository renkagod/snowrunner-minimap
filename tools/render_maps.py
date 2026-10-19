"""Renders minimap background images from SnowRunner level paks.

Each level_*.pak is a zip holding .pct terrain textures: an 88-byte header followed by raw texels.
The height map (16-bit) gives a shaded relief, the extrude map (8-bit) marks mud and dirt roads.
Output per level: <level>.png plus <level>.ini with the world rectangle the image covers.

    python tools/render_maps.py --paks "<game>/preload/paks/client" --out "<game>/Sources/Bin/SnowMinimap/maps"
"""

import argparse
import fnmatch
import struct
import sys
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image

PCT_HEADER_SIZE = 88
METERS_PER_TEXEL = 0.5  # assumed: every surveyed height map is 2^n*k + 1 texels at 0.5 m


def read_pct(pak: zipfile.ZipFile, level: str, name: str, dtype) -> np.ndarray | None:
    entry = f"[textures]/pct/{level}_{name}.pct"
    try:
        data = pak.read(entry)
    except KeyError:
        return None
    width, height = struct.unpack_from("<II", data, 16)
    texels = np.frombuffer(data, dtype, count=width * height, offset=PCT_HEADER_SIZE)
    return texels.reshape(height, width)


def render(pak_path: Path, out_dir: Path, max_size: int) -> bool:
    level = pak_path.stem
    with zipfile.ZipFile(pak_path) as pak:
        heights = read_pct(pak, level, "height_map", "<u2")
        mud = read_pct(pak, level, "extrude_map", np.uint8)
    if heights is None:
        return False

    rows, cols = heights.shape
    scale = min(1.0, max_size / max(rows, cols))
    size = (max(1, round(cols * scale)), max(1, round(rows * scale)))

    def resample(a: np.ndarray) -> np.ndarray:
        return np.asarray(Image.fromarray(a.astype(np.float32)).resize(size, Image.BILINEAR))

    h = resample(heights)
    m = resample(mud) / 255.0 if mud is not None and mud.shape == heights.shape else np.zeros_like(h)

    # Relief lit from the north-west. Height units are not metres; the factor only sets contrast.
    gy, gx = np.gradient(h * 0.02 / scale)
    shade = np.clip((-gx * -0.6 + -gy * -0.6 + 0.55) / np.sqrt(gx * gx + gy * gy + 1), 0, 1)
    t = (h - h.min()) / (np.ptp(h) + 1e-6)
    ground = np.stack([0.42 + 0.25 * t, 0.52 + 0.18 * t, 0.36 + 0.20 * t], -1)
    mud_color = np.array([0.55, 0.42, 0.28])
    mix = np.clip(m, 0, 1)[..., None] * 0.85
    color = (ground * (1 - mix) + mud_color * mix) * (0.45 + 0.75 * shade[..., None])

    out_dir.mkdir(parents=True, exist_ok=True)
    Image.fromarray((np.clip(color, 0, 1) * 255).astype(np.uint8)).save(out_dir / f"{level}.png")
    # Shading alone, multiplied over the game's own terrain colour when the mod finds it.
    Image.fromarray((np.clip(shade, 0, 1) * 255).astype(np.uint8)).save(out_dir / f"{level}_relief.png")

    # Image row 0 is assumed to be the +Z (north) edge, column 0 the -X (west) edge.
    x_span = (cols - 1) * METERS_PER_TEXEL
    z_span = (rows - 1) * METERS_PER_TEXEL
    (out_dir / f"{level}.ini").write_text(
        "[map]\n"
        f"x_left={-x_span / 2:g}\n"
        f"z_top={z_span / 2:g}\n"
        f"x_span={x_span:g}\n"
        f"z_span={-z_span:g}\n",
        encoding="ascii",
    )
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--paks", type=Path, required=True, help="folder with level_*.pak files")
    parser.add_argument("--out", type=Path, required=True, help="SnowMinimap maps folder")
    parser.add_argument("--size", type=int, default=2048, help="longest side of the output image")
    parser.add_argument("--levels", default="level_*", help="glob for level names, e.g. level_us_01_*")
    args = parser.parse_args()

    paks = sorted(p for p in args.paks.glob("level_*.pak") if fnmatch.fnmatch(p.stem, args.levels))
    if not paks:
        print(f"no level paks matching {args.levels} in {args.paks}", file=sys.stderr)
        return 1
    for pak in paks:
        ok = render(pak, args.out, args.size)
        print(f"{pak.stem}: {'ok' if ok else 'no terrain'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
