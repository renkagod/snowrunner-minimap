"""Dumps shader disassembly, signatures and bytecode for chosen events. Runs inside RenderDoc's Python:

    set RDOC_CAPTURE=...rdc  RDOC_OUT=...dir  RDOC_EVENTS=2741,3981  [RDOC_SAVE=rid:event,...]
    qrenderdoc.exe --python tools\\rdoc_shaders.py

Writes ev<id>_<stage>.txt (disassembly with signatures and resource bindings) and ev<id>_<stage>.dxbc.
RDOC_SAVE lists texture resource ids to save as PNG as they are after the given event.
"""

import os
import traceback

import renderdoc as rd

CAPTURE = os.environ["RDOC_CAPTURE"]
OUT = os.environ["RDOC_OUT"]
EVENTS = [int(e) for e in os.environ["RDOC_EVENTS"].split(",") if e]
SAVES = [s.split(":") for s in os.environ.get("RDOC_SAVE", "").split(",") if s]


def signature(params):
    return "\n".join(f"    {p.semanticName}{p.semanticIndex} reg {p.regIndex} comps {p.compCount} "
                     f"type {p.varType} sys {p.systemValue}" for p in params)


def main(log):
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(CAPTURE, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open capture")
    _, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    targets = controller.GetDisassemblyTargets(True)
    log.write(f"disassembly targets: {list(targets)}\n")
    resources = {str(r.resourceId): r.resourceId for r in controller.GetResources()}

    for eid in EVENTS:
        controller.SetFrameEvent(eid, True)
        state = controller.GetPipelineState()
        for stage, tag in ((rd.ShaderStage.Vertex, "vs"), (rd.ShaderStage.Pixel, "ps")):
            refl = state.GetShaderReflection(stage)
            if not refl:
                continue
            pipe = state.GetGraphicsPipelineObject()
            text = controller.DisassembleShader(pipe, refl, targets[0])
            with open(os.path.join(OUT, f"ev{eid}_{tag}.txt"), "w", encoding="utf-8") as f:
                f.write(f"shader {state.GetShader(stage)}\ninputs:\n{signature(refl.inputSignature)}\n")
                f.write(f"outputs:\n{signature(refl.outputSignature)}\nresources:\n")
                for r in refl.readOnlyResources:
                    f.write(f"    {r.name} t{r.fixedBindNumber} {r.textureType}\n")
                for s in refl.samplers:
                    f.write(f"    {s.name} s{s.fixedBindNumber}\n")
                f.write("\n" + text)
            with open(os.path.join(OUT, f"ev{eid}_{tag}.dxbc"), "wb") as f:
                f.write(bytes(refl.rawBytes))
            log.write(f"event {eid} {tag}: {len(refl.rawBytes)} bytes\n")

    for rid_text, eid in SAVES:
        controller.SetFrameEvent(int(eid), True)
        rid = resources.get(f"ResourceId::{rid_text}")
        if rid is None:
            log.write(f"no resource {rid_text}\n")
            continue
        save = rd.TextureSave()
        save.resourceId = rid
        save.destType = rd.FileType.PNG
        save.alpha = rd.AlphaMapping.Preserve
        controller.SaveTexture(save, os.path.join(OUT, f"res{rid_text}_ev{eid}.png"))
        log.write(f"saved {rid_text} at {eid}\n")

    controller.Shutdown()
    cap.Shutdown()


os.makedirs(OUT, exist_ok=True)
with open(os.path.join(OUT, "shaders.txt"), "w", encoding="utf-8") as f:
    try:
        main(f)
    except Exception:
        traceback.print_exc(file=f)
os._exit(0)
