# iotracker

`iotracker` monitors a process's real-time I/O activity. It reports read/write throughput, real disk traffic, and system-call counts every interval. It can also show file events with inotify and tell whether each open file is read sequentially or randomly.

## Requirements

Linux (it uses `/proc`, inotify and timerfd), GCC and Make:

```bash
sudo apt update
sudo apt install build-essential
```

## Build

```bash
make
make test
```

## Run

```bash
./iotracker -i 0.5 -- dd if=/dev/zero of=/tmp/out bs=1M count=200   # launch and monitor a command
./iotracker -p 1234 -n 10                                             # attach to a running process
./iotracker -f -w ./data -- ./myprogram                               # file events + access patterns
./iotracker -q -- tar czf /tmp/backup.tgz .                           # summary only
man ./iotracker.1                                                     # read the man page
```

## Options

| Option     | Meaning                                                      |
|------------|--------------------------------------------------------------|
| `-p PID`   | Monitor an existing process.                                 |
| `-i SEC`   | Sampling interval in seconds (default 1, fractions allowed). |
| `-n COUNT` | Stop after COUNT samples.                                    |
| `-w PATH`  | Watch a file or directory with inotify (repeatable, max 32). |
| `-f`       | Follow open-file offsets: sequential vs random access.       |
| `-q`       | Quiet: only print the final summary.                         |
| `-h`       | Show help.                                                   |

Exit status: `0` success (also after Ctrl+C / SIGTERM), `1` runtime error, `2` bad usage.

## Sample output

```
iotracker: monitoring pid 474 every 0.30s (Ctrl+C to stop)
time             read/s      write/s    disk-rd/s    disk-wr/s  rd-sys  wr-sys
[   0.02s] CREATE        /tmp/demo/data.bin
[   0.02s] MODIFY        /tmp/demo/data.bin
[   0.30s]    304.0 KiB    426.5 KiB          0 B          0 B      39       2
[   0.60s] fd 3   forward    /tmp/demo/data.bin 131072 -> 196608
[   1.20s] fd 3   seek-back  /tmp/demo/data.bin 262144 -> 131072
...
===== iotracker summary: pid 474, 2.42 s, 8 samples =====
  logical read   : 535.9 KiB     logical write : 256.0 KiB
  ...
  fd   pattern         advanced    fwd  seeks  file
  3    sequential     128.0 KiB      2      0  /tmp/demo/data.bin (closed)
  3    random         252.0 KiB      2      1  /tmp/demo/data.bin

  command exited with status 0
```

## Design

- **`/proc/PID/io`** supplies the counters: `rchar`/`wchar` (logical bytes), `read_bytes`/`write_bytes` (disk), `syscr`/`syscw` (syscalls). Each sample prints the change divided by the elapsed time.
- **`timerfd` + `poll()`**: one event loop waits on the sampling timer and the inotify descriptor together, so file events print the moment they happen.
- **`inotify`** watches every `-w` path. **`ioctl(FIONREAD)`** returns how many event bytes are queued, so the read buffer is sized exactly.
- **`ioctl(TIOCGWINSZ)`** gets the terminal width; long paths are shortened from the left to fit.
- **`/proc/PID/fdinfo`** gives each descriptor's offset and open flags. Forward-only movement means sequential and any backward move means random. A change of file or flags on the same fd number counts as a close and reopen.
- **`ptrace(PTRACE_O_TRACEEXIT)`**: after a process exits, `/proc/PID/io` can no longer be read (`EACCES`), so I/O done after the last sample would be lost and a short command would show 0 bytes. The launched command is traced so the kernel stops it just before exit; iotracker takes the final sample there and lets it continue. Other signals are passed through unchanged. `PTRACE_O_EXITKILL` makes sure the command does not outlive iotracker.
- **Signals**: `sigaction` handles SIGINT/SIGTERM (stop, print summary, terminate a launched command) and SIGCHLD (wakes `poll()` as soon as the child stops or exits). Previous handlers are restored on return.
- **Custom Shell integration**: all state lives in one heap-allocated struct, `optind` is reset, and `main()` sits behind `#ifndef IOTRACKER_NO_MAIN`, so the leader can call `iotracker_main(argc, argv)` repeatedly from the shell.

## Limitations

- Counts only the target process (and its threads), not its children: `-- sh -c "cat f"` measures `sh`, not `cat`. Launch the program that does the I/O directly.
- inotify reports events on watched paths from **any** process.
- File offsets are only seen at sample times; lower `-i` to catch short-lived files.
- Disk columns may show 0 when data comes from the page cache or writes are not yet flushed.
- Monitoring another user's process needs root.

## Deliverables

`Rosina Zhou - iotracker.c` (source), `Makefile`, `test.sh` (14 tests), `iotracker.1` (man page).
