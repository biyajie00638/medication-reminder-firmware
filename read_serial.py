import serial
import time
import sys

PORT = "COM6"
BAUD = 115200
DURATION = 180  # seconds to capture

try:
    ser = serial.Serial(PORT, BAUD, timeout=0.5)
except Exception as e:
    print(f"Failed to open {PORT}: {e}", flush=True)
    sys.exit(1)

# Trigger ESP32 reset via DTR/RTS pulse (DTR=0,RTS=1 -> EN low)
print("Triggering reset...", flush=True)
ser.dtr = False
ser.rts = True
time.sleep(0.1)
ser.dtr = True
ser.rts = True
time.sleep(0.5)

print(f"Reading {PORT} for {DURATION}s...", flush=True)
start = time.time()
while time.time() - start < DURATION:
    try:
        line = ser.readline()
        if line:
            try:
                print(line.decode('utf-8', errors='replace').rstrip(), flush=True)
            except:
                pass
    except Exception as e:
        print(f"Read error: {e}", flush=True)
        break

ser.close()
print("--- DONE ---", flush=True)
