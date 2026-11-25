# Custom Shell (cshell)

One program that combines all of the team's commands. You type commands at a `cshell>` prompt, as in a normal Linux shell.

| Command | # | What it does |
|---|---|---|
| `iotracker` | 1 | monitor a process's real-time I/O activity |
| `filecrypt` | 3 | encrypt or decrypt a file with AES-256-GCM |
| `processgroup` | 4 | launch, signal and monitor a group of commands |
| `loganalyzer` | 5 | count log levels and keywords in a log file |
| `sharedmempipe` | 6 | move data between processes through shared memory |
| `filediffadvanced` | 8 | compare two files byte by byte and line by line |

Normal Linux commands (`ls`, `cat`, `echo`, ...) work too.

## Requirements

```bash
sudo apt install build-essential libssl-dev
```

## Build and test

```bash
cd 00_customshell
make          # builds cshell from all six command sources
make test     # 14 tests
```

## Run

```bash
./cshell                       # interactive
./cshell -c "help"             # run one line
./cshell -x demo.txt           # run a script, printing each command first
man ./cshell.1
```

Inside the shell:

```
cshell:00_customshell> help
cshell:00_customshell> help filecrypt
cshell:00_customshell> iotracker -i 0.5 -- dd if=/dev/zero of=/tmp/x bs=1M count=100 status=none
cshell:00_customshell> loganalyzer -k ERROR -v app.log > errors.txt
cshell:00_customshell> printf "one\ntwo\n" | sharedmempipe | wc -l
cshell:00_customshell> cd /tmp
cshell:tmp> exit
```

**Syntax:** quotes (`"a b"`, `'a b'`), redirection (`<`, `>`, `>>`), pipes (`|`), `# comments`.
**Built-ins:** `help [command]`, `cd [dir]`, `exit [status]`; Ctrl+D also exits.

## How the commands are merged

Each command's source file stays in its own folder. Each one has its entry point as a function (`iotracker_main()`, `filecrypt_main()`, and so on), with the usual `main()` wrapped in `#ifndef <NAME>_NO_MAIN`:

- Built in its own folder with `make`, the command works on its own exactly as before.
- The shell's Makefile compiles the same file with `-D<NAME>_NO_MAIN`, which hides `main()`, and links all six into the single `cshell` program. `commands.h` declares the six functions, and a table in `cshell.c` maps each name to its function.

Each `_main()` also resets `optind` and its own flags, so a command can run many times in one session.

## How a command line runs

1. **Tokenize**: split the line into words and the operators `|`, `<`, `>`, `>>`, removing quotes.
2. **Parse**: group the words into pipeline stages, each with its own redirections.
3. **Built-ins** `cd` and `exit` run in the shell itself, because they must change the shell's own state.
4. **Everything else**: for each stage, `pipe()` connects it to the next one and `fork()` creates a child. In the child, `dup2()` attaches the pipes and redirections, the signals are reset to their defaults, and then either the team function is called or `execvp()` runs a Linux program.
5. **Wait**: the shell calls `waitpid()` on every child and reports the last stage's exit status (`128 + N` if signal N killed it).

**Why a child process for every command:** the team commands install their own signal handlers, use global flags and `getopt`, and some call `fork()` themselves. In a child they can't change the shell's state, a crash can't take the shell down, and Ctrl+C stops the command while the shell keeps running (it only sets a flag on SIGINT).

## Limitations

- No variables, wildcards, `;`, `&&`, `||`, background jobs `&`, or job control.
- Commands that run other commands (processgroup, iotracker) read options first, so put `--` before the inner command: `processgroup -- sh -c "echo hi"`.

## Files

`cshell.c` (shell), `commands.h` (command entry points), `Makefile`, `test.sh` (14 tests), `cshell.1` (man page).
