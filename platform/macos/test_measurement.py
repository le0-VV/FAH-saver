"""Explicit GPU check that warm-up frames do not enter measured statistics."""
import json
import subprocess
import sys

binary, capture = sys.argv[1:3]
result = subprocess.run(
    [binary, "--capture", capture, "--style", "1", "--fps", "30",
     "--warmup", "0.15", "--seconds", "0.1", "--width", "320", "--height", "240", "--offscreen"],
    capture_output=True, text=True, timeout=20,
)
assert result.returncode == 0, result.stderr
metrics = json.loads(result.stdout)
assert metrics["warmup_seconds"] >= 0.15
assert metrics["elapsed_seconds"] >= 0.1
count = metrics["submitted_frames"]
assert 1 <= count <= 4, metrics
assert metrics["completed_frames"] == count, metrics
assert metrics["gpu_ms"]["samples"] == count, metrics
assert metrics["cpu_encode_ms"]["samples"] == count, metrics
assert metrics["gpu_errors"] == metrics["presented_frames"] == 0
assert not metrics["early_exit"]
print(f"Warm-up isolation passed: {count} measured submissions; no warm-up samples retained.")
