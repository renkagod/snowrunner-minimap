"""Asks a running game with RenderDoc loaded to capture the next frame, over RenderDoc's
target-control connection (no key presses needed). Runs inside RenderDoc's Python:

    set RDOC_OUT=...\\out_dir
    qrenderdoc.exe --python tools\\rdoc_trigger.py

Writes trigger.txt with the saved capture path, or the error.
"""

import os
import time
import traceback

import renderdoc as rd

OUT = os.environ["RDOC_OUT"]


def main(log):
    ident = rd.EnumerateRemoteTargets("localhost", 0)
    if not ident:
        log.write("no RenderDoc target running\n")
        return
    target = rd.CreateTargetControl("localhost", ident, "snowminimap-trigger", True)
    if target is None:
        log.write(f"cannot connect to target {ident}\n")
        return
    log.write(f"connected to {target.GetTarget()} pid {target.GetPID()}\n")
    # On connect the target replays notices about earlier captures; drain them first.
    drain_until = time.time() + 2
    while time.time() < drain_until:
        target.ReceiveMessage(None)
    target.TriggerCapture(1)

    deadline = time.time() + 120
    while time.time() < deadline:
        msg = target.ReceiveMessage(None)
        if msg.type == rd.TargetControlMessageType.NewCapture:
            log.write(f"capture {msg.newCapture.path}\n")
            break
        if msg.type == rd.TargetControlMessageType.Disconnected:
            log.write("target disconnected\n")
            break
    else:
        log.write("timed out waiting for the capture\n")
    target.Shutdown()


os.makedirs(OUT, exist_ok=True)
with open(os.path.join(OUT, "trigger.txt"), "w", encoding="utf-8") as log:
    try:
        main(log)
    except Exception:
        traceback.print_exc(file=log)
os._exit(0)
