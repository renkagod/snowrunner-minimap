"""Lists API calls of an event range and dumps buffers. Runs inside RenderDoc's Python:

    set RDOC_CAPTURE=...rdc  RDOC_OUT=...dir  RDOC_RANGE=2651-3981  [RDOC_DUMP=rid:event,...]
    qrenderdoc.exe --python tools\\rdoc_calls.py

Writes calls.txt: every API call in the range with its resource arguments, plus the capture's
frame-start chunk list (resource creation counts). RDOC_DUMP saves buffer contents as
buf<rid>_ev<event>.bin as they are at that event.
"""

import os
import traceback

import renderdoc as rd

CAPTURE = os.environ["RDOC_CAPTURE"]
OUT = os.environ["RDOC_OUT"]
FIRST, LAST = (int(v) for v in os.environ["RDOC_RANGE"].split("-"))
DUMPS = [s.split(":") for s in os.environ.get("RDOC_DUMP", "").split(",") if s]


def flatten(obj, prefix=""):
    """Yields (name, value) for leaf members of a structured chunk."""
    if obj.NumChildren() == 0:
        yield prefix + obj.name, obj.data.basic
        return
    for i in range(obj.NumChildren()):
        yield from flatten(obj.GetChild(i), prefix + obj.name + ".")


def main(log):
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(CAPTURE, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("cannot open capture")
    _, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    sdfile = controller.GetStructuredFile()

    events = []
    pending = list(controller.GetRootActions())
    while pending:
        a = pending.pop()
        events.extend(a.events)
        pending.extend(a.children)
    events.sort(key=lambda e: e.eventId)
    for ev in events:
        if not FIRST <= ev.eventId <= LAST:
            continue
        chunk = sdfile.chunks[ev.chunkIndex]
        name = chunk.name
        if any(k in name for k in ("Map", "Unmap", "UpdateSubresource", "Copy", "Create")):
            args = []
            for i in range(chunk.NumChildren()):
                c = chunk.GetChild(i)
                if c.type.basetype == rd.SDBasic.Resource:
                    args.append(f"{c.name}={c.data.basic.id}")
                elif c.NumChildren() == 0 and c.type.basetype in (rd.SDBasic.UnsignedInteger, rd.SDBasic.SignedInteger):
                    args.append(f"{c.name}={c.data.basic.u}")
            log.write(f"{ev.eventId} {name} {' '.join(args)}\n")

    counts = {}
    for chunk in sdfile.chunks:
        counts[chunk.name] = counts.get(chunk.name, 0) + 1
    log.write("\n=== chunk counts\n")
    for name, n in sorted(counts.items(), key=lambda kv: -kv[1])[:40]:
        log.write(f"  {n} {name}\n")

    buffers = {str(b.resourceId): b for b in controller.GetBuffers()}
    for rid, eid in DUMPS:
        controller.SetFrameEvent(int(eid), True)
        b = buffers.get(f"ResourceId::{rid}")
        if b is None:
            log.write(f"no buffer {rid}\n")
            continue
        data = controller.GetBufferData(b.resourceId, 0, 0)
        with open(os.path.join(OUT, f"buf{rid}_ev{eid}.bin"), "wb") as f:
            f.write(bytes(data))
        log.write(f"dumped {rid} at {eid}: {len(data)} bytes\n")

    controller.Shutdown()
    cap.Shutdown()


os.makedirs(OUT, exist_ok=True)
with open(os.path.join(OUT, "calls.txt"), "w", encoding="utf-8") as f:
    try:
        main(f)
    except Exception:
        traceback.print_exc(file=f)
os._exit(0)
