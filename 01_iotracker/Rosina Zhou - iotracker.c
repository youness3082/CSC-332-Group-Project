/*
Name: Rosina Zhou
Class: CSC332 (Operating Systems)
Group #7

iotracker:
Monitor a process's real-time I/O activity. Every interval it reads the
kernel's per-process I/O accounting (/proc/PID/io) and prints read/write
throughput, real disk traffic, and system-call counts. With -w it uses
inotify to report file events (open, read, modify, close, create, delete)
on chosen files or directories, and with -f it follows the offset of every
file the process has open (/proc/PID/fdinfo) to classify the access
pattern as sequential or random.

System calls used:
  inotify_init1/inotify_add_watch  file event notification
  timerfd_create/timerfd_settime   periodic sampling timer
  poll()                           wait on the timer and inotify at once
  ioctl(FIONREAD)                  size the inotify read buffer exactly
  ioctl(TIOCGWINSZ)                fit output lines to the terminal width
  fork/execvp/waitpid              launch and watch a command
  ptrace(PTRACE_O_TRACEEXIT)       stop a launched command just before it
                                   exits to take a final, complete sample
  sigaction                        clean shutdown on SIGINT/SIGTERM/SIGCHLD

Provides iotracker_main(int argc, char **argv) with a conditional main()
guard (#ifndef IOTRACKER_NO_MAIN) so it can run standalone or be compiled
into the Custom Shell with -DIOTRACKER_NO_MAIN without duplicate main()
symbols.

Usage:
iotracker [options] -p PID              monitor an existing process
iotracker [options] -- command [args]   launch a command and monitor it
iotracker -h                            show help
*/
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/ptrace.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_WATCHES 32
#define MAX_FILES 256
#define MAX_FDS 1024
#define PATH_SZ 512
#define FULL_SZ (PATH_SZ + NAME_MAX + 2)   // watched dir + "/" + file name

// Counters from /proc/PID/io
struct io_stats {
    unsigned long long rchar, wchar;             // bytes passed to read()/write()
    unsigned long long syscr, syscw;             // number of read/write syscalls
    unsigned long long read_bytes, write_bytes;  // bytes that actually hit storage
};

// One inotify watch (a file or directory given with -w)
struct watch {
    int wd;
    char path[PATH_SZ];
};

// inotify event counts for one file
struct file_events {
    char path[FULL_SZ];
    unsigned long open, access, modify, close, create, del, move, total;
};

// Offset history for one open file descriptor of the target
struct fd_track {
    int used, open_now, closed;
    char path[PATH_SZ];
    long long last_pos;
    unsigned flags;                // open flags, to spot a close + reopen
    unsigned long long fwd_bytes;  // total bytes the offset moved forward
    unsigned fwd, back;            // number of forward moves / backward seeks
};

// All state for one run (heap allocated so the shell can call us many times)
struct tracker {
    pid_t pid;
    int launched;                  // 1 if we started the process ourselves
    int quiet, track_fds;
    int ifd, tfd;                  // inotify fd, timerfd
    int width;                     // terminal width from TIOCGWINSZ
    double interval;
    long max_samples, samples;
    struct timespec start;
    double last_tick;              // elapsed time of the previous sample

    struct io_stats base, last, cur;
    double peak_r, peak_w, peak_dr, peak_dw;
    long full_windows;             // samples long enough to count as peaks

    struct watch watches[MAX_WATCHES];
    int nwatches;
    struct file_events files[MAX_FILES];
    int nfiles;
    unsigned long ev_total, ev_overflow;

    struct fd_track fds[MAX_FDS];
    struct fd_track history[MAX_FILES];   // files closed before their fd was reused
    int history_fd[MAX_FILES];
    int nhistory;

    int child_status, child_reaped;
};

static volatile sig_atomic_t stop_flag = 0;
static volatile sig_atomic_t child_flag = 0;

static void on_stop(int sig) { (void)sig; stop_flag = 1; }
static void on_child(int sig) { (void)sig; child_flag = 1; }

// Seconds since monitoring started
static double elapsed(const struct tracker *t) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - t->start.tv_sec) +
           (double)(now.tv_nsec - t->start.tv_nsec) / 1e9;
}

// Turn a byte count into a short human readable string like "12.5 KiB"
static const char *fmt_bytes(double b, char *buf, size_t n) {
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    int i = 0;
    while (b >= 1024.0 && i < 4) { b /= 1024.0; i++; }
    snprintf(buf, n, i ? "%.1f %s" : "%.0f %s", b, units[i]);
    return buf;
}

// Ask the terminal how wide it is; use 100 columns when not a terminal
static int term_width(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 40)
        return ws.ws_col;
    return 100;
}

// Print a path, shortening it from the left with "..." if it is too long
static void print_path(const char *path, int room) {
    int len = (int)strlen(path);
    if (room < 8) room = 8;
    if (len <= room) fputs(path, stdout);
    else printf("...%s", path + len - (room - 3));
}

// Read the I/O counters of a process. Returns -1 if they cannot be read.
static int read_io(pid_t pid, struct io_stats *st) {
    char path[64], key[32];
    unsigned long long v;
    FILE *f;

    snprintf(path, sizeof path, "/proc/%d/io", (int)pid);
    if (!(f = fopen(path, "r"))) return -1;
    memset(st, 0, sizeof *st);
    while (fscanf(f, " %31[^:]: %llu", key, &v) == 2) {
        if      (!strcmp(key, "rchar"))       st->rchar = v;
        else if (!strcmp(key, "wchar"))       st->wchar = v;
        else if (!strcmp(key, "syscr"))       st->syscr = v;
        else if (!strcmp(key, "syscw"))       st->syscw = v;
        else if (!strcmp(key, "read_bytes"))  st->read_bytes = v;
        else if (!strcmp(key, "write_bytes")) st->write_bytes = v;
    }
    fclose(f);
    return 0;
}

// Check whether a process we did not start has exited or become a zombie
static int target_gone(pid_t pid) {
    char path[64], buf[256], *p;
    FILE *f;

    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    if (!(f = fopen(path, "r"))) return 1;
    p = fgets(buf, sizeof buf, f);
    fclose(f);
    if (!p || !(p = strrchr(buf, ')'))) return 1;
    return p[1] == ' ' && (p[2] == 'Z' || p[2] == 'X');
}

// ---------------------------------------------------------------- inotify

// Name of an inotify event
static const char *event_name(uint32_t m) {
    if (m & IN_OPEN)          return "OPEN";
    if (m & IN_ACCESS)        return "ACCESS";
    if (m & IN_MODIFY)        return "MODIFY";
    if (m & IN_CLOSE_WRITE)   return "CLOSE_WRITE";
    if (m & IN_CLOSE_NOWRITE) return "CLOSE_NOWRITE";
    if (m & IN_CREATE)        return "CREATE";
    if (m & IN_DELETE)        return "DELETE";
    if (m & IN_DELETE_SELF)   return "DELETE_SELF";
    if (m & IN_MOVED_FROM)    return "MOVED_FROM";
    if (m & IN_MOVED_TO)      return "MOVED_TO";
    if (m & IN_MOVE_SELF)     return "MOVE_SELF";
    if (m & IN_ATTRIB)        return "ATTRIB";
    return "OTHER";
}

// Find the counters for a path, adding a new entry if there is room
static struct file_events *file_entry(struct tracker *t, const char *path) {
    for (int i = 0; i < t->nfiles; i++)
        if (!strcmp(t->files[i].path, path)) return &t->files[i];
    if (t->nfiles == MAX_FILES) return NULL;
    struct file_events *fe = &t->files[t->nfiles++];
    memset(fe, 0, sizeof *fe);
    snprintf(fe->path, sizeof fe->path, "%s", path);
    return fe;
}

// Read every queued inotify event, count it, and print it
static void drain_inotify(struct tracker *t) {
    int avail = 0;
    char *buf;
    ssize_t n;

    // FIONREAD tells us exactly how many bytes of events are waiting
    if (ioctl(t->ifd, FIONREAD, &avail) == -1 || avail <= 0) avail = 4096;
    if (!(buf = malloc((size_t)avail))) { perror("malloc"); return; }

    n = read(t->ifd, buf, (size_t)avail);
    if (n < 0) {
        if (errno != EAGAIN && errno != EINTR) perror("read inotify");
        free(buf);
        return;
    }

    for (char *p = buf; p < buf + n; ) {
        struct inotify_event *ev = (struct inotify_event *)p;
        p += sizeof *ev + ev->len;

        if (ev->mask & IN_Q_OVERFLOW) { t->ev_overflow++; continue; }
        if (ev->mask & IN_IGNORED) continue;

        // Work out the full path from the watch descriptor and the name
        const char *dir = "?";
        for (int i = 0; i < t->nwatches; i++)
            if (t->watches[i].wd == ev->wd) { dir = t->watches[i].path; break; }
        char full[FULL_SZ];
        if (ev->len && ev->name[0]) snprintf(full, sizeof full, "%s/%s", dir, ev->name);
        else snprintf(full, sizeof full, "%s", dir);

        t->ev_total++;
        struct file_events *fe = file_entry(t, full);
        if (fe) {
            fe->total++;
            if (ev->mask & IN_OPEN) fe->open++;
            if (ev->mask & IN_ACCESS) fe->access++;
            if (ev->mask & IN_MODIFY) fe->modify++;
            if (ev->mask & (IN_CLOSE_WRITE | IN_CLOSE_NOWRITE)) fe->close++;
            if (ev->mask & IN_CREATE) fe->create++;
            if (ev->mask & (IN_DELETE | IN_DELETE_SELF)) fe->del++;
            if (ev->mask & (IN_MOVED_FROM | IN_MOVED_TO | IN_MOVE_SELF)) fe->move++;
        }

        if (!t->quiet) {
            printf("[%7.2fs] %-13s ", elapsed(t), event_name(ev->mask));
            print_path(full, t->width - 25);
            putchar('\n');
        }
    }
    free(buf);
    fflush(stdout);
}

// --------------------------------------------------- file offset tracking

// Read the current file offset ("pos:") and open flags ("flags:", octal)
// of one descriptor of the target
static long long fd_pos(pid_t pid, int fd, unsigned *flags) {
    char path[64], line[128];
    long long pos = -1;
    FILE *f;

    *flags = 0;
    snprintf(path, sizeof path, "/proc/%d/fdinfo/%d", (int)pid, fd);
    if (!(f = fopen(path, "r"))) return -1;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "pos: %lld", &pos) == 1) continue;
        if (sscanf(line, "flags: %o", flags) == 1) continue;
    }
    fclose(f);
    return pos;
}

// Remember a closed file whose fd number is being reused
static void save_closed(struct tracker *t, const struct fd_track *f, int fd) {
    if (t->nhistory == MAX_FILES) return;
    t->history[t->nhistory] = *f;
    t->history_fd[t->nhistory++] = fd;
}

static const char *pattern_name(const struct fd_track *f) {
    if (f->back > 0) return "random";
    if (f->fwd > 0)  return "sequential";
    return "idle";
}

// Look at every file the target has open and see how its offset moved
static void scan_fds(struct tracker *t) {
    char dir[64];
    DIR *d;
    struct dirent *e;

    snprintf(dir, sizeof dir, "/proc/%d/fd", (int)t->pid);
    if (!(d = opendir(dir))) return;

    for (int i = 0; i < MAX_FDS; i++) t->fds[i].open_now = 0;

    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        int fd = atoi(e->d_name);
        if (fd < 0 || fd >= MAX_FDS) continue;

        // Find out which file the descriptor points to
        char link[64], target[PATH_SZ];
        snprintf(link, sizeof link, "/proc/%d/fd/%d", (int)t->pid, fd);
        ssize_t n = readlink(link, target, sizeof target - 1);
        if (n <= 0) continue;
        target[n] = '\0';

        // Only follow real files (skip pipes, sockets, terminals, /proc)
        if (target[0] != '/' || !strncmp(target, "/dev/", 5) ||
            !strncmp(target, "/proc/", 6)) continue;

        unsigned flags;
        long long pos = fd_pos(t->pid, fd, &flags);
        if (pos < 0) continue;

        // Same fd number but a different file or open mode means the old
        // file was closed and something new was opened in its place
        struct fd_track *f = &t->fds[fd];
        if (f->used && !f->closed && (strcmp(f->path, target) != 0 || f->flags != flags)) {
            if (!t->quiet) {
                printf("[%7.2fs] fd %-3d closed   ", elapsed(t), fd);
                print_path(f->path, t->width - 35);
                putchar('\n');
            }
            f->closed = 1;
        }
        if (!f->used || f->closed) {
            // New file on this descriptor: keep the old one for the summary
            if (f->used) save_closed(t, f, fd);
            memset(f, 0, sizeof *f);
            f->used = 1;
            f->open_now = 1;
            f->last_pos = pos;
            f->flags = flags;
            snprintf(f->path, sizeof f->path, "%s", target);
            if (!t->quiet) {
                printf("[%7.2fs] fd %-3d opened   ", elapsed(t), fd);
                print_path(target, t->width - 45);
                printf(" @ %lld\n", pos);
            }
            continue;
        }

        f->open_now = 1;
        if (pos == f->last_pos) continue;
        if (pos > f->last_pos) { f->fwd++; f->fwd_bytes += (unsigned long long)(pos - f->last_pos); }
        else f->back++;

        if (!t->quiet) {
            printf("[%7.2fs] fd %-3d %-10s ", elapsed(t), fd, pos > f->last_pos ? "forward" : "seek-back");
            print_path(target, t->width - 60);
            printf(" %lld -> %lld\n", f->last_pos, pos);
        }
        f->last_pos = pos;
    }
    closedir(d);

    // Descriptors we saw before that are gone now were closed
    for (int i = 0; i < MAX_FDS; i++) {
        struct fd_track *f = &t->fds[i];
        if (f->used && !f->open_now && !f->closed) {
            f->closed = 1;
            if (!t->quiet) {
                printf("[%7.2fs] fd %-3d closed   ", elapsed(t), i);
                print_path(f->path, t->width - 35);
                putchar('\n');
            }
        }
    }
}

// ------------------------------------------------------------- sampling

static void print_header(const struct tracker *t) {
    if (t->quiet) return;
    printf("%-10s %12s %12s %12s %12s %7s %7s\n",
           "time", "read/s", "write/s", "disk-rd/s", "disk-wr/s", "rd-sys", "wr-sys");
}

// Take one sample of /proc/PID/io and print the rates since the last one
static int sample(struct tracker *t, double dt) {
    char a[32], b[32], c[32], d[32];
    struct io_stats now;

    if (read_io(t->pid, &now) == -1) return -1;
    t->cur = now;
    t->samples++;
    if (dt <= 0) dt = t->interval;

    double r  = (double)(now.rchar - t->last.rchar) / dt;
    double w  = (double)(now.wchar - t->last.wchar) / dt;
    double dr = (double)(now.read_bytes - t->last.read_bytes) / dt;
    double dw = (double)(now.write_bytes - t->last.write_bytes) / dt;
    // A very short last window (the process exited just after a tick)
    // would give a misleading spike, so only full-ish windows count as peaks
    if (dt >= t->interval / 2) {
        t->full_windows++;
        if (r > t->peak_r) t->peak_r = r;
        if (w > t->peak_w) t->peak_w = w;
        if (dr > t->peak_dr) t->peak_dr = dr;
        if (dw > t->peak_dw) t->peak_dw = dw;
    }

    if (!t->quiet) {
        printf("[%7.2fs] %12s %12s %12s %12s %7llu %7llu\n", elapsed(t),
               fmt_bytes(r, a, sizeof a), fmt_bytes(w, b, sizeof b),
               fmt_bytes(dr, c, sizeof c), fmt_bytes(dw, d, sizeof d),
               now.syscr - t->last.syscr, now.syscw - t->last.syscw);
    }
    t->last = now;

    if (t->track_fds) scan_fds(t);
    fflush(stdout);
    return 0;
}

static void print_summary(struct tracker *t) {
    char a[32], b[32];
    double secs = elapsed(t);
    struct io_stats *s = &t->cur, *z = &t->base;

    printf("\n===== iotracker summary: pid %d, %.2f s, %ld samples =====\n",
           (int)t->pid, secs, t->samples);
    printf("  logical read   : %-12s  logical write : %s\n",
           fmt_bytes((double)(s->rchar - z->rchar), a, sizeof a),
           fmt_bytes((double)(s->wchar - z->wchar), b, sizeof b));
    printf("  disk read      : %-12s  disk write    : %s\n",
           fmt_bytes((double)(s->read_bytes - z->read_bytes), a, sizeof a),
           fmt_bytes((double)(s->write_bytes - z->write_bytes), b, sizeof b));
    printf("  read syscalls  : %-12llu  write syscalls: %llu\n",
           s->syscr - z->syscr, s->syscw - z->syscw);
    if (secs > 0) {
        fmt_bytes((double)(s->rchar - z->rchar) / secs, a, sizeof a);
        fmt_bytes((double)(s->wchar - z->wchar) / secs, b, sizeof b);
        strcat(a, "/s");
        printf("  average read   : %-12s  average write : %s/s\n", a, b);
    }
    if (t->full_windows > 0) {
        fmt_bytes(t->peak_r, a, sizeof a);
        fmt_bytes(t->peak_w, b, sizeof b);
        strcat(a, "/s");
        printf("  peak read      : %-12s  peak write    : %s/s\n", a, b);
    } else {
        printf("  peak read      : %-12s  peak write    : n/a (ended within one interval)\n", "n/a");
    }

    if (t->nwatches > 0) {
        printf("\n  inotify events : %lu", t->ev_total);
        if (t->ev_overflow) printf("  (queue overflowed %lu times)", t->ev_overflow);
        putchar('\n');

        // Sort files by number of events, busiest first
        for (int i = 1; i < t->nfiles; i++) {
            struct file_events tmp = t->files[i];
            int j = i - 1;
            while (j >= 0 && t->files[j].total < tmp.total) { t->files[j + 1] = t->files[j]; j--; }
            t->files[j + 1] = tmp;
        }
        if (t->nfiles > 0)
            printf("  %6s %5s %6s %6s %5s %6s %6s  %s\n",
                   "events", "open", "access", "modify", "close", "create", "delete", "file");
        for (int i = 0; i < t->nfiles && i < 10; i++) {
            struct file_events *f = &t->files[i];
            printf("  %6lu %5lu %6lu %6lu %5lu %6lu %6lu  ", f->total, f->open, f->access,
                   f->modify, f->close, f->create, f->del);
            print_path(f->path, t->width - 52);
            putchar('\n');
        }
    }

    if (t->track_fds) {
        int any = 0;
        // Closed-and-replaced files first, then the current fd table
        for (int k = 0; k < t->nhistory + MAX_FDS; k++) {
            int i = k < t->nhistory ? t->history_fd[k] : k - t->nhistory;
            struct fd_track *f = k < t->nhistory ? &t->history[k] : &t->fds[i];
            if (!f->used) continue;
            if (!any) {
                printf("\n  %-4s %-11s %12s %6s %6s  %s\n",
                       "fd", "pattern", "advanced", "fwd", "seeks", "file");
                any = 1;
            }
            printf("  %-4d %-11s %12s %6u %6u  ", i, pattern_name(f),
                   fmt_bytes((double)f->fwd_bytes, a, sizeof a), f->fwd, f->back);
            print_path(f->path, t->width - 50);
            puts(f->closed ? " (closed)" : "");
        }
        if (!any) printf("\n  no regular files seen open at sample times\n");
    }

    if (t->launched && t->child_reaped) {
        if (WIFEXITED(t->child_status))
            printf("\n  command exited with status %d\n", WEXITSTATUS(t->child_status));
        else if (WIFSIGNALED(t->child_status))
            printf("\n  command killed by signal %d (%s)\n", WTERMSIG(t->child_status),
                   strsignal(WTERMSIG(t->child_status)));
    }
    fflush(stdout);
}

// Handle state changes of the command we launched (it runs under ptrace).
// With block = 0 it only looks at changes that already happened.
// Returns 1 once the child has exited and been reaped.
//
// Why ptrace: once a process has exited, /proc/PID/io can no longer be
// read, so anything it did after our last sample would be lost (a short
// command would show 0 bytes). PTRACE_O_TRACEEXIT makes the kernel stop the
// child just before it exits, while its counters and open files are still
// there, so we take the final sample at that moment.
static int handle_child(struct tracker *t, int block) {
    int st;

    while (!t->child_reaped) {
        pid_t r = waitpid(t->pid, &st, block ? 0 : WNOHANG);
        if (r == 0) return 0;
        if (r == -1) {
            if (errno == EINTR) continue;
            t->child_reaped = 1;             // ECHILD: nothing left to wait for
            break;
        }

        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            t->child_status = st;
            t->child_reaped = 1;
            break;
        }
        if (!WIFSTOPPED(st)) continue;

        int event = st >> 16;
        int sig = WSTOPSIG(st);
        if (event == PTRACE_EVENT_EXIT) {
            // About to exit: take the final, complete sample
            if (t->ifd >= 0) drain_inotify(t);
            sample(t, elapsed(t) - t->last_tick);
            sig = 0;
        } else if (event != 0) {
            sig = 0;                         // exec event: nothing to deliver
        }
        // Let it continue, passing on any signal it was supposed to get
        ptrace(PTRACE_CONT, t->pid, NULL, (void *)(long)sig);
    }
    if (t->ifd >= 0) drain_inotify(t);
    return 1;
}

// Start the command in a child process under ptrace. A close-on-exec pipe
// tells us whether execvp() worked.
static pid_t launch(char **argv) {
    int pfd[2], err = 0, st;
    pid_t pid;

    if (pipe2(pfd, O_CLOEXEC) == -1) { perror("pipe2"); return -1; }
    if ((pid = fork()) == -1) { perror("fork"); close(pfd[0]); close(pfd[1]); return -1; }

    if (pid == 0) {
        close(pfd[0]);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        // Ask to be traced; the kernel stops us with SIGTRAP after exec
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) == 0)
            execvp(argv[0], argv);
        err = errno;
        if (write(pfd[1], &err, sizeof err) < 0) { /* nothing more we can do */ }
        _exit(127);
    }

    close(pfd[1]);
    ssize_t n;
    while ((n = read(pfd[0], &err, sizeof err)) == -1 && errno == EINTR) {}
    close(pfd[0]);
    if (n == (ssize_t)sizeof err) {
        fprintf(stderr, "iotracker: cannot run '%s': %s\n", argv[0], strerror(err));
        while (waitpid(pid, NULL, 0) == -1 && errno == EINTR) {}
        return -1;
    }

    // Wait for the stop right after exec, then set up tracing:
    //  TRACEEXIT  stop before exit so the final counters can be read
    //  TRACEEXEC  later execs give an event instead of a stray SIGTRAP
    //  EXITKILL   if iotracker dies, the command is killed too
    while (waitpid(pid, &st, 0) == -1) {
        if (errno != EINTR) { perror("waitpid"); return -1; }
    }
    if (!WIFSTOPPED(st)) {
        fprintf(stderr, "iotracker: '%s' ended before it could be traced\n", argv[0]);
        return -1;
    }
    if (ptrace(PTRACE_SETOPTIONS, pid, NULL,
               (void *)(long)(PTRACE_O_TRACEEXIT | PTRACE_O_TRACEEXEC | PTRACE_O_EXITKILL)) == -1 ||
        ptrace(PTRACE_CONT, pid, NULL, NULL) == -1) {
        perror("ptrace");
        kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) == -1 && errno == EINTR) {}
        return -1;
    }
    return pid;
}

static void usage(const char *prog, FILE *out) {
    fprintf(out,
        "Usage: %s [options] -p PID\n"
        "       %s [options] -- command [args...]\n"
        "Monitor a process's real-time I/O activity.\n\n"
        "  -p PID    monitor an existing process\n"
        "  -i SEC    sampling interval in seconds (default 1, e.g. 0.5)\n"
        "  -n COUNT  stop after COUNT samples (default: until the process exits)\n"
        "  -w PATH   watch a file or directory with inotify (repeatable, max %d)\n"
        "  -f        follow open file offsets to show sequential/random access\n"
        "  -q        quiet: print only the final summary\n"
        "  -h        show this help\n",
        prog, prog, MAX_WATCHES);
}

int iotracker_main(int argc, char **argv) {
    struct tracker *t;
    struct sigaction sa_stop, sa_child, old_int, old_term, old_chld;
    int opt, status = 0;
    char *end;

    optind = 1;
    stop_flag = 0;
    child_flag = 0;

    if (!(t = calloc(1, sizeof *t))) { perror("calloc"); return 1; }
    t->pid = -1;
    t->ifd = -1;
    t->tfd = -1;
    t->interval = 1.0;

    // "+" stops option parsing at the first non-option (the command)
    while ((opt = getopt(argc, argv, "+p:i:n:w:fqh")) != -1) {
        switch (opt) {
        case 'p': {
            long v;
            errno = 0;
            v = strtol(optarg, &end, 10);
            if (errno || *end || v <= 0 || v > INT_MAX) {
                fprintf(stderr, "iotracker: invalid PID '%s'\n", optarg); free(t); return 2;
            }
            t->pid = (pid_t)v;
            break;
        }
        case 'i':
            errno = 0;
            t->interval = strtod(optarg, &end);
            if (errno || *end || t->interval < 0.05 || t->interval > 3600) {
                fprintf(stderr, "iotracker: interval must be between 0.05 and 3600 seconds\n");
                free(t); return 2;
            }
            break;
        case 'n':
            errno = 0;
            t->max_samples = strtol(optarg, &end, 10);
            if (errno || *end || t->max_samples <= 0) {
                fprintf(stderr, "iotracker: count must be a positive number\n"); free(t); return 2;
            }
            break;
        case 'w':
            if (t->nwatches == MAX_WATCHES) {
                fprintf(stderr, "iotracker: at most %d -w paths\n", MAX_WATCHES); free(t); return 2;
            }
            if (strlen(optarg) >= PATH_SZ) {
                fprintf(stderr, "iotracker: path too long: %s\n", optarg); free(t); return 2;
            }
            snprintf(t->watches[t->nwatches++].path, PATH_SZ, "%s", optarg);
            break;
        case 'f': t->track_fds = 1; break;
        case 'q': t->quiet = 1; break;
        case 'h': usage(argv[0], stdout); free(t); return 0;
        default:  usage(argv[0], stderr); free(t); return 2;
        }
    }

    // Exactly one target: -p PID or a command
    if ((t->pid > 0) == (optind < argc)) {
        fprintf(stderr, "iotracker: give either -p PID or a command to run\n");
        usage(argv[0], stderr);
        free(t);
        return 2;
    }

    t->width = term_width();

    // Set up inotify watches before starting anything so no event is missed
    if (t->nwatches > 0) {
        if ((t->ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) == -1) {
            perror("inotify_init1"); status = 1; goto out;
        }
        for (int i = 0; i < t->nwatches; i++) {
            uint32_t mask = IN_OPEN | IN_ACCESS | IN_MODIFY | IN_CLOSE_WRITE | IN_CLOSE_NOWRITE |
                            IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVED_FROM |
                            IN_MOVED_TO | IN_MOVE_SELF;
            t->watches[i].wd = inotify_add_watch(t->ifd, t->watches[i].path, mask);
            if (t->watches[i].wd == -1) {
                fprintf(stderr, "iotracker: cannot watch '%s': %s\n",
                        t->watches[i].path, strerror(errno));
                status = 1; goto out;
            }
        }
    }

    // Signals: Ctrl+C / SIGTERM end monitoring cleanly; SIGCHLD wakes poll()
    memset(&sa_stop, 0, sizeof sa_stop);
    sa_stop.sa_handler = on_stop;
    sigemptyset(&sa_stop.sa_mask);
    memset(&sa_child, 0, sizeof sa_child);
    sa_child.sa_handler = on_child;
    sigemptyset(&sa_child.sa_mask);
    if (sigaction(SIGINT, &sa_stop, &old_int) == -1 ||
        sigaction(SIGTERM, &sa_stop, &old_term) == -1 ||
        sigaction(SIGCHLD, &sa_child, &old_chld) == -1) {
        perror("sigaction"); status = 1; goto out;
    }

    // Get the starting counters
    clock_gettime(CLOCK_MONOTONIC, &t->start);
    if (optind < argc) {
        if ((t->pid = launch(&argv[optind])) == -1) { status = 1; goto restore; }
        t->launched = 1;
        // A new process starts with all counters at zero, so base stays zero
    } else {
        if (kill(t->pid, 0) == -1 && errno == ESRCH) {
            fprintf(stderr, "iotracker: no process with PID %d\n", (int)t->pid);
            status = 1; goto restore;
        }
        if (read_io(t->pid, &t->base) == -1) {
            fprintf(stderr, "iotracker: cannot read /proc/%d/io: %s\n", (int)t->pid,
                    errno == EACCES ? "permission denied (try sudo)" : strerror(errno));
            status = 1; goto restore;
        }
    }
    t->last = t->cur = t->base;

    // Periodic timer for sampling
    if ((t->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC)) == -1) {
        perror("timerfd_create"); status = 1; goto finish;
    }
    struct itimerspec its;
    its.it_interval.tv_sec = (time_t)t->interval;
    its.it_interval.tv_nsec = (long)((t->interval - (double)its.it_interval.tv_sec) * 1e9);
    its.it_value = its.it_interval;
    if (timerfd_settime(t->tfd, 0, &its, NULL) == -1) {
        perror("timerfd_settime"); status = 1; goto finish;
    }

    if (!t->quiet) {
        printf("iotracker: monitoring pid %d every %.2fs (Ctrl+C to stop)\n",
               (int)t->pid, t->interval);
        print_header(t);
    }
    if (t->track_fds) scan_fds(t);
    fflush(stdout);

    // Main event loop: wait for the timer or inotify, whichever comes first
    struct pollfd pfds[2] = {
        { .fd = t->tfd, .events = POLLIN },
        { .fd = t->ifd, .events = POLLIN },   // poll() ignores fd -1
    };
    while (!stop_flag) {
        if (t->launched && handle_child(t, 0)) break;

        int r = poll(pfds, 2, -1);
        if (r == -1) {
            if (errno == EINTR) continue;    // a signal arrived; loop checks flags
            perror("poll"); status = 1; break;
        }

        if (pfds[1].revents & POLLIN) drain_inotify(t);

        if (pfds[0].revents & POLLIN) {
            uint64_t ticks;
            if (read(t->tfd, &ticks, sizeof ticks) != (ssize_t)sizeof ticks) continue;
            double now = elapsed(t);
            if (sample(t, now - t->last_tick) == -1) {
                if (t->launched) continue;   // handle_child will finish up
                printf("iotracker: process %d has exited\n", (int)t->pid);
                break;
            }
            t->last_tick = now;
            if (t->max_samples && t->samples >= t->max_samples) break;
            if (!t->launched && target_gone(t->pid)) {
                printf("iotracker: process %d has exited\n", (int)t->pid);
                break;
            }
        }
    }

finish:
    // If we started the command and are stopping early, stop the command
    // too. Its exit stop still gives us one final sample.
    if (t->launched && !t->child_reaped) {
        kill(t->pid, SIGTERM);
        handle_child(t, 1);
    }
    print_summary(t);

restore:
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);
    sigaction(SIGCHLD, &old_chld, NULL);
out:
    if (t->tfd >= 0) close(t->tfd);
    if (t->ifd >= 0) close(t->ifd);
    free(t);
    return status;
}

#ifndef IOTRACKER_NO_MAIN
int main(int argc, char **argv) {
    return iotracker_main(argc, argv);
}
#endif
