#!/usr/bin/env python3
"""Measure a comparison app and its WebKit services, outside the measured app."""

import argparse
import ctypes
import datetime
import json
import os
from pathlib import Path
import plistlib
import resource
import statistics
import subprocess
import sys
import time


class Usage(ctypes.Structure):
    _fields_ = [("uuid", ctypes.c_uint8 * 16)] + [
        (name, ctypes.c_uint64) for name in
        ("user", "system", "idle", "interrupt", "pageins", "wired", "resident", "footprint", "start")
    ]


class Timebase(ctypes.Structure):
    _fields_ = [("numer", ctypes.c_uint32), ("denom", ctypes.c_uint32)]


class MacCounters:
    def __init__(self):
        self.proc = ctypes.CDLL("/usr/lib/libproc.dylib")
        self.proc.proc_pid_rusage.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
        self.proc.proc_pid_rusage.restype = ctypes.c_int
        system = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
        # Benchmark-only SPI: launchd is the parent of WebKit XPC services.
        self.responsible = system.responsibility_get_pid_responsible_for_pid
        self.responsible.argtypes = [ctypes.c_int]
        self.responsible.restype = ctypes.c_int
        base = Timebase()
        system.mach_timebase_info.argtypes = [ctypes.POINTER(Timebase)]
        if system.mach_timebase_info(ctypes.byref(base)) or not base.denom:
            raise RuntimeError("Cannot read the Mach clock timebase")
        self.tick_seconds = base.numer / base.denom / 1e9

    def usage(self, pid):
        buffer = ctypes.create_string_buffer(4096)
        if self.proc.proc_pid_rusage(pid, 4, buffer):
            return None
        value = Usage.from_buffer(buffer)
        return {"pid": pid, "start_ticks": value.start,
                "start_uptime": value.start * self.tick_seconds,
                "cpu_seconds": (value.user + value.system) * self.tick_seconds,
                "footprint_bytes": value.footprint, "resident_bytes": value.resident}


def inventory():
    result = subprocess.run(["/bin/ps", "-axo", "pid=,uid=,comm="],
                            check=True, capture_output=True, text=True)
    rows = {}
    for line in result.stdout.splitlines():
        fields = line.strip().split(None, 2)
        if len(fields) == 3 and int(fields[1]) == os.getuid():
            rows[int(fields[0])] = fields[2].rsplit("/", 1)[-1]
    return rows


def gpu_counters():
    result = subprocess.run(["/usr/sbin/ioreg", "-a", "-r", "-c", "IOAccelerator", "-d", "1"],
                            check=True, capture_output=True)
    records = plistlib.loads(result.stdout)
    values = []
    for device in records:
        stats = device.get("PerformanceStatistics", {})
        if "Device Utilization %" in stats:
            values.append({key: stats[key] for key in
                           ("Device Utilization %", "Renderer Utilization %", "Tiler Utilization %")
                           if key in stats})
    if len(values) != 1:
        raise RuntimeError("Expected one GPU with utilisation counters; cannot attribute this sampling mode")
    return {"uptime": time.monotonic(), "scope": "whole GPU; not per-process", **values[0]}


def interval_cpu(before, after):
    old = {(v["pid"], v["start_ticks"]): v for v in before["members"]}
    total = 0.0
    for value in after["members"]:
        previous = old.get((value["pid"], value["start_ticks"]))
        if previous:
            delta = value["cpu_seconds"] - previous["cpu_seconds"]
            if delta < 0:
                raise RuntimeError("A process CPU counter moved backwards")
            total += delta
        elif value["start_uptime"] >= before["uptime"]:
            total += value["cpu_seconds"]
    return total


def summarise(rows, phase):
    selected = [row for row in rows if row["phase"] == phase]
    if not selected:
        return None
    seconds = sum(b["uptime"] - a["uptime"] for a, b in zip(selected, selected[1:]))
    cpu = sum(interval_cpu(a, b) for a, b in zip(selected, selected[1:]))
    footprint = [row["footprint_bytes"] for row in selected]
    gpu = {row["gpu"]["uptime"]: row["gpu"]["Device Utilization %"]
           for row in selected if row.get("gpu") and
           selected[0]["uptime"] <= row["gpu"]["uptime"] <= selected[-1]["uptime"]}
    return {"samples": len(selected), "sampled_seconds": seconds,
            "observed_process_cpu_seconds": cpu,
            "cpu_percent_one_core": cpu / seconds * 100 if seconds else None,
            "median_group_footprint_bytes": statistics.median(footprint),
            "max_sampled_group_footprint_bytes": max(footprint),
            "group_process_counts": sorted(set(len(row["members"]) for row in selected)),
            "gpu_utilisation_samples": len(gpu),
            "mean_whole_gpu_utilisation_percent": statistics.mean(gpu.values()) if gpu else None}


def self_test():
    assert Usage.footprint.offset == 72
    def member(pid, start, birth, cpu):
        return {"pid": pid, "start_ticks": start, "start_uptime": birth, "cpu_seconds": cpu}
    first = {"uptime": 10, "members": [member(1, 1, 0, 5)]}
    second = {"uptime": 11, "members": [member(1, 1, 0, 5.5), member(2, 2, 10.2, .2)]}
    assert abs(interval_cpu(first, second) - .7) < 1e-9
    reused = {"uptime": 12, "members": [member(1, 9, 11.5, .1)]}
    assert abs(interval_cpu(second, reused) - .1) < 1e-9
    counters = MacCounters()
    deadline = time.process_time() + .08
    while time.process_time() < deadline:
        pass
    own = counters.usage(os.getpid())
    assert own and 0 < own["start_uptime"] <= time.monotonic()
    measured = resource.getrusage(resource.RUSAGE_SELF)
    assert abs(own["cpu_seconds"] - measured.ru_utime - measured.ru_stime) < .01
    assert counters.responsible(os.getpid()) > 0
    print("Process accounting checks passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--status", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--seconds", type=float, default=240)
    parser.add_argument("--interval", type=float, default=.25)
    parser.add_argument("--gpu", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.status or not args.output or not (.1 <= args.interval <= 2) or not (1 <= args.seconds <= 900):
        parser.error("Supply --status and --output; interval must be .1–2 seconds and duration 1–900 seconds")
    counters = MacCounters()
    rows, known, read_errors = [], {}, []
    began = time.monotonic()
    host = None
    host_start = None
    last_gpu = None
    final_status = None
    last_status = {}
    exited_at = None
    os.umask(0o077)
    print("Comparison sampler ready", flush=True)
    while time.monotonic() - began < args.seconds:
        tick = time.monotonic()
        if args.status.exists():
            last_status = json.loads(args.status.read_text())
            if last_status.get("pid"):
                if host is not None and last_status["pid"] != host:
                    raise RuntimeError("Benchmark host PID changed within a trial")
                host = int(last_status["pid"])
        if host is None:
            time.sleep(args.interval)
            continue
        procs = inventory()
        own = counters.usage(host)
        if own:
            if host_start is None:
                host_start = own["start_ticks"]
                if counters.responsible(host) != host:
                    raise RuntimeError("Launch comparison .app through LaunchServices for isolated responsibility")
            elif own["start_ticks"] != host_start:
                raise RuntimeError("Benchmark host PID was reused")
        elif exited_at is None:
            exited_at = tick
        members = []
        for pid, name in procs.items():
            web = "com.apple.WebKit." in name
            if pid != host and not web:
                continue
            value = counters.usage(pid)
            if value is None:
                if pid == host or any(key[0] == pid for key in known):
                    read_errors.append({"uptime": tick, "pid": pid})
                continue
            key = (pid, value["start_ticks"])
            if pid != host and key not in known and counters.responsible(pid) != host:
                continue
            value["role"] = "host" if pid == host else name
            known[key] = value["role"]
            members.append(value)
        if args.gpu and (last_gpu is None or tick - last_gpu["uptime"] >= 1):
            last_gpu = gpu_counters()
        phase = "after_exit" if not own else last_status.get("phase", "loading")
        rows.append({"uptime": tick, "unix_time": time.time(), "phase": phase,
                     "members": members, "footprint_bytes": sum(v["footprint_bytes"] for v in members),
                     "resident_bytes": sum(v["resident_bytes"] for v in members), "gpu": last_gpu})
        if last_status.get("phase") in ("complete", "error"):
            final_status = last_status
        if exited_at is not None and tick - exited_at >= 10:
            break
        time.sleep(max(0, args.interval - (time.monotonic() - tick)))
    result = {"host_pid": host, "status": final_status or last_status,
              "sampling_interval_seconds": args.interval, "sampling_errors": read_errors,
              "attribution": "host plus WebKit services with matching kernel responsibility PID; known identities followed after exit",
              "limitations": ["Responsibility lookup is a benchmark-only private macOS SPI.",
                              "Memory peaks are sampled simultaneous group footprints, not a sum of independent peaks.",
                              "CPU includes observed process lifetimes; an exiting process's unsampled final CPU is unavailable.",
                              "IORegistry utilisation covers the whole GPU, including other apps and compositor work."],
              "summary": {phase: summarise(rows, phase) for phase in ("warmup", "measurement", "stopped", "after_exit")},
              "samples": rows}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    if not host or not final_status or final_status.get("phase") != "complete":
        raise RuntimeError("Benchmark did not complete; partial sampling result was retained")
    print(json.dumps(result["summary"], indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(f"Comparison sampler: {error}", file=sys.stderr)
        sys.exit(1)
