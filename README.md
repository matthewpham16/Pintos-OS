# CS 439 Project 4: Pintos File Systems

This repository holds my CS 439 (UT Austin) solution to Project 4 of Pintos. Pintos is an instructional 32-bit x86 operating system written in C, originally developed at Stanford by Ben Pfaff. It boots on real hardware but is normally built and run inside a simulator such as Bochs or QEMU.

This repo builds on the earlier projects (threads and user programs). Project 4 replaces the baseline file system, which has a single flat directory and contiguous file allocation, with:

- **Indexed, extensible files:** inode-based allocation (`filesys/inode.c`) that supports files growing as they are written, and sparse files.
- **Subdirectories:** hierarchical paths, including `.` and `..`, plus the `mkdir`, `chdir`, `readdir`, and `isdir` system calls (`filesys/directory.c`, `filesys/filesys.c`, `userprog/syscall.c`).
- **Buffer cache and synchronization:** a block cache between the file system and the disk, with per-inode synchronization.

The user-program layer (argument passing, system calls, process wait/exec, write protection of executables) is also part of this tree under `userprog/`.

## Layout

Everything lives under `src/`.

| Path | Purpose |
| --- | --- |
| `src/filesys/` | **Main project 4 code.** `inode.c` (indexed inodes, file extension), `directory.c` (directories), `filesys.c` (path resolution, create/open/remove), `file.c`, `free-map.c`, `fsutil.c` |
| `src/userprog/` | Process loading and system calls: `process.c`, `syscall.c`, `exception.c`, `pagedir.c` |
| `src/threads/` | Kernel threads, scheduler, synchronization primitives (`synch.c`) |
| `src/devices/` | Drivers for timer, keyboard, serial, VGA, and the IDE disk |
| `src/lib/` | Small C library shared by kernel and user programs (`list.c`, `hash.c`, string functions, etc.) |
| `src/vm/` | Virtual memory directory (not part of this project) |
| `src/tests/` | Regression tests (`tests/filesys/base`, `tests/filesys/extended`, `tests/userprog`) and grading rubrics |
| `src/examples/` | Sample user programs used by the tests |
| `src/utils/` | The `pintos` launcher script and helpers (`pintos-mkdisk`, `pintos-gdb`, `backtrace`) |
| `src/design_filesys.txt` | Project 4 design document |
| `src/design_userprog.txt` | Project 2 design document |
| `src/stress.py` | Script that runs the grader repeatedly with random timer jitter to find flaky tests |
| `src/grade`, `src/stress_log` | Saved output from the last grading and stress runs |
| `src/Make.config`, `src/Makefile.*` | Shared build configuration |

## Prerequisites

The build needs a 32-bit x86 capable toolchain (`gcc`, `ld`, `make`, `perl`) and a simulator (Bochs or QEMU). On the UTCS lab machines, the course tools are already available once `.profile` has been sourced, since it adds the course `pintos` utilities and Bochs to `PATH`:

```sh
source .profile
```

## Building

The file system project is built from `src/filesys/`:

```sh
cd src/filesys
make            # builds build/kernel.bin, build/loader.bin, etc.
```

The user programs under `src/examples/` can be built with `make -C src/examples`.

## Running

Run commands from `src/filesys/build`. Create a simulated disk, format it, copy a program onto it, and run it:

```sh
cd src/filesys
make
cd build
pintos-mkdisk filesys.dsk --filesys-size=2
pintos -f -q                                  # format the file system
pintos -p ../../examples/echo -a echo -- -q   # copy a program onto the disk
pintos -q run 'echo hello'                    # run it
```

Useful options:

- `--bochs` or `--qemu` selects the simulator (for example `pintos --bochs -- -q run 'echo hello'`).
- `-v` disables the VGA display, and `-k` limits the run time.
- `--gdb` starts the simulator waiting for a debugger. Attach with `pintos-gdb kernel.o`.
- `-j <n>` sets the timer jitter seed, which is useful for reproducing scheduling-dependent failures.

## Testing

From `src/filesys/build`:

```sh
make check                       # run all tests, report pass/fail
make tests/filesys/base/lg-create.result   # run a single test
make grade                       # run all tests and write build/grade with rubric scores
```

Tests are grouped as:

- `tests/filesys/base`: basic file system behavior (create, read, write, sequential and random access).
- `tests/filesys/extended`: directories, file growth, persistence across reboots, and robustness.
- `tests/userprog`: system call and process tests.

To hunt for race conditions, run the stress script from `src/filesys` with the number of cycles to run. Each cycle does a clean build and runs the grader with a random jitter value, logging results to `src/stress_log`:

```sh
cd src
python3 stress.py filesys 10
```

## Status

The last saved grade in `src/grade` shows all 125 tests passing.

## Documentation

- Design write-up: [src/design_filesys.txt](src/design_filesys.txt)
- Pintos reference manual: https://web.stanford.edu/class/cs140/projects/pintos/pintos_1.html
- Course project docs: https://www.cs.utexas.edu/~ans/classes/cs439/projects/pintos/WWW/pintos_5.html
