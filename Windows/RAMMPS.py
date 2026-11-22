import serial
import time
import msvcrt
from pathlib import Path

with open(Path(__file__).parent / "config.txt") as f:
    PORT = f.readline().strip()
    BAUD = int(f.readline().strip())

with serial.Serial(PORT, BAUD, timeout=1) as esp:
    lastHeartbeat = 0
    running = True

    while True:
        now = time.monotonic()

        if now - lastHeartbeat >= 2 and running:
            esp.write(b"skibidi\n")
            print("command sent")
            lastHeartbeat = now
        if esp.in_waiting:
            print("ESP32: ", esp.readline().decode("utf-8", errors="replace").strip())

        if msvcrt.kbhit():
            key = msvcrt.getwch()
            if key.lower() == "q":
                running = False
            if key.lower() == "r":
                running = True

        time.sleep(0.01)