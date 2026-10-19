"""Summarises a RenderDoc capture as render passes. Runs inside RenderDoc's own Python:

    set RDOC_CAPTURE=...\\snowrunner_frame0.rdc
    set RDOC_OUT=...\\out_dir
    set RDOC_EXE_BASE=0x7ff76d110000      (SnowRunner.exe base from SnowMinimap.log, for callstacks)
    qrenderdoc.exe --python tools\\rdoc_passes.py

A pass is a run of consecutive draws/dispatches/clears that write the same output targets.
Writes passes.txt (one block per pass, with the game's return addresses for its first draw) and
a PNG of each pass's first colour target after the pass finished.
"""

import os
import sys
import traceback

import renderdoc as rd

CAPTURE = os.environ["RDOC_CAPTURE"]
OUT = os.environ["RDOC_OUT"]
EXE_BASE = int(os.environ.get("RDOC_EXE_BASE", "0"), 16)
EXE_SIZE = 0x2DC9000
SAVE_LIMIT = int(os.environ.get("RDOC_SAVE_LIMIT", "400"))

WORK_FLAGS = rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch | rd.ActionFlags.Clear | rd.ActionFlags.Copy


def flatten(actions, out):
    for a in actions:
        out.append(a)
        flatten(a.children, out)
    return out


def exe_frames(sdfile, action):
    """Return addresses inside SnowRunner.exe for the API call that issued this action."""
    for ev in action.events:
        if ev.eventId != action.eventId:
            continue
        stack = sdfile.chunks[ev.chunkIndex].metadata.callstack
        return [f"exe+{a - EXE_BASE:#x}" for a in stack if EXE_BASE and EXE_BASE <= a < EXE_BASE + EXE_SIZE]
    return []


def main():
    os.makedirs(OUT, exist_ok=True)
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(CAPTURE, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open capture")
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError(f"cannot replay capture: {result}")

    sdfile = controller.GetStructuredFile()
    textures = {t.resourceId: t for t in controller.GetTextures()}
    names = {r.resourceId: r.name for r in controller.GetResources()}

    def describe(rid):
        if rid == rd.ResourceId.Null():
            return "-"
        t = textures.get(rid)
        size = f"{t.width}x{t.height} {t.format.Name()}" if t else "?"
        return f"{names.get(rid, rid)} [{size}]"

    actions = [a for a in flatten(controller.GetRootActions(), []) if a.flags & WORK_FLAGS]
    passes = []
    for a in actions:
        key = (tuple(o for o in a.outputs if o != rd.ResourceId.Null()), a.depthOut)
        if not passes or passes[-1]["key"] != key:
            passes.append({"key": key, "actions": []})
        passes[-1]["actions"].append(a)

    saved = 0
    with open(os.path.join(OUT, "passes.txt"), "w", encoding="utf-8") as report:
        report.write(f"{CAPTURE}\n{len(actions)} actions, {len(passes)} passes\n\n")
        for i, p in enumerate(passes):
            first, last = p["actions"][0], p["actions"][-1]
            draws = sum(1 for a in p["actions"] if a.flags & rd.ActionFlags.Drawcall)
            outputs, depth = p["key"]
            report.write(f"pass {i}: events {first.eventId}-{last.eventId}, {len(p['actions'])} actions, {draws} draws\n")
            for o in outputs:
                report.write(f"  colour {describe(o)}\n")
            report.write(f"  depth  {describe(depth)}\n")
            report.write(f"  first  {first.GetName(sdfile)}\n")
            frames = exe_frames(sdfile, first)
            if frames:
                report.write(f"  stack  {' '.join(frames[:12])}\n")

            if outputs and saved < SAVE_LIMIT:
                controller.SetFrameEvent(last.eventId, True)
                save = rd.TextureSave()
                save.resourceId = outputs[0]
                save.destType = rd.FileType.PNG
                save.alpha = rd.AlphaMapping.Discard
                path = os.path.join(OUT, f"pass{i:03d}_ev{last.eventId}.png")
                if controller.SaveTexture(save, path) == rd.ResultCode.Succeeded:
                    report.write(f"  image  {os.path.basename(path)}\n")
                    saved += 1
            report.write("\n")

    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    with open(os.path.join(OUT, "error.txt"), "w", encoding="utf-8") as f:
        traceback.print_exc(file=f)
finally:
    os._exit(0)  # do not let qrenderdoc open its window afterwards
