"""Reads texel values of textures at chosen events. Runs inside RenderDoc's Python:

    set RDOC_CAPTURE=...rdc  RDOC_OUT=...dir  RDOC_PICK=rid@event,...  RDOC_PIXELS=x:y,...
    qrenderdoc.exe --python tools\\rdoc_pick.py

Writes pick.txt with the RGBA float value of every texture at every pixel.
"""

import os
import traceback

import renderdoc as rd

CAPTURE = os.environ["RDOC_CAPTURE"]
OUT = os.environ["RDOC_OUT"]
PICKS = [p.split("@") for p in os.environ["RDOC_PICK"].split(",") if p]
PIXELS = [tuple(int(v) for v in p.split(":")) for p in os.environ["RDOC_PIXELS"].split(",") if p]


def main(log):
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(CAPTURE, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open capture")
    _, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    textures = {str(t.resourceId): t for t in controller.GetTextures()}
    for rid, eid in PICKS:
        controller.SetFrameEvent(int(eid), True)
        tex = textures[f"ResourceId::{rid}"]
        for x, y in PIXELS:
            v = controller.PickPixel(tex.resourceId, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
            log.write(f"{rid}@{eid} {x},{y}: {' '.join(f'{c:.4f}' for c in v.floatValue)}\n")
    controller.Shutdown()
    cap.Shutdown()


os.makedirs(OUT, exist_ok=True)
with open(os.path.join(OUT, "pick.txt"), "w", encoding="utf-8") as f:
    try:
        main(f)
    except Exception:
        traceback.print_exc(file=f)
os._exit(0)
