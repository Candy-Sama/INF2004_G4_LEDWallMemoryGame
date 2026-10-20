"""PC-only recovery example. Uses installed Mosquitto CLI tools; no Python packages.
This is a ONE-PANEL protocol demonstration, not the game's controller.
"""
import argparse
import queue
import secrets
import subprocess
import threading
import tempfile
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("role", choices=["controller", "panel"])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default="1883")
    parser.add_argument("--node", default="node1")
    parser.add_argument("--colour", type=int, choices=range(4), default=2)
    parser.add_argument("--duration", type=float, default=0, help="stop after N seconds; 0 runs indefinitely")
    parser.add_argument("--once", action="store_true", help="exit panel after one complete handshake")
    parser.add_argument("--bin-dir", default=r"C:\Program Files\mosquitto")
    args = parser.parse_args()
    base = "se4/memorygame/" + args.node
    cli = ["-h", args.host, "-p", args.port]
    flags = subprocess.CREATE_NO_WINDOW if hasattr(subprocess, "CREATE_NO_WINDOW") else 0
    messages = queue.Queue()
    sub = subprocess.Popen([str(Path(args.bin_dir)/"mosquitto_sub.exe"), *cli,
                            "-t", base+"/+", "-q", "1", "-F", "%t %x"],
                           stdout=subprocess.PIPE, text=True, creationflags=flags)
    def read_messages():
        for line in sub.stdout:
            parts = line.strip().split(" ", 1)
            if len(parts) == 2:
                try:
                    messages.put((parts[0].rsplit("/", 1)[1], bytes.fromhex(parts[1])))
                except ValueError:
                    pass
    threading.Thread(target=read_messages, daemon=True).start()
    def publish(suffix, data):
        # File input preserves binary zeros reliably with Windows CLI tools.
        with tempfile.TemporaryDirectory() as folder:
            payload_file = Path(folder)/"payload.bin"
            payload_file.write_bytes(data)
            subprocess.run([str(Path(args.bin_dir)/"mosquitto_pub.exe"), *cli,
                            "-t", base+"/"+suffix, "-q", "1", "-f", str(payload_file)],
                           check=True, timeout=3, creationflags=flags)
    token = secrets.token_bytes(8)
    header = None
    phase = "requesting"
    last_send = last_resume = 0.0
    print(args.role, "listening on", base, flush=True)
    started = time.monotonic()
    try:
        while sub.poll() is None and (not args.duration or time.monotonic()-started < args.duration):
            now = time.monotonic()
            try:
                kind, data = messages.get(timeout=0.05)
            except queue.Empty:
                kind, data = "", b""
            if args.role == "controller":
                if kind == "sync" and len(data) == 8:
                    # Replace this body with an authoritative full game snapshot.
                    candidate = data + (1).to_bytes(4, "big")
                    if candidate != header:
                        phase = "waiting_applied"
                    header = candidate
                    publish("state", header+bytes([args.colour]))
                    print("Sent state; waiting for application confirmation", flush=True)
                elif kind == "applied" and len(data) == 12 and data == header:
                    phase = "ready"
                if phase == "ready" and now-last_send >= 1:
                    publish("resume", header)
                    last_send = now
                    print("Confirmed resume / refreshed controller lease", flush=True)
            else:
                if kind == "state" and len(data) == 13 and data[:8] == token and data[12] <= 3:
                    if header != data[:12]:
                        header = data[:12]
                        # Stand-in for installing assignment/state and updating LEDs.
                        print("Applied simulated display colour", data[12], flush=True)
                        phase = "applied"
                        last_send = 0
                elif kind == "resume" and len(data) == 12 and data == header and phase in ("applied", "ready"):
                    phase = "ready"
                    last_resume = now
                    print("GAME READY: controller authorised play", flush=True)
                    if args.once:
                        return
                if phase == "ready" and now-last_resume >= 3:
                    print("Controller lease expired: stop input and request sync", flush=True)
                    token = secrets.token_bytes(8)
                    header, phase, last_send = None, "requesting", 0
                if phase != "ready" and now-last_send >= 0.2:
                    publish("sync" if phase == "requesting" else "applied",
                            token if phase == "requesting" else header)
                    last_send = now
    except KeyboardInterrupt:
        pass
    finally:
        sub.terminate()
        sub.wait(timeout=3)
        sub.stdout.close()


if __name__ == "__main__":
    main()
