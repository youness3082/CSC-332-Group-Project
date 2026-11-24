/*
Name: Rasul Mohammad
Class: CSC332 (Operating Systems)
Group #7

The core idea:
Set up data transfer between 2 processes. We do this by creating a buffer memory space via mmap(). This is a shared memory block (think of venn diagrams)
that Process A can write to and Process B can read from. We enforce strict turn-taking to avoid data loss and race conditions. 

Process A cannot send data#2 into the buffer until Process B finishes reading data#1 from the buffer. We use pipes to communicate status reports between
our processes. The flow looks something like

ProcessA --> sends data#1 into buffer --> update pipe w/ 'READY' --> ProcessB --> reads data#1 from buffer --> update pipe w/ 'DONE' --> ProcessA 
--> Sends data#2 into buffer --> ..

Usage:
sharedmempipe -m "text"     send one message
sharedmempipe < file        send everything read from standard input
sharedmempipe -h            show help
 */

#define _GNU_SOURCE          // needed for MAP_ANONYMOUS on Linux
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

// Size of the shared data area. 4KB  is the usual size of one memory page on linux so 4096 bytes was chosen for that reason
// longer A bigger chunk size means we batch more data which is faster but more memory-expensive. Opt for smaller batches if time isn't a constraint
#define CHUNK_SIZE 4096

// The 1-byte tokens sent through the pipes to convey status 
#define TOKEN_DATA 'D'   // producer -> consumer: the block has new data. DONE
#define TOKEN_QUIT 'Q'   // producer -> consumer: no more data, exit. QUIT
#define TOKEN_ACK  'A'   // consumer -> producer: done reading the block. ACK

// Each pipe has two ends: index 0 is for reading, index 1 is for writing. Standard pipe communication like from last HW assignment
#define READ_END  0
#define WRITE_END 1

// Struct to define shared memory area 
struct shared_block {
    size_t        length;               // how many bytes in data[] are valid
    unsigned char data[CHUNK_SIZE];     // the chunk being transferred
};

// when you ctrl + c, we run a small function to atomically switch from 0 -> 1. This is what we know to do before sending a new chunk / clean up
static volatile sig_atomic_t stop_requested = 0;

// function declarations
static void handle_stop_signal(int signal_number);
static void print_usage(const char *program_name);
static int write_all(int fd, const void *buffer, size_t count);
static int read_all(int fd, void *buffer, size_t count);
static int send_token(int fd, char token);
static int receive_token(int fd, char *token);
static void install_signal_handlers(void);
static int run_consumer(struct shared_block *block, int ready_fd, int ack_fd);
static int hand_off_chunk(struct shared_block *block, size_t length, int ready_fd, int ack_fd);
static int produce_from_message(struct shared_block *block, const char *message, int ready_fd, int ack_fd);
static int produce_from_stdin(struct shared_block *block, int ready_fd, int ack_fd);

int main(int argc, char **argv) {
    const char *message = NULL;
    int option;

    // 1. Parse command-line options with getopt(). Function parses CLI options automatically
    while ((option = getopt(argc, argv, "m:h")) != -1) {
        switch (option) {
        case 'm':
            message = optarg;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:                       // unknown option or missing text
            print_usage(argv[0]);
            return 2;
        }
    }
    if (optind != argc) {              // extra words that are not options
        print_usage(argv[0]);
        return 2;
    }

    install_signal_handlers();

    // 2. Now we create the shared memory block
    //MAP_SHARED:    changes are visible to the child after fork().
    // MAP_ANONYMOUS: not backed by a file, just plain memory.
    struct shared_block *block = mmap(NULL, sizeof *block, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (block == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    // 3. Create signaling pipes: ready_pipe for producer -> consumer ('D' and 'Q') + ack_pipe for consumer -> producer ('A')
    int ready_pipe[2];
    int ack_pipe[2];
    if (pipe(ready_pipe) != 0) {
        perror("pipe");
        munmap(block, sizeof *block);
        return 1;
    }
    if (pipe(ack_pipe) != 0) {
        perror("pipe");
        close(ready_pipe[READ_END]);
        close(ready_pipe[WRITE_END]);
        munmap(block, sizeof *block);
        return 1;
    }

    // 4. Split into producer (parent) and consumer (child) vua fork() command
    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        close(ready_pipe[READ_END]);
        close(ready_pipe[WRITE_END]);
        close(ack_pipe[READ_END]);
        close(ack_pipe[WRITE_END]);
        munmap(block, sizeof *block);
        return 1;
    }

    if (child == 0) {
        // CHILD = consumer. Close the pipe ends it does not use.
        close(ready_pipe[WRITE_END]);
        close(ack_pipe[READ_END]);

        int result = run_consumer(block, ready_pipe[READ_END], ack_pipe[WRITE_END]);

        close(ready_pipe[READ_END]);
        close(ack_pipe[WRITE_END]);
        munmap(block, sizeof *block);
        _exit(result);   // _exit: do not run the parent's exit cleanup
    }

    // PARENT = producer. Close the pipe ends it does not use.
    close(ready_pipe[READ_END]);
    close(ack_pipe[WRITE_END]);

    // error handling
    int failed;
    if (message != NULL)
        failed = produce_from_message(block, message, ready_pipe[WRITE_END], ack_pipe[READ_END]);
    else
        failed = produce_from_stdin(block, ready_pipe[WRITE_END], ack_pipe[READ_END]);

    // 5. Send 'Q' and then clean up
    send_token(ready_pipe[WRITE_END], TOKEN_QUIT);
    close(ready_pipe[WRITE_END]);
    close(ack_pipe[READ_END]);

    int status = 0;
    if (waitpid(child, &status, 0) < 0) {   // wait so no zombie is left
        perror("waitpid");
        failed = 1;
    }
    munmap(block, sizeof *block);

    // Exit 0 only if both sides succeeded and nobody pressed Ctrl+C.
    int child_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (failed || stop_requested || !child_ok)
        return 1;
    return 0;
}

static void handle_stop_signal(int signal_number) {
    (void)signal_number;    
    stop_requested = 1;
}


static void print_usage(const char *program_name) {
    fprintf(stderr,
            "Usage: %s [-m text]\n"
            "  -m text   send this text through shared memory\n"
            "  -h        show this help\n"
            "With no -m, data is read from standard input.\n",
            program_name);
}

/**
Write exactly 'count' bytes to a file descriptor.
write() may write fewer bytes than asked, or be interrupted by a signal
(EINTR), so we loop until everything is written.
Returns 0 on success, -1 on error.
*/
static int write_all(int fd, const void *buffer, size_t count) {
    const char *position = buffer;

    while (count > 0) {
        ssize_t written = write(fd, position, count);
        if (written < 0 && errno == EINTR)
            continue;                    // interrupted by a signal: retry
        if (written <= 0)
            return -1;
        position += written;
        count    -= (size_t)written;
    }
    return 0;
}

/**
Loop to read() until we get the total amount of bytes it asked for (count). We return once we've read all bytes. Will only read 1 byte ('D' or 'A' or 'Q')
*/
static int read_all(int fd, void *buffer, size_t count) {
    char *position = buffer;

    while (count > 0) {
        ssize_t got = read(fd, position, count);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return -1;
        position += got;
        count    -= (size_t)got;
    }
    return 0;
}

// sends 1-byte token through a pipe.
static int send_token(int fd, char token) {
    return write_all(fd, &token, 1);
}

// Wait for (block on) a 1-byte token from a pipe.
static int receive_token(int fd, char *token) {
    return read_all(fd, token, 1);
}

/**
Install our signal handlers.
SIGINT / SIGTERM: ask the producer to stop after the current chunk.
SIGPIPE: ignore it. If the consumer dies, writing to its pipe would
         normally kill us with SIGPIPE. Ignoring it makes write() fail
         with an error instead, which we can report.
*/
static void install_signal_handlers(void) {
    struct sigaction action;

    memset(&action, 0, sizeof action);
    action.sa_handler = handle_stop_signal;
    sigaction(SIGINT,  &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, NULL);
}

/**
CONSUMER (runs in the child process).
Loop: wait for a token. On 'D', print the shared block and reply 'A'.
On 'Q' (or if the producer closes its pipe), stop.
Returns 0 on success, 1 on error. The child exits with this value.
*/
static int run_consumer(struct shared_block *block, int ready_fd, int ack_fd) {
    char token;

    // Wait for a message
    while (receive_token(ready_fd, &token) == 0) {
        // If 'Q' theres no more data, return
        if (token == TOKEN_QUIT)
            return 0;

        // Safety check: never trust a length bigger than the buffer.
        if (block->length > CHUNK_SIZE)
            return 1;
        
            // Print the data 
        if (fwrite(block->data, 1, block->length, stdout) != block->length)
            return 1;
        if (fflush(stdout) != 0)
            return 1;

        // Tell the producer the block is free to reuse.
        if (send_token(ack_fd, TOKEN_ACK) != 0)
            return 1;
    }
    return 0;   // producer closed the pipe without 'Q': treat as done
}

/**
PRODUCER helper: the chunk is already in block->data.
Tell the consumer it is there, then wait until the consumer is done.
This wait is what stops us from overwriting data still being read.
*/
static int hand_off_chunk(struct shared_block *block, size_t length, int ready_fd, int ack_fd) {
    char reply;

    block->length = length;
    if (send_token(ready_fd, TOKEN_DATA) != 0)
        return -1;
    if (receive_token(ack_fd, &reply) != 0)
        return -1;
    return 0;
}

// PRODUCER. Cuts up text from -m "text" into pieces of CHUNK_SIZE (4KB / 4096 bytes) and sends them one at a time for processing
static int produce_from_message(struct shared_block *block, const char *message, int ready_fd, int ack_fd) {
    size_t total  = strlen(message);
    size_t offset = 0;

    // Loop until we've exhausted the message
    while (offset < total && !stop_requested) {
        size_t remaining = total - offset;
        size_t length    = remaining > CHUNK_SIZE ? CHUNK_SIZE : remaining;

        // Use memcpy() to copy the chunk and send into into shared memory
        memcpy(block->data, message + offset, length);
        if (hand_off_chunk(block, length, ready_fd, ack_fd) != 0) // sends length, 'D' and waits for 'A'
            return 1;
        offset += length;
    }
    return 0;
}

// PRODUCER for standard input: read straight into the shared block.
static int produce_from_stdin(struct shared_block *block, int ready_fd, int ack_fd) {
    size_t length;

    while (!stop_requested &&
           (length = fread(block->data, 1, CHUNK_SIZE, stdin)) > 0) {
        if (hand_off_chunk(block, length, ready_fd, ack_fd) != 0)
            return 1;
    }
    return ferror(stdin) ? 1 : 0;
}
