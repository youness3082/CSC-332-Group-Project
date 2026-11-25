/*
Class: CSC332 (Operating Systems)
Group #7

cshell - the Custom Shell.
Combines the team's commands into one program. It reads command lines,
splits them into words, and runs them. The team commands (iotracker,
filecrypt, processgroup, loganalyzer, sharedmempipe, filediffadvanced) are
linked into this program and called through their <name>_main() functions;
any other name (ls, cat, ...) is run with execvp().

Every command runs in its own child process (fork), the same way a real
shell works. That keeps each command's signal handlers, global variables
and memory separate from the shell, and lets Ctrl+C stop a command without
stopping the shell.

Supported syntax:
  cmd args...             run a command
  "quoted words" 'too'    quotes keep spaces inside one argument
  cmd < in > out >> log   input / output / append redirection
  cmd1 | cmd2 | cmd3      pipelines
  # comment               ignored
Built-ins: help [command], cd [dir], exit [status]

Usage:
cshell                    interactive shell
cshell script             run the commands in a file
cshell -c "command line"  run one command line
cshell -x ...             echo each command before running it
cshell -h                 show help
*/
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "commands.h"

#define MAX_TOKENS 256
#define MAX_ARGS   128
#define MAX_STAGES 16

// ------------------------------------------------------- command table

static int help_main(int argc, char **argv);

struct command {
    const char *name;
    int (*run)(int argc, char **argv);
    const char *number;
    const char *summary;
};

static const struct command commands[] = {
    { "iotracker",        iotracker_main,        "1", "monitor a process's real-time I/O activity" },
    { "filecrypt",        filecrypt_main,        "3", "encrypt or decrypt a file with AES-256-GCM" },
    { "processgroup",     processgroup_main,     "4", "launch, signal and monitor a group of commands" },
    { "loganalyzer",      loganalyzer_main,      "5", "count log levels and keywords in a log file" },
    { "sharedmempipe",    sharedmempipe_main,    "6", "move data between processes through shared memory" },
    { "filediffadvanced", filediffadvanced_main, "8", "compare two files byte by byte and line by line" },
    { "help",             help_main,             "",  "list commands, or show one command's help" },
};
#define NCOMMANDS (sizeof commands / sizeof commands[0])

static const struct command *find_command(const char *name) {
    for (size_t i = 0; i < NCOMMANDS; i++)
        if (strcmp(commands[i].name, name) == 0) return &commands[i];
    return NULL;
}

// "help" lists everything; "help NAME" runs "NAME -h"
static int help_main(int argc, char **argv) {
    if (argc > 1) {
        const struct command *c = find_command(argv[1]);
        if (!c || c->run == help_main) {
            fprintf(stderr, "help: '%s' is not a team command\n", argv[1]);
            return 1;
        }
        char *args[] = { argv[1], "-h", NULL };
        return c->run(2, args);
    }
    printf("Team commands:\n");
    for (size_t i = 0; i + 1 < NCOMMANDS; i++)
        printf("  %-17s (Command %s) %s\n", commands[i].name, commands[i].number, commands[i].summary);
    printf("\nBuilt-ins:\n"
           "  help [command]    this list, or the usage of one command\n"
           "  cd [dir]          change directory (default: $HOME)\n"
           "  exit [status]     leave the shell (Ctrl+D also works)\n"
           "\nSyntax: quotes \"a b\" 'a b', redirection < > >>, pipes |, # comments.\n"
           "Any other command (ls, cat, ...) runs as a normal Linux program.\n");
    return 0;
}

// ------------------------------------------------------- signals

static volatile sig_atomic_t got_sigint = 0;
static void on_sigint(int sig) { (void)sig; got_sigint = 1; }

// The shell itself survives Ctrl+C: it only sets a flag. No SA_RESTART, so
// a blocked read of the prompt line returns and we can show a new prompt.
static void shell_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    sa.sa_handler = SIG_IGN;
    sigaction(SIGQUIT, &sa, NULL);
}

// Children start with the normal default behavior for every signal
static void child_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);
}

// ------------------------------------------------------- parsing

enum { T_WORD, T_PIPE, T_IN, T_OUT, T_APPEND };

struct token {
    int type;
    char *text;      // only for T_WORD
};

struct stage {
    char *argv[MAX_ARGS + 1];
    int argc;
    char *in, *out;  // redirection files, or NULL
    int append;
};

// Split a line into words and operators. Words are written into buf, which
// must be at least strlen(line) + MAX_TOKENS bytes long.
// Returns the number of tokens, or -1 on a syntax error.
static int tokenize(const char *line, char *buf, struct token *toks) {
    const char *p = line;
    char *w = buf;
    int n = 0;

    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == '\0' || *p == '#') break;
        if (n == MAX_TOKENS) { fprintf(stderr, "cshell: line has too many words\n"); return -1; }

        if (*p == '|') { toks[n++] = (struct token){ T_PIPE, NULL }; p++; continue; }
        if (*p == '<') { toks[n++] = (struct token){ T_IN, NULL };   p++; continue; }
        if (*p == '>') {
            if (p[1] == '>') { toks[n++] = (struct token){ T_APPEND, NULL }; p += 2; }
            else             { toks[n++] = (struct token){ T_OUT, NULL };    p++; }
            continue;
        }

        // A word: read until unquoted whitespace or an operator
        toks[n].type = T_WORD;
        toks[n].text = w;
        while (*p && !strchr(" \t\r\n|<>", *p)) {
            if (*p == '\'') {                       // '...' everything literal
                p++;
                while (*p && *p != '\'') *w++ = *p++;
                if (!*p) { fprintf(stderr, "cshell: missing closing '\n"); return -1; }
                p++;
            } else if (*p == '"') {                 // "..." allows \" and \\ escapes
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) p++;
                    *w++ = *p++;
                }
                if (!*p) { fprintf(stderr, "cshell: missing closing \"\n"); return -1; }
                p++;
            } else if (*p == '\\' && p[1]) {        // \x outside quotes: literal x
                p++;
                *w++ = *p++;
            } else {
                *w++ = *p++;
            }
        }
        *w++ = '\0';
        n++;
    }
    return n;
}

// Group tokens into pipeline stages. Returns the number of stages, 0 for an
// empty line, or -1 on a syntax error.
static int build_stages(struct token *toks, int ntoks, struct stage *st) {
    int ns = 0;
    if (ntoks == 0) return 0;

    memset(&st[0], 0, sizeof st[0]);
    ns = 1;
    for (int i = 0; i < ntoks; i++) {
        struct stage *s = &st[ns - 1];
        switch (toks[i].type) {
        case T_WORD:
            if (s->argc == MAX_ARGS) { fprintf(stderr, "cshell: too many arguments\n"); return -1; }
            s->argv[s->argc++] = toks[i].text;
            break;
        case T_PIPE:
            if (s->argc == 0 || i == ntoks - 1) {
                fprintf(stderr, "cshell: syntax error near '|'\n"); return -1;
            }
            if (ns == MAX_STAGES) { fprintf(stderr, "cshell: pipeline too long\n"); return -1; }
            memset(&st[ns], 0, sizeof st[ns]);
            ns++;
            break;
        default:                                    // < > >> need a file name next
            if (i + 1 >= ntoks || toks[i + 1].type != T_WORD) {
                fprintf(stderr, "cshell: missing file name after redirection\n"); return -1;
            }
            if (toks[i].type == T_IN) s->in = toks[i + 1].text;
            else { s->out = toks[i + 1].text; s->append = (toks[i].type == T_APPEND); }
            i++;
            break;
        }
    }
    if (st[ns - 1].argc == 0) { fprintf(stderr, "cshell: missing command\n"); return -1; }
    for (int i = 0; i < ns; i++) st[i].argv[st[i].argc] = NULL;
    return ns;
}

// ------------------------------------------------------- running

// Runs inside the child: set up redirection, then run the command.
// Never returns.
static void run_stage(struct stage *s) {
    child_signals();

    if (s->in) {
        int fd = open(s->in, O_RDONLY);
        if (fd < 0) { fprintf(stderr, "cshell: %s: %s\n", s->in, strerror(errno)); _exit(1); }
        dup2(fd, STDIN_FILENO);
        close(fd);
    }
    if (s->out) {
        int flags = O_WRONLY | O_CREAT | (s->append ? O_APPEND : O_TRUNC);
        int fd = open(s->out, flags, 0644);
        if (fd < 0) { fprintf(stderr, "cshell: %s: %s\n", s->out, strerror(errno)); _exit(1); }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }

    const struct command *c = find_command(s->argv[0]);
    if (c) {
        int status = c->run(s->argc, s->argv);
        fflush(NULL);                               // push out buffered output
        _exit(status & 0xff);
    }
    execvp(s->argv[0], s->argv);
    fprintf(stderr, "cshell: %s: %s\n", s->argv[0],
            errno == ENOENT ? "command not found" : strerror(errno));
    _exit(127);
}

// Start every stage of a pipeline, connected with pipes, and wait for all
// of them. Returns the exit status of the last stage, shell style
// (128 + signal number if it was killed by a signal).
static int run_pipeline(struct stage *st, int ns) {
    pid_t pids[MAX_STAGES];
    int started = 0, prev_read = -1, last_status = 0;

    fflush(stdout);                                 // children must not repeat our output
    fflush(stderr);

    for (int i = 0; i < ns; i++) {
        int pfd[2] = { -1, -1 };
        if (i < ns - 1 && pipe(pfd) == -1) { perror("cshell: pipe"); last_status = 1; break; }

        pid_t pid = fork();
        if (pid == -1) {
            perror("cshell: fork");
            if (pfd[0] >= 0) { close(pfd[0]); close(pfd[1]); }
            last_status = 1;
            break;
        }
        if (pid == 0) {
            if (prev_read >= 0) { dup2(prev_read, STDIN_FILENO); close(prev_read); }
            if (pfd[1] >= 0) { dup2(pfd[1], STDOUT_FILENO); close(pfd[1]); close(pfd[0]); }
            run_stage(&st[i]);
        }

        // Parent: keep only the read end for the next stage
        if (prev_read >= 0) close(prev_read);
        if (pfd[1] >= 0) close(pfd[1]);
        prev_read = pfd[0];
        pids[started++] = pid;
    }
    if (prev_read >= 0) close(prev_read);

    // Wait for every child; Ctrl+C reaches them directly from the terminal
    for (int i = 0; i < started; i++) {
        int status;
        while (waitpid(pids[i], &status, 0) == -1) {
            if (errno != EINTR) { perror("cshell: waitpid"); status = 1 << 8; break; }
        }
        if (i == started - 1) {
            if (WIFEXITED(status)) last_status = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) {
                last_status = 128 + WTERMSIG(status);
                if (WTERMSIG(status) == SIGINT) fputc('\n', stderr);
                else fprintf(stderr, "cshell: %s: %s\n", st[ns - 1].argv[0], strsignal(WTERMSIG(status)));
            }
        }
    }
    got_sigint = 0;
    return last_status;
}

// Built-ins that must change the shell itself, so they run in the shell
// process. Returns 1 if the line was a built-in.
static int run_builtin(struct stage *st, int ns, int *status, int *should_exit) {
    if (ns != 1) return 0;
    struct stage *s = &st[0];

    if (strcmp(s->argv[0], "exit") == 0 || strcmp(s->argv[0], "quit") == 0) {
        if (s->argc > 1) {
            char *end;
            long v = strtol(s->argv[1], &end, 10);
            if (*end) { fprintf(stderr, "cshell: exit: numeric argument required\n"); *status = 2; return 1; }
            *status = (int)(v & 0xff);
        }
        *should_exit = 1;
        return 1;
    }
    if (strcmp(s->argv[0], "cd") == 0) {
        const char *dir = s->argc > 1 ? s->argv[1] : getenv("HOME");
        if (!dir) dir = "/";
        if (chdir(dir) == -1) {
            fprintf(stderr, "cshell: cd: %s: %s\n", dir, strerror(errno));
            *status = 1;
        } else {
            *status = 0;
        }
        return 1;
    }
    return 0;
}

// Parse and run one line. Returns 1 if the shell should exit.
static int run_line(const char *line, int echo, int *status) {
    struct token toks[MAX_TOKENS];
    static struct stage st[MAX_STAGES];
    int should_exit = 0;

    char *buf = malloc(strlen(line) + MAX_TOKENS + 1);
    if (!buf) { perror("cshell: malloc"); *status = 1; return 0; }

    int ntoks = tokenize(line, buf, toks);
    int ns = ntoks < 0 ? -1 : build_stages(toks, ntoks, st);
    if (ns < 0) {
        *status = 2;
    } else if (ns > 0) {
        if (echo) {
            printf("cshell> %s", line);
            if (line[0] && line[strlen(line) - 1] != '\n') putchar('\n');
        }
        if (!run_builtin(st, ns, status, &should_exit))
            *status = run_pipeline(st, ns);
    }
    free(buf);
    return should_exit;
}

// ------------------------------------------------------- main loop

static void print_prompt(void) {
    char cwd[PATH_MAX];
    const char *dir = "?";
    if (getcwd(cwd, sizeof cwd)) {
        const char *slash = strrchr(cwd, '/');
        dir = (slash && slash[1]) ? slash + 1 : cwd;
    }
    printf("cshell:%s> ", dir);
    fflush(stdout);
}

static int run_stream(FILE *in, int interactive, int echo) {
    char *line = NULL;
    size_t cap = 0;
    int status = 0;

    for (;;) {
        if (interactive) print_prompt();
        errno = 0;
        ssize_t n = getline(&line, &cap, in);
        if (n == -1) {
            if (errno == EINTR || got_sigint) {     // Ctrl+C at the prompt
                clearerr(in);
                got_sigint = 0;
                putchar('\n');
                continue;
            }
            if (interactive) putchar('\n');         // Ctrl+D
            break;
        }
        if (run_line(line, echo, &status)) break;
    }
    free(line);
    return status;
}

static void usage(const char *prog, FILE *out) {
    fprintf(out,
        "Usage: %s [-x] [script]\n"
        "       %s [-x] -c \"command line\"\n"
        "Custom Shell: runs the team commands and normal Linux commands.\n\n"
        "  -c LINE   run one command line and exit\n"
        "  -x        print each command before running it\n"
        "  -h        show this help\n"
        "Type 'help' inside the shell for the list of commands.\n",
        prog, prog);
}

int main(int argc, char **argv) {
    const char *line = NULL;
    int opt, echo = 0, status;

    while ((opt = getopt(argc, argv, "+c:xh")) != -1) {
        switch (opt) {
        case 'c': line = optarg; break;
        case 'x': echo = 1; break;
        case 'h': usage(argv[0], stdout); return 0;
        default:  usage(argv[0], stderr); return 2;
        }
    }
    if ((line && optind < argc) || argc - optind > 1) { usage(argv[0], stderr); return 2; }

    shell_signals();

    // One command line from -c
    if (line) {
        run_line(line, echo, &status);
        return status;
    }

    // A script file. "e" = close-on-exec, so commands can't read the script.
    if (optind < argc) {
        FILE *f = fopen(argv[optind], "re");
        if (!f) { fprintf(stderr, "cshell: %s: %s\n", argv[optind], strerror(errno)); return 1; }
        status = run_stream(f, 0, echo);
        fclose(f);
        return status;
    }

    // Interactive (or commands piped into standard input)
    int interactive = isatty(STDIN_FILENO);
    if (interactive)
        printf("Custom Shell - CSC332 Group 7. Type 'help' for commands, 'exit' to quit.\n");
    return run_stream(stdin, interactive, echo);
}
