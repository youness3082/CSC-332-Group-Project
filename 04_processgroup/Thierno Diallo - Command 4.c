/* Launch, signal, and monitor a batch of commands. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <errno.h>
#include <string.h>
#include <time.h>
static volatile sig_atomic_t interrupted = 0;
static pid_t pgid = -1;

// A negative PID targets every process in the group.
static void forward(int sig)
{
    interrupted = sig;
    if (pgid > 0) {
        kill(-pgid, sig);
    }
}
static void usage(const char *p)
{
    fprintf(stderr,
            "Usage: %s [-i seconds] [-t max_seconds] command [args] [::: command [args] "
            "...]\n",
            p);
}
int main(int argc, char **argv)
{

    // Read the monitor interval and optional timeout before the commands.
    int opt, interval = 1, timeout = 0;
    while ((opt = getopt(argc, argv, "i:t:h")) != -1) {
        if (opt == 'i' || opt == 't') {
            char *e;
            long n = strtol(optarg, &e, 10);
            if (*e || n < 1 || n > 3600) {
                usage(argv[0]);
                return 2;
            }
            if (opt == 'i') {
                interval = (int)n;
            } else {
                timeout = (int)n;
            }
        } else {
            usage(argv[0]);
            return opt == 'h' ? 0 : 2;
        }
    }
    if (optind == argc) {
        usage(argv[0]);
        return 2;
    }

    // Record where each command begins. Replace each separator with NULL to end its
    // argument list.
    int starts[128], count = 0;
    starts[count++] = optind;
    for (int i = optind; i < argc; i++) {
        if (!strcmp(argv[i], ":::")) {
            if (i == starts[count - 1] || i == argc - 1 || count == 128) {
                usage(argv[0]);
                return 2;
            }
            argv[i] = NULL;
            starts[count++] = i + 1;
        }
    }

    // Forward Ctrl+C and SIGTERM from the parent to the group.
    struct sigaction sa = {0};
    sa.sa_handler = forward;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    time_t started = time(NULL);
    int timedout = 0;
    pid_t pids[128];
    int alive[128] = {0}, left = 0, failed = 0;
    struct rusage ru;

    // Create one child per command. The first child supplies the group ID.
    for (int i = 0; i < count; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            failed = 1;
            break;
        }

        // Join the group before replacing this child with the requested command.
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            if (setpgid(0, pgid > 0 ? pgid : 0) < 0) {
                perror("setpgid");
                _exit(127);
            }
            execvp(argv[starts[i]], argv + starts[i]);
            perror("execvp");
            _exit(127);
        }

        // The parent also sets the group to cover either process running first.
        if (pgid < 0) {
            pgid = pid;
        }
        if (setpgid(pid, pgid) < 0 && errno != EACCES && errno != ESRCH) {
            perror("setpgid");
        }
        pids[i] = pid;
        alive[i] = 1;
        left++;
        printf("Started PID %ld: %s\n", (long)pid, argv[starts[i]]);
    }
    if (failed && pgid > 0) {
        kill(-pgid, SIGTERM);
    }
    fflush(stdout);

    // Check for timeouts and collect children until none are left.
    while (left) {
        if (timeout && !timedout && time(NULL) - started >= timeout) {
            fprintf(stderr, "Time limit reached: sending SIGTERM to process group\n");
            if (pgid > 0) {
                kill(-pgid, SIGTERM);
            }
            timedout = 1;
            failed = 1;
        }

        // WNOHANG lets the parent check for an exit without blocking.
        int status;
        pid_t done = wait4(-1, &status, WNOHANG, &ru);
        if (done > 0) {
            for (int i = 0; i < count; i++) {
                if (alive[i] && pids[i] == done) {
                    alive[i] = 0;
                    left--;
                    printf("PID %ld: %s, CPU %.3fs, max RSS %ld KB\n", (long)done,
                           WIFEXITED(status) ? "exited" : "signaled",
                           ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
                               ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6,
                           ru.ru_maxrss);
                    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                        failed = 1;
                    }
                    break;
                }
            }
            continue;
        }
        if (done < 0 && errno != EINTR) {
            perror("wait4");
            failed = 1;
            break;
        }

        // No child was collected this time. Sleep before checking again.
        printf("Running %d process(es) in group %ld\n", left, (long)pgid);
        fflush(stdout);
        struct timespec ts = {interval, 0};
        while (nanosleep(&ts, &ts) < 0 && errno == EINTR && !interrupted) {
        }
    }
    return failed || interrupted ? 1 : 0;
}
