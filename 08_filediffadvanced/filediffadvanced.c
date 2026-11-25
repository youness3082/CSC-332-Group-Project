/* Command 8: compare regular files using read-only memory mappings. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <string.h>

static volatile sig_atomic_t interrupted = 0;
static void handle_signal(int sig) { interrupted = sig; }

struct file {
    int fd;
    size_t size;
    const unsigned char *data;
};

static int map_file(const char *path, struct file *f)
{
    struct stat st;
    f->fd = open(path, O_RDONLY | O_NONBLOCK);
    if (f->fd < 0 || fstat(f->fd, &st) < 0) {
        perror(path);
        return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uintmax_t)st.st_size > SIZE_MAX) {
        fprintf(stderr, "%s: unsupported file type or size\n", path);
        return -1;
    }
    f->size = (size_t)st.st_size;
    // Empty files cannot be mapped; there are no bytes to access.
    if (f->size == 0) return 0;
    void *p = mmap(NULL, f->size, PROT_READ, MAP_PRIVATE, f->fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return -1; }
    f->data = p;
    return 0;
}

static void release_file(struct file *f)
{
    if (f->data) munmap((void *)f->data, f->size);
    if (f->fd >= 0) close(f->fd);
}

// Find the end of a line, including its newline if present.
static size_t line_end(const struct file *f, size_t pos)
{
    while (pos < f->size && !interrupted) {
        if (f->data[pos++] == '\n') break;
    }
    return pos;
}

static void compare_lines(const struct file *a, const struct file *b)
{
    size_t x = 0, y = 0, line = 0, changes = 0;
    while ((x < a->size || y < b->size) && !interrupted) {
        size_t end_a = line_end(a, x), end_b = line_end(b, y);
        size_t len_a = end_a - x, len_b = end_b - y;
        if (interrupted) return;
        line++;
        if (len_a != len_b || (len_a && memcmp(a->data + x, b->data + y, len_a))) {
            changes++;
            if (changes <= 20) printf("Line %zu differs\n", line);
        }
        x = end_a;
        y = end_b;
    }
    if (!interrupted) printf("Differing lines: %zu\n", changes);
}

static void usage(const char *name)
{
    fprintf(stderr, "Usage: %s [-v] [-t] file1 file2\n", name);
}

// filediffadvanced_main() is the entry point. The Custom Shell compiles this
// file with -DFILEDIFFADVANCED_NO_MAIN and calls it directly.
int filediffadvanced_main(int argc, char **argv)
{
    int opt, verbose = 0, text = 0, result = 2;
    // Reset state so the command can run more than once in the same program.
    optind = 1;
    interrupted = 0;
    while ((opt = getopt(argc, argv, "tvh")) != -1) {
        if (opt == 'v') verbose = 1;
        else if (opt == 't') text = 1;
        else { usage(argv[0]); return opt == 'h' ? 0 : 2; }
    }
    if (argc - optind != 2) { usage(argv[0]); return 2; }

    // The handler only sets a flag; main releases resources safely.
    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction"); return 2;
    }
    struct file a = {.fd = -1}, b = {.fd = -1};
    struct timespec start, end;
    if (map_file(argv[optind], &a) < 0 || map_file(argv[optind + 1], &b) < 0)
        goto cleanup;
    if (clock_gettime(CLOCK_MONOTONIC, &start) < 0) {
        perror("clock_gettime"); goto cleanup;
    }
    size_t positions = a.size > b.size ? a.size : b.size;
    size_t diffs = 0;
    for (size_t i = 0; i < positions && !interrupted; i++) {
        // -1 represents a missing byte, not a real binary value.
        int x = i < a.size ? a.data[i] : -1;
        int y = i < b.size ? b.data[i] : -1;
        if (x != y) {
            diffs++;
            if (verbose && diffs <= 20) printf("Byte %zu: %d vs %d\n", i, x, y);
        }
    }
    if (interrupted) goto cleanup;
    if (clock_gettime(CLOCK_MONOTONIC, &end) < 0) {
        perror("clock_gettime"); goto cleanup;
    }
    double sec = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    double bytes = (double)a.size + (double)b.size;
    printf("%s; differing positions: %zu; compared positions: %zu; time: %.6f s; throughput: %.2f MiB/s\n",
           diffs ? "DIFFERENT" : "IDENTICAL", diffs, positions, sec,
           sec > 0 ? bytes / sec / 1048576.0 : 0);
    if (verbose && diffs > 20) puts("Only first 20 positions shown");
    if (text) compare_lines(&a, &b);
    result = diffs ? 1 : 0;
cleanup:
    release_file(&a);
    release_file(&b);
    if (interrupted) {
        fprintf(stderr, "Comparison interrupted by signal %d\n", (int)interrupted);
        return 2;
    }
    return result;
}

#ifndef FILEDIFFADVANCED_NO_MAIN
int main(int argc, char **argv)
{
    return filediffadvanced_main(argc, argv);
}
#endif
