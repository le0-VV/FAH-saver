"""Check CLI input boundaries without opening a window or using the GPU."""
import subprocess
import sys

binary = sys.argv[1]
help_result = subprocess.run([binary, "--help"], capture_output=True, text=True, timeout=10)
assert help_result.returncode == 0 and "--offscreen" in help_result.stdout
cases = [
    ([], "Choose exactly one"),
    (["--capture", "unused", "--source", "ws://localhost:7396/api/websocket"], "Choose exactly one"),
    (["--capture", "unused", "--fps", "0"], "Frame-rate limit must be a whole number"),
    (["--capture", "unused", "--fps", "1.5"], "Frame-rate limit must be a whole number"),
    (["--capture", "unused", "--fps", "NaN"], "Invalid frame-rate limit"),
    (["--capture", "unused", "--seconds", "-1"], "Duration must be greater than zero"),
    (["--capture", "unused", "--warmup", "-1"], "Warm-up duration must be between zero and 120"),
    (["--capture", "unused", "--warmup", "121"], "Warm-up duration must be between zero and 120"),
    (["--capture", "unused", "--warmup", "NaN"], "Invalid warm-up duration"),
    (["--capture", "unused", "--snapshot-interval", "0"], "Snapshot interval must be"),
    (["--capture", "unused", "--snapshot-interval", "0.049"], "Snapshot interval must be"),
    (["--capture", "unused", "--snapshot-interval", "60.001"], "Snapshot interval must be"),
    (["--capture", "unused", "--snapshot-interval", "NaN"], "Invalid snapshot interval"),
    (["--capture", "unused", "--snapshot-interval", "0.5junk"], "Invalid snapshot interval"),
    (["--capture", "unused", "--width", "4097"], "Drawable dimensions must be whole numbers"),
    (["--capture", "unused", "--style", "0"], "Style must be"),
    (["--capture", "unused", "--playback", "invalid"], "Playback must be"),
    (["--capture", "unused", "--background", "invalid"], "Background must be"),
    (["--capture", "unused", "--colour", "1,2"], "Colour must contain"),
    (["--capture", "unused", "--screenshot", "unused.png"], "--screenshot requires --offscreen"),
    (["--capture", "unused", "--unit", "id"], "--unit requires --source"),
    (["--capture"], "Missing value"),
    (["--unknown", "value"], "Unknown option"),
]
for args, expected in cases:
    result = subprocess.run([binary, *args], capture_output=True, text=True, timeout=10)
    assert result.returncode == 1 and expected in result.stderr, (args, result.returncode, result.stderr)
print(f"CLI help and {len(cases)} input boundary checks passed.")
