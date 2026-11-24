# Command 8 — filediffadvanced

Operating System I — Custom Shell group project.

Compares two regular files by byte position, optionally compares corresponding text lines, and reports elapsed comparison time and throughput.

## Build and run (Linux)

```sh
make
./filediffadvanced -v file1.bin file2.bin
./filediffadvanced -t file1.txt file2.txt
make test
make clean
```

Options: `-v` shows the first 20 differing byte positions (zero-based, -1 means missing); `-t` also reports differing lines (one-based, first 20 shown); `-h` shows usage. Exit status: 0 identical/help, 1 different, 2 error/interrupted.

## Course connections

- Chapter 3, slide 3.51, Examples of IPC Systems — POSIX Shared Memory: demonstrates `mmap()` and access through a returned pointer. This command applies the same mapping mechanism to regular files, not IPC.
- Chapter 4, slide 4.51, Signal Handling: a user-defined handler overrides the default behavior. Here `sigaction()` registers handlers for SIGINT and SIGTERM; the handler sets a flag and main performs cleanup.
- Chapter 5, slides 5.10–5.11, Scheduling Criteria: motivates measuring time and throughput. Our metric is input MiB per second, not completed processes per second.
- Chapter 8, slide 8.4, System Model: request, use, release resources. We open/map, compare, then unmap/close.

The assignment itself illustrates `open`, `fstat`, and `mmap` and requires `getopt` and error/signal handling. Other API details are implementation choices, not claims that they appear in the lectures.

## Scope and limitations

Files must remain unchanged during comparison; truncating a mapped file concurrently can cause SIGBUS. Only regular files are supported. Text comparison is positional, not an edit-distance or insertion-aware diff. Timing covers the byte comparison loop (including verbose output and page faults), not setup, cleanup or the optional text pass. Throughput counts bytes from both files, not physical disk traffic. No threading or extra libraries are required.

## Integration

Place this folder at `08_filediffadvanced/` in the group repository. The command is built as a standalone executable with its existing interface. Do not commit the executable.
