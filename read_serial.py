import serial
import time
import sys

PORT = "COM6"
BAUD = 115200
DURATION = 280       # max seconds to keep the port open
TAIL_AFTER_VOICE = 6  # extra seconds to capture after a recording is seen

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

print(f"Reading {PORT} for up to {DURATION}s...", flush=True)
start = time.time()
voice_seen_at = None
while time.time() - start < DURATION:
    try:
        line = ser.readline()
        if line:
            try:
                text = line.decode('utf-8', errors='replace').rstrip()
            except Exception:
                text = ""
            print(text, flush=True)
            if "[VOICE] Recorded" in text and voice_seen_at is None:
                voice_seen_at = time.time()
                print(">>> recording detected, capturing tail then exiting...", flush=True)
            if voice_seen_at is not None and (time.time() - voice_seen_at) >= TAIL_AFTER_VOICE:
                break
    except Exception as e:
        print(f"Read error: {e}", flush=True)
        break

ser.close()
print("--- DONE ---", flush=True)
