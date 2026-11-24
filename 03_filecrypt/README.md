# filecrypt

`filecrypt` is a command-line tool that allows users to encrypt and decrypt regular files using AES-256-GCM. It uses a password provided by the user to create a 256-bit encryption key and checks that the file has not been changed before decrypting it.

## Requirements

- GCC compiler
- Make
- OpenSSL development library

On Ubuntu, install the required packages with:

```bash
sudo apt update
sudo apt install build-essential libssl-dev
```

## Build
```bash
make
make test
```

## Run
```bash
./filecrypt -e -p password input.txt encrypted.bin
./filecrypt -d -p password encrypted.bin output.txt
./filecrypt -h
```

## Options
`-e` encrypts; `-d` decrypts; `-p` password sets the key material; `-h` shows help.

## Design and limits
Uses `mmap()` for input reading, PBKDF2-SHA256 for key derivation, and `OPENSSL_cleanse()` to wipe sensitive memory. Rejects wrong passwords, corrupted or tampered files, invalid formats, and existing output paths; creates output files with `0600` permissions. Exposes `filecrypt_main()` behind `#ifndef FILECRYPT_NO_MAIN` and resets `optind` for Custom Shell integration.

## Deliverables
`James Lu - Command 3.c` (source), `Makefile`, `test.sh` (tests), `filecrypt.1` (man page).