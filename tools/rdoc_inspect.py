"""Dumps pipeline details for chosen events of a RenderDoc capture. Runs inside RenderDoc's Python:

    set RDOC_CAPTURE=...rdc  RDOC_OUT=...dir  RDOC_EVENTS=242,263  [RDOC_EXE_BASE=0x...]
    qrenderdoc.exe --python tools\\rdoc_inspect.py

Writes inspect.txt: viewport, shaders, bound pixel/vertex shader resources, constant buffer
contents and the API callstack for each event, and saves each input texture as PNG.
"""

import os
import struct
import traceback

import renderdoc as rd

CAPTURE = os.environ["RDOC_CAPTURE"]
OUT = os.environ["RDOC_OUT"]
EVENTS = [int(e) for e in os.environ["RDOC_EVENTS"].split(",")]
EXE_BASE = int(os.environ.get("RDOC_EXE_BASE", "0"), 16)


def main(log):
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(CAPTURE, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open capture")
    _, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    sdfile = controller.GetStructuredFile()
    textures = {t.resourceId: t for t in controller.GetTextures()}
    names = {r.resourceId: r.name for r in controller.GetResources()}
    by_event = {}
    pending = list(controller.GetRootActions())
    while pending:
        a = pending.pop()
        by_event[a.eventId] = a
        pending.extend(a.children)

    def tex(rid):
        t = textures.get(rid)
        return f"{names.get(rid, rid)} {t.width}x{t.height} mips {t.mips} {t.format.Name()}" if t else str(names.get(rid, rid))

    for eid in EVENTS:
        controller.SetFrameEvent(eid, True)
        action = by_event.get(eid)
        state = controller.GetPipelineState()
        log.write(f"=== event {eid}: {action.GetName(sdfile) if action else '?'}\n")
        if action:
            log.write(f"  indices {action.numIndices} instances {action.numInstances}\n")
            for ev in action.events:
                stack = sdfile.chunks[ev.chunkIndex].metadata.callstack
                frames = [f"exe+{a - EXE_BASE:#x}" if EXE_BASE <= a < EXE_BASE + 0x2DC9000 else f"{a:#x}" for a in stack]
                log.write(f"  callstack({len(stack)}) {' '.join(frames[:16])}\n")
        for vp in [state.GetViewport(0)]:
            log.write(f"  viewport {vp.x},{vp.y} {vp.width}x{vp.height}\n")
        for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel):
            refl = state.GetShaderReflection(stage)
            log.write(f"  {stage} shader {state.GetShader(stage)} entry {refl.entryPoint if refl else '-'}\n")
            for used in state.GetReadOnlyResources(stage, True):
                rid = used.descriptor.resource
                log.write(f"    srv t{used.access.index}: {tex(rid)}\n")
                if rid in textures and stage == rd.ShaderStage.Pixel:
                    save = rd.TextureSave()
                    save.resourceId = rid
                    save.destType = rd.FileType.PNG
                    save.alpha = rd.AlphaMapping.Discard
                    controller.SaveTexture(save, os.path.join(OUT, f"ev{eid}_ps_t{used.access.index}.png"))
            if refl:
                for i, cb in enumerate(refl.constantBlocks):
                    bound = state.GetConstantBlock(stage, i, 0)
                    data = controller.GetBufferData(bound.descriptor.resource, bound.descriptor.byteOffset,
                                                    min(bound.descriptor.byteSize, 256)) if bound.descriptor.resource != rd.ResourceId.Null() else b""
                    floats = struct.unpack(f"<{len(data) // 4}f", data[: len(data) // 4 * 4]) if data else ()
                    log.write(f"    cb{i} {cb.name}: {' '.join(f'{v:.4g}' for v in floats[:64])}\n")
                    for var in cb.variables:
                        log.write(f"      {var.name} @ {var.byteOffset}\n")
        log.write("\n")

    controller.Shutdown()
    cap.Shutdown()


os.makedirs(OUT, exist_ok=True)
with open(os.path.join(OUT, "inspect.txt"), "w", encoding="utf-8") as f:
    try:
        main(f)
    except Exception:
        traceback.print_exc(file=f)
os._exit(0)
