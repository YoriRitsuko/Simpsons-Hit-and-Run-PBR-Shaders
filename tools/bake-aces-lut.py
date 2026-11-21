"""Bake the ACES 1.3 SDR sRGB output used by the Direct3D 9 shader.

Run with Python 3 and OpenColorIO 2.4.2. The resulting resource is included
in the source package so the native build does not need either dependency.
"""

from math import log2
from pathlib import Path
from struct import pack

import PyOpenColorIO as ocio


SIZE = 64
WHITE = 64.0
SCALE = 32.0
CONFIG = "cg-config-v2.2.0_aces-v1.3_ocio-v2.4"


def bake():
    config = ocio.Config.CreateFromBuiltinConfig(CONFIG)
    display = ocio.DisplayViewTransform()
    display.setSrc("Linear Rec.709 (sRGB)")
    display.setDisplay("sRGB - Display")
    display.setView("ACES 1.0 - SDR Video")
    gamut = ocio.LookTransform()
    gamut.setSrc("Linear Rec.709 (sRGB)")
    gamut.setDst("Linear Rec.709 (sRGB)")
    gamut.setLooks("ACES 1.3 Reference Gamut Compression")
    transform = ocio.GroupTransform()
    transform.appendTransform(gamut)
    transform.appendTransform(display)
    processor = config.getProcessor(transform).getDefaultCPUProcessor()

    log_range = log2(1 + SCALE * WHITE)
    axis = [(2 ** (log_range * i / (SIZE - 1)) - 1) / SCALE for i in range(SIZE)]
    output = bytearray()
    for b in axis:
        for g in axis:
            for r in axis:
                rgb = processor.applyRGB([r, g, b])
                output += pack("<4e", *(min(1.0, max(0.0, x)) for x in rgb), 1.0)
    destination = Path(__file__).resolve().parents[1] / "resources" / "Aces13Srgb.rgba16f"
    destination.parent.mkdir(exist_ok=True)
    destination.write_bytes(output)
    print(f"Baked {SIZE}^3 ACES 1.3 sRGB samples to {destination}")


if __name__ == "__main__":
    bake()
