"""Lists the draws of an event range with their geometry bindings. Runs inside RenderDoc's Python:

    set RDOC_CAPTURE=...rdc  RDOC_OUT=...dir  RDOC_RANGE=2741-3981
    qrenderdoc.exe --python tools\\rdoc_draws.py

Writes draws.txt: per draw the index/instance counts and offsets, shaders, vertex layout,
vertex/index buffer bindings (with buffer size and creation flags), bound textures and the
first floats of every constant buffer. Also lists every buffer of the capture by size.
"""

import os
import struct
import traceback

import renderdoc as rd

CAPTURE = os.environ["RDOC_CAPTURE"]
OUT = os.environ["RDOC_OUT"]
FIRST, LAST = (int(v) for v in os.environ["RDOC_RANGE"].split("-"))


def main(log):
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(CAPTURE, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open capture")
    _, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    sdfile = controller.GetStructuredFile()
    buffers = {b.resourceId: b for b in controller.GetBuffers()}
    textures = {t.resourceId: t for t in controller.GetTextures()}
    names = {r.resourceId: r.name for r in controller.GetResources()}

    actions = []
    pending = list(controller.GetRootActions())
    while pending:
        a = pending.pop()
        if a.flags & rd.ActionFlags.Drawcall and FIRST <= a.eventId <= LAST:
            actions.append(a)
        pending.extend(a.children)
    actions.sort(key=lambda a: a.eventId)

    def buf(rid):
        b = buffers.get(rid)
        return f"{names.get(rid, rid)} size {b.length} flags {b.creationFlags}" if b else str(rid)

    def tex(rid):
        t = textures.get(rid)
        return f"{names.get(rid, rid)} {t.width}x{t.height} {t.format.Name()}" if t else str(names.get(rid, rid))

    layouts_seen = set()
    for a in actions:
        controller.SetFrameEvent(a.eventId, True)
        state = controller.GetPipelineState()
        log.write(f"=== event {a.eventId}: {a.GetName(sdfile)} idx {a.numIndices} inst {a.numInstances} "
                  f"startIdx {a.indexOffset} baseVtx {a.baseVertex} startInst {a.instanceOffset}\n")
        vs = state.GetShader(rd.ShaderStage.Vertex)
        ps = state.GetShader(rd.ShaderStage.Pixel)
        log.write(f"  vs {vs} ps {ps}\n")
        attrs = state.GetVertexInputs()
        key = (str(vs), tuple((v.name, v.vertexBuffer, v.byteOffset, v.format.Name(), v.perInstance) for v in attrs))
        if key not in layouts_seen:
            layouts_seen.add(key)
            for v in attrs:
                log.write(f"    attr {v.name} vb{v.vertexBuffer} +{v.byteOffset} {v.format.Name()} "
                          f"{'instance' if v.perInstance else 'vertex'}\n")
        for i, vb in enumerate(state.GetVBuffers()):
            if vb.resourceId != rd.ResourceId.Null():
                log.write(f"  vb{i}: {buf(vb.resourceId)} stride {vb.byteStride} offset {vb.byteOffset}\n")
        ib = state.GetIBuffer()
        if ib.resourceId != rd.ResourceId.Null():
            log.write(f"  ib: {buf(ib.resourceId)} stride {ib.byteStride} offset {ib.byteOffset}\n")
        for used in state.GetReadOnlyResources(rd.ShaderStage.Pixel, True):
            log.write(f"  ps t{used.access.index}: {tex(used.descriptor.resource)}\n")
        refl = state.GetShaderReflection(rd.ShaderStage.Pixel)
        if refl:
            for i, cb in enumerate(refl.constantBlocks):
                if cb.name not in ("CB_INSTANCE", "CB_GLOBAL_OBJECT_BUFFER"):
                    continue
                bound = state.GetConstantBlock(rd.ShaderStage.Pixel, i, 0)
                d = bound.descriptor
                data = controller.GetBufferData(d.resource, d.byteOffset, min(d.byteSize, 160))
                floats = struct.unpack(f"<{len(data) // 4}f", data[: len(data) // 4 * 4])
                log.write(f"  {cb.name}: {' '.join(f'{v:.4g}' for v in floats)}\n")
                if a.eventId == actions[-1].eventId or len(layouts_seen) == 1:
                    for var in cb.variables:
                        log.write(f"      {var.name} @ {var.byteOffset}\n")

    log.write("\n=== buffers by size\n")
    for b in sorted(buffers.values(), key=lambda b: -b.length)[:60]:
        log.write(f"  {names.get(b.resourceId, b.resourceId)} {b.length} flags {b.creationFlags} gpu {b.gpuAddress}\n")

    controller.Shutdown()
    cap.Shutdown()


os.makedirs(OUT, exist_ok=True)
with open(os.path.join(OUT, "draws.txt"), "w", encoding="utf-8") as f:
    try:
        main(f)
    except Exception:
        traceback.print_exc(file=f)
os._exit(0)
