import serial
import time
import msvcrt
from pathlib import Path
import psutil
import pynvml

last_net = psutil.net_io_counters()
last_time = time.monotonic()

def getNetworkSpeed():
    global last_net, last_time

    current_net = psutil.net_io_counters()
    current_time = time.monotonic()

    elapsed = current_time - last_time

    download = (current_net.bytes_recv - last_net.bytes_recv) / elapsed / 1024
    upload = (current_net.bytes_sent - last_net.bytes_sent) / elapsed / 1024

    last_net = current_net
    last_time = current_time

    return round(download), round(upload)

def getGPUPerformance():
    pynvml.nvmlInit()
    gpu = pynvml.nvmlDeviceGetHandleByIndex(0)
    
    gpu_usage = pynvml.nvmlDeviceGetUtilizationRates(gpu).gpu
    gpu_temp = pynvml.nvmlDeviceGetTemperature(
        gpu, pynvml.NVML_TEMPERATURE_GPU
    )

    return gpu_usage, gpu_temp

def getComputerPerformance():
    cpu_usage = psutil.cpu_percent(interval=0.1)
    ram_usage = psutil.virtual_memory().percent
    gpu_usage, gpu_temp = getGPUPerformance()
    download, upload = getNetworkSpeed()
    
    return " " + str(round(cpu_usage)) + " " + str(round(ram_usage)) + " " + str(gpu_usage) + " " + str(gpu_temp) + " " + str(upload) + " " + str(download)

with open(Path(__file__).parent / "config.txt") as f:
    PORT = f.readline().strip()
    BAUD = int(f.readline().strip())

with serial.Serial(PORT, BAUD, timeout=1) as esp:
    lastHeartbeat = 0
    running = True

    while True:
        now = time.monotonic()

        if now - lastHeartbeat >= 2 and running:
            message = "TELEMETRY" + getComputerPerformance() + "\n"
            esp.write(message.encode())
            print("Sent:", message.strip())
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