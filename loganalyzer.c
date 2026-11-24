/*
Command 5: loganalyzer
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>


/* Print the correct command format when -h is used or arguments are invalid. */
static void usage(const char *name) {
    fprintf(stderr, "Usage: %s [-k keyword] [-v] FILE\n", name);
}

/*
 * Check whether a line contains a keyword.
 * s points to the line, n is its length, and key is the search text.
 * Returns 1 if found and 0 otherwise. Matching is case-sensitive.
 * memcmp() lets us search without requiring a null-terminated line.
 */
static int contains(const char *s, size_t n, const char *key) {
    size_t k = strlen(key);
    if (!k) return 1;
    if (k > n) return 0;
    for (size_t i = 0; i <= n-k; ++i)
        if (memcmp(s+i, key, k) == 0) return 1;
    return 0;
}

/* Parse options, read the log, count matching lines, and print results. */
int main(int argc, char **argv) {
    /* -k stores the optional keyword; -v enables printing matching lines. */
    const char *keyword = NULL;
    int verbose = 0, option;
    /* getopt() reads the supported command-line options. */
    while ((option = getopt(argc, argv, "k:vh")) != -1) {
        switch (option) {
            case 'k': keyword = optarg; break;
            case 'v': verbose = 1; break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 2;
        }
    }
    /* After the options, exactly one log file path must remain. */
    if (optind+1 != argc) { usage(argv[0]); return 2; }
    /* Measure elapsed time with a monotonic clock. */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    /* Open the input file without permission to modify it. */
    int fd = open(argv[optind], O_RDONLY);
    if (fd < 0) { perror("open"); return 2; }
    /* Obtain the file size and reject files that are not regular files. */
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        fprintf(stderr, "Input must be a regular file\n"); close(fd); return 2;
    }
    /* Make sure the file size can be represented by size_t. */
    size_t size = (size_t)st.st_size;
    if ((off_t)size != st.st_size) { fputs("File too large\n", stderr); close(fd); return 2; }
    /* Map nonempty files into read-only, private virtual memory. */
    const char *data = NULL;
    if (size) {
        data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (data == MAP_FAILED) { perror("mmap"); close(fd); return 2; }
    }
    /* Keep separate counts for total lines and each matching category. */
    size_t lines=0, errors=0, warnings=0, infos=0, matches=0;
    /* Process one line at a time, using newline characters as boundaries. */
    for (size_t begin=0; begin<size;) {
        size_t finish=begin;
        while (finish<size && data[finish]!='\n') finish++;
        size_t len=finish-begin;
        /* Ignore a trailing carriage return in Windows-style CRLF lines. */
        if (len && data[begin+len-1]=='\r') len--;
        const char *line=data+begin;
        lines++;
        /* Count lines containing each label (not the number of occurrences). */
        if (contains(line,len,"ERROR")) errors++;
        if (contains(line,len,"WARN")) warnings++;
        if (contains(line,len,"INFO")) infos++;
        /* If -k is given, count matching lines; -v also prints each match. */
        if (keyword && contains(line,len,keyword)) {
            matches++;
            if (verbose) printf("%zu: %.*s\n",lines,(int)(len>1000?1000:len),line);
        }
        /* Move past the newline to the beginning of the next line. */
        begin=finish<size?finish+1:finish;
    }
    /* Stop the timer and calculate the elapsed time in seconds. */
    clock_gettime(CLOCK_MONOTONIC, &end);
    double seconds=(end.tv_sec-start.tv_sec)+(end.tv_nsec-start.tv_nsec)/1e9;
    /* Report the file size, total lines, and matching line counts. */
    printf("File: %s\nBytes: %zu\nLines: %zu\nERROR lines: %zu\nWARN lines: %zu\nINFO lines: %zu\n",argv[optind],size,lines,errors,warnings,infos);
    if (keyword) printf("Keyword '%s' lines: %zu\n",keyword,matches);
    printf("Elapsed: %.6f seconds\n",seconds);
    /* Release the mapping and close the file descriptor. */
    if (size && munmap((void*)data,size)<0) perror("munmap");
    close(fd);
    return 0;
}
