/*
Name: James Lu
Class: CSC332 (Operating Systems)
Group #7

filecrypt:
Encrypt and decrypt regular files using AES-256-GCM encryption with OpenSSL 
and memory-mapped file I/O via mmap(). Wrong passwords, corrupted or tampered 
files, invalid formats, and existing output paths are rejected, and output 
files are created with 0600 permissions.

Provides filecrypt_main(int argc, char **argv) with a conditional main()
guard (#ifndef FILECRYPT_NO_MAIN) so it can run standalone or be compiled
into the Custom Shell with -DFILECRYPT_NO_MAIN without duplicate main()
symbols.

Usage:
filecrypt (-e|-d) -p password <input> <output>    encrypt (-e) or decrypt (-d) a file
filecrypt -h                                      show help
*/
#define _POSIX_C_SOURCE 200809L
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/mman.h>

#define SALT_SZ 16
#define IV_SZ 12
#define TAG_SZ 16
#define KEY_SZ 32
#define CHUNK_SZ 4096
#define HDR_SZ (4 + SALT_SZ + IV_SZ)

// Keep track of whether the program received a stop signal
static volatile sig_atomic_t interrupted = 0;

// Set the interrupted flag when Ctrl+C or a stop signal is received
static void on_signal(int sig) { (void)sig; interrupted = 1; }

// Write all the data to the output file
static int write_all(int fd, const unsigned char *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        if (interrupted) return -1;
        ssize_t n = write(fd, buf + done, len - done);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

// Encrypt or decrypt the file based on the selected mode
static int crypt_file(int enc, const char *in_path, const char *out_path, const char *password) {
    int status = 1, in_fd = -1, out_fd = -1, created = 0, n = 0, m = 0;
    size_t file_sz = 0, cipher_sz = 0;
    struct stat st;
    unsigned char *mapped = MAP_FAILED, *plain = NULL;
    const unsigned char *cipher_data = NULL;
    unsigned char salt[SALT_SZ] = {0}, iv[IV_SZ] = {0}, key[KEY_SZ] = {0}, tag[TAG_SZ] = {0};
    unsigned char block[CHUNK_SZ + 32];
    EVP_CIPHER_CTX *ctx = NULL;

    // Open the input file and check that it is a regular file
    if ((in_fd = open(in_path, O_RDONLY)) < 0) { perror(in_path); goto cleanup; }
    if (fstat(in_fd, &st) == -1 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "Input must be a regular file\n"); goto cleanup;
    }
    file_sz = (size_t)st.st_size;

    // Load the input file into memory so it can be read
    if (file_sz > 0 && (mapped = mmap(NULL, file_sz, PROT_READ, MAP_PRIVATE, in_fd, 0)) == MAP_FAILED) {
        perror("mmap"); goto cleanup;
    }

    // Create new salt and IV values when encrypting
    // Read the salt, IV, encrypted data, and tag when decrypting
    if (enc) {
        if (RAND_bytes(salt, SALT_SZ) != 1 || RAND_bytes(iv, IV_SZ) != 1) goto cleanup;
    } else {
        // Check that the encrypted file has the correct size and header
        if (file_sz < HDR_SZ + TAG_SZ || memcmp(mapped, "FC01", 4) != 0) {
            fprintf(stderr, "Invalid encrypted file\n"); goto cleanup;
        }

        // Get the salt, IV, encrypted data, and tag from the file
        memcpy(salt, mapped + 4, SALT_SZ);
        memcpy(iv, mapped + 4 + SALT_SZ, IV_SZ);
        cipher_data = mapped + HDR_SZ;
        cipher_sz = file_sz - HDR_SZ - TAG_SZ;
        memcpy(tag, mapped + file_sz - TAG_SZ, TAG_SZ);
    }

    // Create the encryption key using the password and salt
    if (PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, SALT_SZ,
                          200000, EVP_sha256(), KEY_SZ, key) != 1) goto cleanup;

    // Set up AES-256-GCM using the key and IV
    if (!(ctx = EVP_CIPHER_CTX_new()) ||
        EVP_CipherInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL, enc) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IV_SZ, NULL) != 1 ||
        EVP_CipherInit_ex(ctx, NULL, NULL, key, iv, enc) != 1) goto cleanup;

    if (!enc) {
        // Decrypt the data and check if the password and file are correct
        // The output file is not created until the check is successful
        plain = malloc(cipher_sz + 32);
        if (!plain) { perror("malloc"); goto cleanup; }
        if (EVP_CipherUpdate(ctx, plain, &n, cipher_data, (int)cipher_sz) != 1 ||
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_SZ, tag) != 1 ||
            EVP_CipherFinal_ex(ctx, plain + n, &m) != 1) {
            fprintf(stderr, "Wrong password or corrupted file\n"); goto cleanup;
        }

        // Stop before creating the output file if a signal was received
        if (interrupted) goto cleanup;

        // Create the output file only if it does not already exist
        if ((out_fd = open(out_path, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0) {
            perror(out_path); goto cleanup;
        }
        created = 1;

        // Write the decrypted data to the output file
        if (write_all(out_fd, plain, (size_t)(n + m)) != 0) goto cleanup;
    } else {
        // Create the output file for the encrypted data
        if ((out_fd = open(out_path, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0) {
            perror(out_path); goto cleanup;
        }
        created = 1;

        // Write the file header, salt, and IV
        if (write_all(out_fd, (const unsigned char *)"FC01", 4) != 0 ||
            write_all(out_fd, salt, SALT_SZ) != 0 ||
            write_all(out_fd, iv, IV_SZ) != 0) goto cleanup;

        // Encrypt the file in small parts and write each part
        for (size_t off = 0; off < file_sz; off += CHUNK_SZ) {
            size_t take = (file_sz - off < CHUNK_SZ) ? (file_sz - off) : CHUNK_SZ;
            if (EVP_CipherUpdate(ctx, block, &n, mapped + off, (int)take) != 1 ||
                write_all(out_fd, block, (size_t)n) != 0) goto cleanup;
        }

        // Finish encryption and get the tag used to check the file later
        if (EVP_CipherFinal_ex(ctx, block, &n) != 1 || write_all(out_fd, block, (size_t)n) != 0 ||
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_SZ, tag) != 1 ||
            write_all(out_fd, tag, TAG_SZ) != 0) goto cleanup;
    }

    // Mark the program as successful if it was not interrupted
    if (!interrupted) status = 0;

cleanup:
    // Show a message if the program was stopped
    if (interrupted) fprintf(stderr, "Interrupted\n");

    // Clear sensitive data from memory
    OPENSSL_cleanse(key, sizeof(key));
    OPENSSL_cleanse(salt, sizeof(salt));
    OPENSSL_cleanse(iv, sizeof(iv));
    OPENSSL_cleanse(tag, sizeof(tag));
    OPENSSL_cleanse(block, sizeof(block));

    // Clear and free the decrypted data
    if (plain) { OPENSSL_cleanse(plain, cipher_sz + 32); free(plain); }

    // Free the encryption setup
    if (ctx) EVP_CIPHER_CTX_free(ctx);

    // Close the mapped input file and input file descriptor
    if (mapped != MAP_FAILED && munmap(mapped, file_sz) == -1) { perror("munmap"); status = 1; }
    if (in_fd >= 0 && close(in_fd) == -1) { perror("close"); status = 1; }

    // Close the output file
    if (out_fd >= 0 && close(out_fd) == -1) { perror("close"); status = 1; }

    // Delete the output file if something went wrong
    if (status != 0 && created && unlink(out_path) == -1) perror("unlink");

    return status;
}

// Handle command-line options and start the encryption or decryption process
int filecrypt_main(int argc, char **argv) {
    int opt, enc_mode = -1;
    const char *password = NULL;
    struct sigaction sa = { .sa_handler = on_signal };

    // Reset the command-line options and stop flag
    optind = 1;
    interrupted = 0;

    // Set up the program to handle Ctrl+C and termination signals
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction"); return 1;
    }

    // Read the options entered by the user
    while ((opt = getopt(argc, argv, "edp:h")) != -1) {
        if (opt == 'e' || opt == 'd') {
            // Make sure the user selects only one mode
            if (enc_mode != -1) { fprintf(stderr, "Choose -e or -d\n"); return 2; }
            enc_mode = (opt == 'e');
        } else if (opt == 'p') {
            // Save the password entered by the user
            password = optarg;
        } else {
            // Show the correct command format if the options are invalid
            fprintf(stderr, "Usage: %s (-e|-d) -p password input output\n", argv[0]);
            return (opt == 'h') ? 0 : 2;
        }
    }

    // Check that the mode, password, and file names are valid
    // Also make sure the input and output file names are different
    if (enc_mode < 0 || !password || !*password || optind + 2 != argc ||
        strcmp(argv[optind], argv[optind + 1]) == 0) {
        fprintf(stderr, "Usage: %s (-e|-d) -p password input output\n", argv[0]);
        return 2;
    }

    // Start encrypting or decrypting the selected file
    return crypt_file(enc_mode, argv[optind], argv[optind + 1], password);
}

#ifndef FILECRYPT_NO_MAIN
int main(int argc, char **argv)
{
    return filecrypt_main(argc, argv);
}
#endif