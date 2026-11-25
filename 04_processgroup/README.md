# processgroup

Launch a batch of commands in one process group, signal and monitor.

## Build

```bash
make
make test
```

## Run

```bash
./processgroup -i 1 sleep 2 ::: sleep 3
```

## Options

-i monitor interval in seconds; ::: separates commands. `-h` shows help.

## Design and limits

fork/execvp start up to 128 processes in a common process group; Ctrl+C/SIGTERM forwards a signal to the group. wait4 reports CPU usage and maximum resident memory for each completed process. The group monitor reports number of active processes. Avoid shell pipelines in command arguments unless invoked through sh -c.

## Deliverables

`Thierno Diallo - Command 4.c` (source), `Makefile`, `test.sh` (tests), `processgroup.1` (man page), `processgroup.pdf` (man page as PDF).
