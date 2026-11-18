# Process Manager – Real-Time Windows Process Monitoring (C)

A native Windows desktop application written in C using Win32 APIs and MSVC. This version builds on the working process-monitoring implementation and adds a richer dashboard.

## Features
- Enumerates running processes using PSAPI `EnumProcesses`.
- Displays PID, executable name, process CPU percentage, working-set memory, read/write I/O throughput, and heuristic classification.
- Refreshes approximately every 2 seconds on a background worker thread.
- Search by process name.
- Filter by Idle, CPU-Bound, I/O-Bound, Balanced, or Warming Up.
- Sort by CPU (highest first), memory (highest first), name, or PID.
- Select a process to view its current metrics and sample count.
- Displays rolling system CPU and memory-use history charts.
- Pause and resume monitoring.
- Uses a critical section to protect the latest worker snapshot while the GUI copies it.

## Requirements
- Windows 10 or Windows 11
- Visual Studio 2022/2026 with **Desktop development with C++** workload
- MSVC compiler and Windows SDK

## Build and run
1. Extract the project folder.
2. Open **Developer Command Prompt for Visual Studio**.
3. Change directory to the folder containing `build.bat`.
4. Run `build.bat`.
5. Run `build\ProcessManager.exe`.

## Monitoring notes
- Process CPU percentage is calculated from the difference in process kernel + user CPU time divided by elapsed wall-clock time and logical processor count. It is normalized to a 0–100% per-machine display.
- Process memory is the working set reported by `GetProcessMemoryInfo`, not private committed memory.
- I/O throughput uses differences in `IO_COUNTERS` transfer-byte counters. These are process-attributed I/O counters and are not necessarily physical disk throughput.
- The classifier uses a five-sample rolling window and project-defined thresholds: Warming Up (<3 samples), Idle (CPU <=1% and I/O <=64 KB/s), CPU-Bound (CPU >=20%), I/O-Bound (I/O >=1024 KB/s and CPU <=10%), otherwise Balanced. This is a heuristic, not a Windows-provided process label.
- Windows may deny access to protected/system processes. Such processes remain listed with fallback names and/or unavailable metrics. PID 0 and PID 4 are special system processes and are expected to have limited metrics.
- System memory and system CPU are separate machine-wide measurements; the per-process table can legitimately contain many low or zero CPU readings while the machine is mostly idle.
- The worker thread does not directly update controls. It posts a window message; the GUI thread copies the snapshot and updates the controls.

## Project structure
- `src/`: C implementation files
- `include/`: header files
- `tests/`: classifier test source and instructions
- `build/`: generated executable (created by the build script)
- `build.bat`: MSVC build script

## OS concepts demonstrated
Process enumeration, process CPU accounting, working-set memory inspection, process I/O counters, periodic sampling, multithreading, synchronization, GUI-thread message dispatch, sorting/filtering, and heuristic workload classification.

## Known limitations
- The charts show the recent history of total system CPU and memory utilization, not per-process history.
- Protected processes can have missing names or metrics depending on Windows security restrictions.
- The System/User label is not currently inferred because a robust distinction requires process-token/session inspection and should not be guessed from executable names.
- This source package has not been compiled on the user's Windows machine as part of this update. Compile with `build.bat` and report any MSVC diagnostics before relying on it for evaluation.

\n## GUI theme update\n\nThe GUI uses a navy-and-white title area, a pale-blue status strip, alternating process-table rows, and colour-coded classification text (CPU-Bound, I/O-Bound, Idle, Balanced, and Warming Up). CPU and memory history charts retain blue and green lines respectively.\n