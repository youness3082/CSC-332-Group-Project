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

Compile the program using:

```bash
make
```

Run the test script to check the encryption and decryption functions:

```bash
make test
```

## Run

**Encrypt a file:**

```bash
./filecrypt -e -p password input.txt encrypted.bin
```

**Decrypt a file:**

```bash
./filecrypt -d -p password encrypted.bin output.txt
```

**Display help:**

```bash
./filecrypt -h
```

## Options

- `-e` encrypts a file.
- `-d` decrypts a file.
- `-p` provides the password used for encryption or decryption.
- `-h` displays the help message.

## Test / Example

The following commands demonstrate how to build the program, encrypt a file, and decrypt it using the correct password. They also show how `filecrypt` handles an incorrect password.

**1. Build the program**

```bash
cd ../03_filecrypt
make
```

Moves into the `03_filecrypt` directory and compiles the program using the Makefile.

**2. Create a sample file**

```bash
echo "top secret message" > plain.txt
```

Creates a text file named `plain.txt` containing the message `top secret message`.

**3. Encrypt the file**

```bash
./filecrypt -e -p demo123 plain.txt enc.bin
```

Encrypts `plain.txt` using the password `demo123` and saves the encrypted data in `enc.bin`.

**4. Check the encrypted file**

```bash
xxd enc.bin | head -3
```

Displays the first three lines of the encrypted file in hexadecimal format. The original message is no longer readable as normal text.

**5. Decrypt the file**

```bash
./filecrypt -d -p demo123 enc.bin out.txt && cat out.txt
```

Decrypts `enc.bin` using the correct password and saves the recovered message in `out.txt`. The `cat` command displays the decrypted message.

**Expected output:**

```text
top secret message
```

**6. Try an incorrect password**

```bash
./filecrypt -d -p wrongpass enc.bin bad.txt
```

Attempts to decrypt the file using an incorrect password. The program rejects the request because the password is incorrect, and no output file is created.

**7. Test Ctrl + C interruption**

```bash
./filecrypt -e -p demo123 plain.txt interrupted.bin
```

While the program is running, press `Ctrl + C` to interrupt the operation.

`filecrypt` handles the interrupt signal and stops the operation safely. If the operation is interrupted before completion, the incomplete output file is removed so that it is not mistaken for a successfully encrypted file.

## Design and Limits

- Uses `mmap()` to read input files.
- Uses PBKDF2-SHA256 to derive the encryption key from the password.
- Uses AES-256-GCM for encryption and integrity checking.
- Uses `OPENSSL_cleanse()` to wipe sensitive memory.
- Rejects incorrect passwords, corrupted or tampered files, invalid file formats, and existing output paths.
- Creates output files with `0600` permissions.
- Supports `filecrypt_main()` behind `#ifndef FILECRYPT_NO_MAIN` and resets `optind` for Custom Shell integration.

## Deliverables

- `James Lu - Command 3.c` (source code)
- `Makefile`
- `test.sh` (tests)
- `filecrypt.1` (man page)
