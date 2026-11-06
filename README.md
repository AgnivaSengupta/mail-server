# Multi-Client Email Service

A lightweight, concurrent, self-contained educational email service written in **C** using **TCP/IP Berkeley socket programming** and **POSIX threads (`pthread`)**.

This project implements a central multithreaded email server and an interactive command-line email client. It satisfies the academic requirements of the *B.Tech. Information Technology – Internet Technology Laboratory* curriculum.

---

## Table of Contents

- [Features](#features)
- [Architecture Overview](#architecture-overview)
- [Directory Structure](#directory-structure)
- [Prerequisites & Dependencies](#prerequisites--dependencies)
- [Compilation & Build](#compilation--build)
- [Execution Guide](#execution-guide)
  - [Starting the Server](#1-starting-the-server)
  - [Starting the Client](#2-starting-the-client)
- [Application Protocol Specification](#application-protocol-specification)
  - [Protocol Framing & Encoding](#protocol-framing--encoding)
  - [Command Reference](#command-reference)
  - [Response Formats](#response-formats)
- [Data Storage Schema](#data-storage-schema)
- [Multi-Client Demonstration Walkthrough](#multi-client-demonstration-walkthrough)
- [Assignment Compliance & Correctness Check](#assignment-compliance--correctness-check)
- [Known Limitations & Recommendations](#known-limitations--recommendations)
- [Viva Voce Cheat Sheet](#viva-voce-cheat-sheet)

---

## Features

- **Concurrent Multi-Client Handling:** POSIX multi-threading (`pthread`) with detached threads (`pthread_detach`), enabling concurrent communication with arbitrary simultaneous clients.
- **Robust Application Protocol:** Pipe-delimited (`|`), newline-terminated (`\n`) custom text protocol with **percent-encoding** (`%25`, `%7C`, `%0A`, `%0D`) to prevent delimiter collision and protocol injection.
- **Enterprise-Grade Authentication:**
  - Password security using **PBKDF2-HMAC-SHA256** (210,000 iterations, 16-byte cryptographically secure random salt via OpenSSL).
  - Timing-attack resistant verification (`CRYPTO_memcmp`).
  - No plaintext passwords stored on disk.
  - Prevention of duplicate registrations (unique username and unique email address).
- **Session Management:**
  - Dedicated in-memory session tracking table.
  - Mutual exclusion preventing duplicate logins on the same socket or concurrent logins of the same user across multiple sockets.
  - Automatic session cleanup on graceful logout or unexpected client disconnect.
- **Mail Operations:**
  - Send email with recipient existence validation.
  - Inbox retrieval with read/unread flags.
  - Sent mail history retrieval.
  - Message inspection with automatic atomic read-status transition.
- **Persistent Storage:** File-based flat-file storage (`data/users.dat` and `data/messages.dat`) protected by thread-safe POSIX mutexes and atomic file replacement (`.tmp` + `rename()`).

---

## Architecture Overview

```
                      +-----------------------------+
                      |       Email Client 1        |
                      |  (Terminal / Interactive)   |
                      +--------------+--------------+
                                     |
                                     | TCP Socket
                                     | (Port 8000)
                                     v
+---------------------+    +---------+---------+    +---------------------+
|   Email Client 2    |--->|   Email Server    |<---|    Email Client 3   |
| (Terminal / Alice)  |    |  (Master Listener)|    |  (Terminal / Bob)   |
+---------------------+    +---------+---------+    +---------------------+
                                     |
                         Worker Thread per Client
                   +-----------------+-----------------+
                   |                 |                 |
                   v                 v                 v
             +-----------+     +-----------+     +-----------+
             | Worker T1 |     | Worker T2 |     | Worker T3 |
             +-----+-----+     +-----+-----+     +-----+-----+
                   |                 |                 |
                   +--------+--------+--------+--------+
                            |                 |
                            v                 v
                    +---------------+ +---------------+
                    | Session Table | | Mutex Locks   |
                    | (Max 128)     | | (auth/mail/   |
                    +---------------+ |  session)     |
                            |         +---------------+
                            v                 |
                    +-------------------------v-------+
                    | Persistent Data Files           |
                    | - data/users.dat (PBKDF2 Hashed)|
                    | - data/messages.dat (Atomic)    |
                    +---------------------------------+
```

---

## Directory Structure

```
mail-server/
├── Makefile                # Build configuration for server and client
├── README.md               # Quickstart and project reference
├── auth/                   # Authentication & cryptographic hashing
│   ├── auth.c
│   └── auth.h
├── client/                 # Interactive terminal client
│   └── client.c
├── common/                 # Protocol parser and definitions
│   ├── protocol.c
│   └── protocol.h
├── data/                   # Persistent flat-file database
│   ├── messages.dat        # Serialized email records
│   └── users.dat           # Serialized user records with salt & hash
├── docs/                   # Detailed academic documentation
│   ├── ARCHITECTURE.md     # In-depth architectural design & diagrams
│   └── PROJECT_REPORT.md   # Complete lab submission project report
├── mail/                   # Mail store, retrieve, and status update
│   ├── mail.c
│   └── mail.h
├── server/                 # Master socket listener and request router
│   └── server.c
└── session/                # Active user connection & session registry
    ├── session.c
    └── session.h
```

---

## Prerequisites & Dependencies

The project targets Linux/POSIX environments (e.g., Ubuntu, Debian, Fedora, Arch, WSL on Windows, macOS):

- **C Compiler:** `gcc` (with C99/C11 support)
- **Make:** GNU `make`
- **OpenSSL Library:** `libssl-dev` (Debian/Ubuntu) or `openssl-devel` (Fedora/RHEL) for PBKDF2 hashing functions (`-lcrypto`)
- **POSIX Threads:** `pthread` (standard GNU C Library)

### Installing Prerequisites (Ubuntu / Debian / WSL):
```bash
sudo apt update
sudo apt install build-essential libssl-dev
```

---

## Compilation & Build

Compile both the server and client binaries using the provided `Makefile`:

```bash
# Build both email_server and email_client
make

# Clean compilation artifacts
make clean
```

### Manual Compilation Commands:
```bash
# Compile Server
gcc -Wall -Wextra -g -pthread \
    server/server.c common/protocol.c auth/auth.c session/session.c mail/mail.c \
    -o email_server -lcrypto

# Compile Client
gcc -Wall -Wextra -g client/client.c -o email_client
```

---

## Execution Guide

### 1. Starting the Server

The server accepts an optional port argument (default is `8000`):

```bash
# Run server on default port 8000
./email_server

# Or specify a custom port (e.g., 9050)
./email_server 9050
```

**Expected Server Output:**
```
=================================
       EMAIL SERVER STARTED      
=================================
Listening on port: 8000
Waiting for clients...
```

### 2. Starting the Client

The client accepts optional arguments for server IP and server port (defaults: `127.0.0.1` and `8000`):

```bash
# Connect to localhost on default port 8000
./email_client

# Connect to a specific host and port
./email_client 127.0.0.1 8000
```

Upon connection, the client displays:
```
WELCOME|Connected to Email Server

========== EMAIL SERVICE ==========
1. Register
2. Login
3. Exit
===================================
Select option: 
```

---

## Application Protocol Specification

The system uses a custom text-based application-layer protocol running over reliable TCP.

### Protocol Framing & Encoding
- **Field Separator:** Pipe character (`|`).
- **Message Terminator:** Newline character (`\n` or `\r\n`).
- **Delimiter Escaping:** Percent-encoding is used for user content to allow safe transmission of special characters:
  - `%` $\rightarrow$ `%25`
  - `|` $\rightarrow$ `%7C`
  - `\n` $\rightarrow$ `%0A`
  - `\r` $\rightarrow$ `%0D`

### Command Reference

| Command | Syntax | Description | Required State |
| :--- | :--- | :--- | :--- |
| `PING` | `PING\n` | Health-check / ping test | Any |
| `HELP` | `HELP\n` | List available server commands | Any |
| `REGISTER` | `REGISTER\|<username>\|<email>\|<password>\n` | Register a new user | Unauthenticated |
| `LOGIN` | `LOGIN\|<username>\|<password>\n` | Authenticate user session | Unauthenticated |
| `LOGOUT` | `LOGOUT\n` | End active session | Authenticated |
| `SEND` | `SEND\|<recipient>\|<subject>\|<body>\n` | Send email to user | Authenticated |
| `INBOX` | `INBOX\n` | List messages received by user | Authenticated |
| `SENT` | `SENT\n` | List messages sent by user | Authenticated |
| `READ` | `READ\|<message_id>\n` | View full message & mark read | Authenticated |
| `QUIT` | `QUIT\n` | Disconnect connection | Any |

### Response Formats

- **Success Responses:**
  - `OK|REGISTERED`
  - `OK|LOGIN_SUCCESS|<username>`
  - `OK|LOGGED_OUT`
  - `OK|SENT|<message_id>`
  - `OK|INBOX|<count>` followed by `MESSAGE|<id>|<timestamp>|<sender>|<is_read>|<subject>` and ending with `END`
  - `OK|SENT|<count>` followed by `MESSAGE|<id>|<timestamp>|<recipient>|<is_read>|<subject>` and ending with `END`
  - `OK|MESSAGE|<id>|<timestamp>|<sender>|<recipient>|<subject>|<body>`
- **Error Responses:**
  - `ERROR|USER_EXISTS` / `ERROR|EMAIL_EXISTS`
  - `ERROR|INVALID_CREDENTIALS` / `ERROR|INVALID_INPUT`
  - `ERROR|ALREADY_LOGGED_IN` / `ERROR|USER_ALREADY_ACTIVE`
  - `ERROR|NOT_AUTHENTICATED`
  - `ERROR|RECIPIENT_NOT_FOUND`
  - `ERROR|MESSAGE_NOT_FOUND` / `ERROR|INVALID_MESSAGE_ID`
  - `ERROR|UNKNOWN_COMMAND` / `ERROR|INVALID_COMMAND_FORMAT`

---

## Data Storage Schema

All persistent state is stored in plain-text flat files inside the `data/` directory.

### 1. User Database (`data/users.dat`)
Record structure (pipe-delimited):
```
<username>|<email>|<salt_hex>|<hash_hex>
```
Example:
```
alice|alice@iitlab.local|2cc2ee202bb991c60101842bba773e97|49fe2231f972f7a73626a3cbe91ee852df1dfaa1ea2cecc9f12484e616765335
bob|bob@iitlab.local|5bd887dbe5871cf1fc31122f332b0fb9|5b034c1888ed48ffc2b86720d56173b81d2b3aa22d5f68eecf107d193a723a88
```

### 2. Message Store (`data/messages.dat`)
Record structure (pipe-delimited):
```
<id>|<timestamp>|<sender>|<recipient>|<is_read>|<subject>|<body>
```
- `<id>`: 64-bit monotonically increasing integer.
- `<timestamp>`: Unix epoch time (`time_t`).
- `<is_read>`: `0` (Unread) or `1` (Read).
- `<subject>`, `<body>`: Percent-encoded strings.

---

## Multi-Client Demonstration Walkthrough

To satisfy the mandatory requirement of demonstrating **at least 3 simultaneous clients**:

### Step 1: Open 4 Separate Terminal Windows
- **Terminal 0 (Server):** `./email_server 8000`
- **Terminal 1 (Alice):** `./email_client 127.0.0.1 8000`
- **Terminal 2 (Bob):** `./email_client 127.0.0.1 8000`
- **Terminal 3 (Charlie):** `./email_client 127.0.0.1 8000`

### Step 2: Register & Log in Users
1. In **Terminal 1**, register `alice` (`alice@iitlab.local`), then log in.
2. In **Terminal 2**, register `bob` (`bob@iitlab.local`), then log in.
3. In **Terminal 3**, register `charlie` (`charlie@iitlab.local`), then log in.

### Step 3: Send & Receive Messages Simultaneously
1. **Alice (Term 1)** sends email to `bob`:
   - Subject: `Lab Assignment 1`
   - Message: `Please review socket programming code.`
2. **Charlie (Term 3)** sends email to `bob`:
   - Subject: `Meeting Tomorrow`
   - Message: `Let us meet at 4 PM in the library.`
3. **Bob (Term 2)** selects `2. View Inbox`:
   - Bob observes two emails listed with `Status: Unread`.
4. **Bob (Term 2)** selects `3. Read Email` and enters message ID `1`:
   - Bob reads the email from Alice.
   - Bob views his inbox again; message 1 now displays `Status: Read`.
5. **Alice (Term 1)** selects `4. View Sent Emails`:
   - Alice sees her sent email to Bob recorded with ID `1`.

### Step 4: Disconnection & Concurrency Test
- Close Charlie's terminal abruptly (`Ctrl+C`).
- Notice in Terminal 0 that the server logs `Client disconnected (fd=...)` and continues running smoothly without crashing.
- Alice and Bob continue sending and reading emails uninterrupted.

---

## Assignment Compliance & Correctness Check

| Assignment Section | Requirement | Status | Notes |
| :--- | :--- | :--- | :--- |
| **3.A Server** | Multi-client TCP socket server | **PASSED** | Uses `socket`, `bind`, `listen`, `accept`, `pthread_create`. |
| **3.A Server** | Configurable TCP port | **PASSED** | `./email_server [port]` supported. |
| **3.A Server** | Non-crashing on client drop | **PASSED** | Error checking on all I/O; detached threads terminate safely. |
| **3.B Client** | Menu-driven terminal UI | **PASSED** | Clean authentication and dashboard menus. |
| **3.B & 4.3 Sending** | One or more recipients | **PASSED** | Supports single or comma-separated recipients (`bob, charlie`) with atomic pre-validation. |
| **4.1 Registration** | Username, email, password | **EXCEEDED** | Prevents duplicate usernames and emails; hashes passwords via PBKDF2-HMAC-SHA256. |
| **4.2 Login** | Authenticate, handle invalid/active | **EXCEEDED** | Handles wrong password, non-existent user, and concurrent duplicate logins. |
| **4.4 Receiving** | Inbox listing & message reading | **PASSED** | Displays ID, Sender, Date/Time (`YYYY-MM-DD HH:MM`), Read/Unread Status, and Subject. Senders can also read their sent emails. |
| **4.5 Sent Mail** | View sent messages | **PASSED** | Lists sender's outgoing messages with timestamps and read status. |
| **4.6 Concurrency** | $\ge 3$ simultaneous clients | **PASSED** | Tested with multi-client threads and mutex-locked shared resources. |
| **5 Protocol** | Custom application protocol | **EXCEEDED** | Pipe-delimited, newline-terminated with percent-encoding. |
| **6 Data Storage** | Persistent file storage | **PASSED** | Atomic updates using temp file and rename. |

---
