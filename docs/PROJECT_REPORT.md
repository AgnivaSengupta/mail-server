# Internet Technology Laboratory – Individual Assignment Report

**Project Title:** Design and Development of a Multi-Client Email Service Using C and Network Socket Programming  
**Course:** B.Tech. Information Technology – Internet Technology Laboratory  
**Roll No:** 2023ITB070   
**Technology Stack:** C (C99/C11), POSIX Sockets (TCP/IP), POSIX Threads (`pthread`), OpenSSL (`libcrypto`)  
**Repository:** [https://github.com/AgnivaSengupta/mail-server](https://github.com/AgnivaSengupta/mail-server)

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Problem Statement](#2-problem-statement)
3. [Learning Objectives & System Scope](#3-learning-objectives--system-scope)
4. [System Architecture](#4-system-architecture)
    - [4.1 Client-Server Architecture](#41-client-server-architecture)
    - [4.2 Application-Layer Protocol Design](#42-application-layer-protocol-design)
    - [4.3 Data Storage & Persistence Design](#43-data-storage--persistence-design)
    - [4.4 Algorithms and Flowcharts](#44-algorithms-and-flowcharts)
5. [Implementation Details](#5-implementation-details)
    - [5.1 Multi-Client Handling via POSIX Threads](#51-multi-client-handling-via-posix-threads)
    - [5.2 Session Management Subsystem](#52-session-management-subsystem)
    - [5.3 Cryptographic Authentication](#53-cryptographic-authentication)
    - [5.4 Mail Storage and Atomic State Update](#54-mail-storage-and-atomic-state-update)
6. [Test Plan, Test Cases, and Results](#6-test-plan-test-cases-and-results)
    - [6.1 Test Case Matrix](#61-test-case-matrix)
    - [6.2 Execution Evidence and Transcripts](#62-execution-evidence-and-transcripts)
7. [AI / LLM Usage Log](#7-ai--llm-usage-log)
8. [Security Considerations](#8-security-considerations)
9. [Limitations & Future Enhancements](#9-limitations--future-enhancements)
10. [Conclusion & References](#10-conclusion--references)

---

## 1. Introduction

The Internet relies on standardized communication protocols operating over the client-server paradigm. Among core Internet services, electronic mail (email) represents a foundational application-layer technology, conventionally governed by protocols such as SMTP, POP3, and IMAP.

This laboratory project designs and implements a multi-client email service in the **C programming language** using **Berkeley TCP/IP socket programming** and **POSIX threads (`pthread`)**. The software operates as a client-server system capable of handling concurrent network connections, registering users, securing credentials with cryptographic key derivation, managing active sessions, and enabling persistent mail exchange.

---

## 2. Problem Statement

The objective of this assignment is to design and develop a client-server email system meeting the following requirements:

1. **Central Server:** A standalone server listening on a configurable TCP port that manages concurrent client connections, authenticates users, prevents duplicate active logins, and persists user accounts and email messages on disk.
2. **Interactive Client:** A menu-driven terminal interface allowing users to register, log in, compose emails to one or more registered users, view their inbox and sent mail with timestamps and read status, read individual messages, and log out or disconnect cleanly.
3. **Concurrency:** Concurrent handling of multiple simultaneous client connections without server instability or data corruption.
4. **Resilience & Security:** Safe handling of connection drops, oversized or malformed protocol lines, special delimiter characters in user input, and salted password hashing.

---

## 3. Learning Objectives & System Scope

- **Socket Programming:** Creating, binding, listening, accepting, and connecting TCP sockets using POSIX APIs (`socket()`, `setsockopt()`, `bind()`, `listen()`, `accept()`, `connect()`, `send()`, `recv()`, `close()`).
- **Application Protocol Design:** Formulating line-oriented framing, field delimiters, percent-encoding, error codes, and request-response semantics.
- **Concurrent Server Architecture:** Implementing a thread-per-client model using `pthread_create()` and `pthread_detach()`, and protecting shared resources using POSIX mutexes (`pthread_mutex_t`).
- **Persistent Data Structures:** Designing flat-file serialization formats and performing atomic file updates via temporary files and POSIX `rename()`.
- **Robust Error Handling:** Suppressing `SIGPIPE` on socket writes via `MSG_NOSIGNAL`, draining oversized lines safely, and cleaning up sessions on unexpected client disconnects.

---

## 4. System Architecture

### 4.1 Client-Server Architecture

The system follows a two-tier architecture consisting of a multithreaded server (`email_server`) and interactive terminal clients (`email_client`).

```text
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

### 4.2 Application-Layer Protocol Design

The application protocol is a line-oriented, pipe-delimited text protocol transmitted over TCP.

#### Framing & Delimiters

- **Field Separator:** Pipe symbol (`|`).
- **Record Terminator:** Newline (`\n`), with optional `\r` stripped on receipt.
- **Percent-Encoding Mechanism:**
  To guarantee that user input containing `|`, `\n`, `\r`, or `%` does not corrupt protocol framing or storage records, fields are escaped using percent-encoding:
    - `%` $\rightarrow$ `%25`
    - `|` $\rightarrow$ `%7C`
    - `\n` $\rightarrow$ `%0A`
    - `\r` $\rightarrow$ `%0D`

#### Command Reference Table

| Command    | Arguments                     | Required State  | Success Response                                                                                    | Error Responses                                                                                                                                              |
| :--------- | :---------------------------- | :-------------- | :-------------------------------------------------------------------------------------------------- | :----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `PING`     | None                          | Any             | `PONG\n`                                                                                            | `ERROR\|INVALID_COMMAND_FORMAT`                                                                                                                              |
| `HELP`     | None                          | Any             | `COMMANDS\|PING,HELP,REGISTER,LOGIN,LOGOUT,SEND,INBOX,READ,SENT,QUIT\n`                             | `ERROR\|INVALID_COMMAND_FORMAT`                                                                                                                              |
| `REGISTER` | `user\|email\|pass`           | Any             | `OK\|REGISTERED\n`                                                                                  | `ERROR\|USER_EXISTS`, `ERROR\|EMAIL_EXISTS`, `ERROR\|INVALID_INPUT`, `ERROR\|STORAGE_ERROR`                                                                  |
| `LOGIN`    | `user\|pass`                  | Unauthenticated | `OK\|LOGIN_SUCCESS\|<user>\n`                                                                       | `ERROR\|INVALID_CREDENTIALS`, `ERROR\|USER_ALREADY_ACTIVE`, `ERROR\|ALREADY_LOGGED_IN`, `ERROR\|SERVER_BUSY`, `ERROR\|INVALID_INPUT`, `ERROR\|STORAGE_ERROR` |
| `LOGOUT`   | None                          | Authenticated   | `OK\|LOGGED_OUT\n`                                                                                  | `ERROR\|NOT_AUTHENTICATED`                                                                                                                                   |
| `SEND`     | `recipient(s)\|subject\|body` | Authenticated   | `OK\|SENT\|<first_id>\n`                                                                            | `ERROR\|RECIPIENT_NOT_FOUND`, `ERROR\|NOT_AUTHENTICATED`, `ERROR\|SEND_FAILED`                                                                               |
| `INBOX`    | None                          | Authenticated   | `OK\|INBOX\|<count>\n` followed by `MESSAGE\|<id>\|<ts>\|<from>\|<read>\|<sub>\n` lines and `END\n` | `ERROR\|NOT_AUTHENTICATED`, `ERROR\|STORAGE_ERROR`, `ERROR\|SERVER_ERROR`                                                                                    |
| `SENT`     | None                          | Authenticated   | `OK\|SENT\|<count>\n` followed by `MESSAGE\|<id>\|<ts>\|<to>\|<read>\|<sub>\n` lines and `END\n`    | `ERROR\|NOT_AUTHENTICATED`, `ERROR\|STORAGE_ERROR`, `ERROR\|SERVER_ERROR`                                                                                    |
| `READ`     | `<message_id>`                | Authenticated   | `OK\|MESSAGE\|<id>\|<ts>\|<from>\|<to>\|<sub>\|<body>\n`                                            | `ERROR\|NOT_AUTHENTICATED`, `ERROR\|INVALID_MESSAGE_ID`, `ERROR\|MESSAGE_NOT_FOUND`                                                                          |
| `QUIT`     | None                          | Any             | `BYE\n` (connection closes)                                                                         | `ERROR\|INVALID_COMMAND_FORMAT`                                                                                                                              |

### 4.3 Data Storage & Persistence Design

Data is persisted in structured text flat files inside the `data/` directory.

#### User Database File (`data/users.dat`)

```text
<username>|<email>|<salt_hex>|<hash_hex>\n
```

- `<username>`: Identifier (1–1023 printable characters, excluding `|`, `\r`, `\n`).
- `<email>`: Email address (1–1023 printable characters, excluding `|`, `\r`, `\n`).
- `<salt_hex>`: 32 lowercase hex characters representing a 16-byte random salt.
- `<hash_hex>`: 64 lowercase hex characters representing a 32-byte PBKDF2-HMAC-SHA256 digest (210,000 iterations).

#### Mail Database File (`data/messages.dat`)

```text
<id>|<timestamp>|<sender>|<recipient>|<is_read>|<subject>|<body>\n
```

- `<id>`: 64-bit unsigned integer ID (`unsigned long long`), assigned sequentially as $\max(\text{id}) + 1$.
- `<timestamp>`: Unix epoch timestamp in seconds (`time_t`).
- `<sender>`: Percent-encoded sender username (max 255 bytes).
- `<recipient>`: Percent-encoded recipient username (max 255 bytes).
- `<is_read>`: `0` (Unread) or `1` (Read).
- `<subject>`: Percent-encoded subject line (max 511 bytes).
- `<body>`: Percent-encoded message body (max 2047 bytes).

### 4.4 Algorithms and Flowcharts

#### 1. Connection and Worker Thread Lifecycle

```text
+-----------------------------------+
|           Start Server            |
+-----------------+-----------------+
                  |
                  v
+-----------------------------------+
| socket() -> setsockopt(REUSEADDR) |
| bind(INADDR_ANY, port) -> listen()|
+-----------------+-----------------+
                  |
                  v
         +--------+--------+
         |  accept() Loop  |<------------------+
         +--------+--------+                   |
                  |                            |
                  | New client_fd              |
                  v                            |
+-----------------------------------+          |
| Allocate ClientArg on Heap        |          |
| pthread_create(handle_client)     +----------+
+-----------------+-----------------+
                  |
                  | (Detached Worker Thread)
                  v
+-----------------------------------+
| pthread_detach(pthread_self())    |
| Send "WELCOME|Connected..."       |
+-----------------+-----------------+
                  |
                  v
       +----------+----------+
       | recv_line(client_fd)|<----------------+
       +----------+----------+                 |
                  |                            |
        +---------+---------+                  |
        | Bytes Read > 0 ?  |                  |
        +----+---------+----+                  |
             |         |                       |
         Yes |         | No / Error / QUIT     |
             v         |                       |
+------------------+   |                       |
| parse_command()  |   |                       |
| Execute Handler  |   |                       |
| send_response()  +---|-----------------------+
+------------------+   |
                       v
        +--------------+--------------+
        | session_logout(client_fd)   |
        | close(client_fd)            |
        | Thread Terminates           |
        +-----------------------------+
```

#### 2. Sending Email Algorithm (Single or Multiple Recipients)

```text
+---------------------------------------------+
| Client issues SEND|recipient(s)|subj|body   |
+----------------------+----------------------+
                       |
                       v
        +--------------+--------------+
        | Socket Authenticated in     |--- No ---> Return ERROR|NOT_AUTHENTICATED
        | session_get_username() ?    |
        +--------------+--------------+
                       | Yes
                       v
+---------------------------------------------+
| Tokenize comma-separated recipients         |
| Trim leading/trailing whitespace (max 64)   |
+----------------------+----------------------+
                       |
                       v
        +--------------+--------------+
        | At least 1 recipient AND    |--- No ---> Return ERROR|RECIPIENT_NOT_FOUND
        | all exist in users.dat ?    |
        +--------------+--------------+
                       | Yes
                       v
+---------------------------------------------+
| For each recipient: call mail_send()        |
|   1. Lock mail_mutex                        |
|   2. Scan messages.dat for next_id          |
|   3. Set timestamp = time(NULL), is_read = 0|
|   4. Percent-encode fields & append record  |
|   5. fflush() & unlock mail_mutex           |
+----------------------+----------------------+
                       |
                       v
+---------------------------------------------+
| Return OK|SENT|<first_message_id>           |
+---------------------------------------------+
```

#### 3. Atomic Read & Mark-as-Read Algorithm

```text
+---------------------------------------------------+
| Client issues READ|<message_id>                   |
+-------------------------+-------------------------+
                          |
                          v
           +--------------+--------------+
           | Socket Authenticated ?      |--- No ---> Return ERROR|NOT_AUTHENTICATED
           +--------------+--------------+
                          | Yes
                          v
+---------------------------------------------------+
| Lock mail_mutex                                   |
| Open data/messages.dat (r) & data/messages.tmp (w)|
+-------------------------+-------------------------+
                          |
                          v
+---------------------------------------------------+
| Stream each line from data/messages.dat:          |
|   If record.id == target_id AND                   |
|      (user == recipient OR user == sender):       |
|        - If user == recipient: set is_read = 1    |
|        - Copy record to output struct             |
|        - Write updated record to messages.tmp     |
|   Else:                                           |
|        - Copy original line to messages.tmp       |
+-------------------------+-------------------------+
                          |
                          v
           +--------------+--------------+
           | Target Found & Write OK ?   |
           +------+---------------+------+
                  |               |
              Yes |               | No
                  v               v
+-----------------------+   +-----------------------+
| rename(messages.tmp,  |   | remove(messages.tmp)  |
|        messages.dat)  |   +-----------+-----------+
+-----------+-----------+               |
            |                           |
            v                           v
+-----------------------+   +-----------------------+
| Unlock mail_mutex     |   | Unlock mail_mutex     |
| Return OK|MESSAGE|... |   | Return ERROR|         |
+-----------------------+   | MESSAGE_NOT_FOUND     |
                            +-----------------------+
```

---

## 5. Implementation Details

### 5.1 Multi-Client Handling via POSIX Threads

- Each accepted client socket is passed via a heap-allocated `ClientArg` struct to a worker thread created with `pthread_create(&thread, NULL, handle_client, args)`.
- The worker calls `pthread_detach(pthread_self())` so kernel thread resources are reclaimed automatically on termination.
- Shared state is protected by three independent, non-nested mutexes (preventing deadlocks):
    - `auth_mutex` (`auth/auth.c`): Serializes reads and appends to `data/users.dat`.
    - `session_mutex` (`session/session.c`): Serializes access to the in-memory `sessions[128]` table.
    - `mail_mutex` (`mail/mail.c`): Serializes access to `data/messages.dat` and `data/messages.tmp`.

### 5.2 Session Management Subsystem

Active logins are tracked in memory:

```c
typedef struct {
    int active;
    int socket_fd;
    char username[SESSION_USERNAME_SIZE];
} Session;
static Session sessions[MAX_SESSIONS];
```

- `session_login()` rejects login attempts if the current socket is already authenticated (`SESSION_ALREADY_LOGGED_IN`), if the username is already logged in on another active socket (`SESSION_USER_ACTIVE`), or if all 128 slots are occupied (`SESSION_TABLE_FULL`).
- On `LOGOUT`, `QUIT`, or unexpected socket closure, `session_logout(client_fd)` clears the slot.
- On the client side, selecting **Logout** prompts for `y/n` confirmation before dispatching `LOGOUT`, and clears the local `logged_in` flag only upon receiving `OK|LOGGED_OUT`.

### 5.3 Cryptographic Authentication

- **Key Derivation:** Uses OpenSSL `PKCS5_PBKDF2_HMAC` with `EVP_sha256()`, a 16-byte salt, 210,000 iterations, and a 32-byte output hash.
- **Random Salt Generation:** Uses `RAND_bytes(salt, 16)` for each user registration.
- **Constant-Time Comparison:** Uses `CRYPTO_memcmp(expected_hash, actual_hash, 32)` during login to prevent timing side-channel leaks.

### 5.4 Mail Storage and Atomic State Update

- **Listing Cap:** `INBOX` and `SENT` retrieve up to `MAX_LIST_MESSAGES` (100) messages per request.
- **Atomic Read Updates:** `mail_read_message()` streams `data/messages.dat` into `data/messages.tmp`, updates `is_read = 1` if the reader is the recipient, and replaces `data/messages.dat` via `rename()`.
- **Client Formatting:** `client/client.c` decodes percent-encoded subjects and bodies, formats Unix timestamps into local `YYYY-MM-DD HH:MM` (tables) and `YYYY-MM-DD HH:MM:SS` (message view) strings, and drains oversized responses safely returning `-2` so socket framing remains synchronized.

---

## 6. Test Plan, Test Cases, and Results

### 6.1 Test Case Matrix

|  Test ID  | Category       | Objective / Scenario               | Input / Action                                  | Expected Result                                                               |  Status  |
| :-------: | :------------- | :--------------------------------- | :---------------------------------------------- | :---------------------------------------------------------------------------- | :------: |
| **TC-01** | Server Init    | Start server with default port     | `./email_server`                                | Listens on port 8000                                                          | **PASS** |
| **TC-02** | Server Init    | Start server with custom port      | `./email_server 9050`                           | Listens on port 9050                                                          | **PASS** |
| **TC-03** | Server Init    | Invalid port handling              | `./email_server 99999`                          | Prints `Invalid port number` and exits                                        | **PASS** |
| **TC-04** | Client Connect | Valid connection to server         | `./email_client 127.0.0.1 8000`                 | Receives `WELCOME\|Connected to Email Server`                                 | **PASS** |
| **TC-05** | Registration   | New user registration              | `REGISTER\|rahul\|rahul@iitlab.local\|Pass123`  | `OK\|REGISTERED`                                                              | **PASS** |
| **TC-06** | Registration   | Duplicate username                 | `REGISTER\|rahul\|new@iitlab.local\|Pass123`    | `ERROR\|USER_EXISTS`                                                          | **PASS** |
| **TC-07** | Registration   | Duplicate email                    | `REGISTER\|rahul2\|rahul@iitlab.local\|Pass123` | `ERROR\|EMAIL_EXISTS`                                                         | **PASS** |
| **TC-08** | Authentication | Valid credentials                  | `LOGIN\|rahul\|Pass123`                         | `OK\|LOGIN_SUCCESS\|rahul`                                                    | **PASS** |
| **TC-09** | Authentication | Wrong password                     | `LOGIN\|rahul\|WrongPass`                       | `ERROR\|INVALID_CREDENTIALS`                                                  | **PASS** |
| **TC-10** | Authentication | Non-existent user                  | `LOGIN\|ghost\|Pass123`                         | `ERROR\|INVALID_CREDENTIALS`                                                  | **PASS** |
| **TC-11** | Concurrency    | Duplicate login on 2nd client      | Login `rahul` on Terminal 2                     | `ERROR\|USER_ALREADY_ACTIVE`                                                  | **PASS** |
| **TC-12** | Mail Send      | Single & multiple recipients       | `SEND\|alice, bob\|Lab 1\|Complete sockets.`    | `OK\|SENT\|<id>`                                                              | **PASS** |
| **TC-13** | Mail Send      | Non-existent recipient             | `SEND\|nobody\|Hi\|Test`                        | `ERROR\|RECIPIENT_NOT_FOUND`                                                  | **PASS** |
| **TC-14** | Mail Inbox     | Retrieve inbox messages            | `INBOX`                                         | Displays table with ID, Sender, Date/Time, Status, Subject                    | **PASS** |
| **TC-15** | Mail Read      | Read email by ID                   | `READ\|1`                                       | Full message displayed; recipient status becomes `Read`                       | **PASS** |
| **TC-16** | Mail Sent      | View sent history & read sent mail | `SENT`, then `READ\|1`                          | Lists sent messages; sender can view message without altering `Unread` status | **PASS** |
| **TC-17** | Delimiters     | Special character escaping         | Subject: `A\|B`, Body: `C%D`                    | Safely percent-encoded on wire and decoded on display                         | **PASS** |
| **TC-18** | Concurrency    | 3 concurrent clients               | Alice, Bob, Charlie active simultaneously       | Concurrent send, inbox, and read operations succeed                           | **PASS** |
| **TC-19** | Robustness     | Abrupt client disconnect           | Client terminated via `Ctrl+C`                  | Server logs disconnect, frees session slot, continues running                 | **PASS** |

### 6.2 Execution Evidence and Transcripts

#### Server Startup & Client Connections

```text
=================================
       EMAIL SERVER STARTED
=================================
Listening on port: 8000
Waiting for clients...

[SERVER] New connection from 127.0.0.1:51240
[SERVER] Client connected (fd=4)
[SERVER] New connection from 127.0.0.1:51242
[SERVER] Client connected (fd=5)
[SERVER] New connection from 127.0.0.1:51244
[SERVER] Client connected (fd=6)
```

#### Client 1 (Alice) Sending Email to Bob

```text
========== EMAIL DASHBOARD ==========
1. Compose Email
2. View Inbox
3. Read Email
4. View Sent Emails
5. Logout
6. Exit
=====================================
Select option: 1
Recipient username(s): bob
Subject: Project Discussion
Message: Hello Bob, let's discuss our project tomorrow.
OK|SENT|1
```

#### Client 2 (Bob) Receiving and Reading Email

```text
Select option: 2

INBOX
+------+----------------------+---------------------+----------+------------------------------+
| ID   | Sender               | Date/Time           | Status   | Subject                      |
+------+----------------------+---------------------+----------+------------------------------+
| 1    | alice                | 2026-11-06 07:58    | Unread   | Project Discussion           |
+------+----------------------+---------------------+----------+------------------------------+

Select option: 3
Enter message ID: 1

============================================================
                     EMAIL MESSAGE
============================================================
Message ID : 1
Date       : 2026-11-06 07:58:12
From       : alice
To         : bob
Subject    : Project Discussion
------------------------------------------------------------
Message:

Hello Bob, let's discuss our project tomorrow.
============================================================
```

---

## 7. AI / LLM Usage Log

|     Date     | Task                         | AI Tool | Prompt / Question                                                                   | What Was Used                           | Student Verification                                             |
| :----------: | :--------------------------- | :-----: | :---------------------------------------------------------------------------------- | :-------------------------------------- | :--------------------------------------------------------------- |
| **05/11/26** | Project Architecture & Scope | ChatGPT | Asked about assignment scope, mail flow, concurrency, and directory layout          | Architecture design & file structure    | Reviewed against assignment specifications                       |
| **05/11/26** | TCP Server Foundation        | ChatGPT | Requested initial server implementation and line-by-line explanation of `server.c`  | Concept/code                            | Compiled with `make` and tested via `nc`                         |
| **05/11/26** | Application Protocol         | ChatGPT | Asked to design Step 2 custom pipe-delimited protocol and command parser            | Code suggestion (`protocol.c`/`.h`)     | Verified command parsing and error responses                     |
| **05/11/26** | Auth & Session Module        | ChatGPT | Requested Step 3 registration, PBKDF2 hashing, login, and session tracking          | Code suggestion (`auth.c`, `session.c`) | Tested registration, duplicate checks, and login via `nc`        |
| **05/11/26** | Mail Management              | ChatGPT | Requested Step 4 implementation for `SEND`, `INBOX`, `READ`, and `SENT`             | Code suggestion (`mail.c`/`.h`)         | Verified message flow and read-status updates across 2 terminals |
| **06/11/26** | CLI Client & Login Bug       | ChatGPT | Asked for menu-driven CLI client and debugged `INVALID_COMMAND_FORMAT` on login     | Code suggestion & bug fix               | Recompiled and verified 2-field `LOGIN` command                  |
| **06/11/26** | Client UI Formatting         | ChatGPT | Asked for formatted `INBOX`/`SENT` tables, `READ` panel, and logout confirmation    | Code suggestion (`client.c`)            | Modified client and tested table rendering in CLI                |
| **06/11/26** | Code & Report Audit          | ChatGPT | Shared GitHub repo (`mail-server`) to review codebase, `README`, and project report | Code review & documentation audit       | Cross-checked identified edge cases against repository           |

---

## 8. Security Considerations

1. **Salted Password Hashing:** Plaintext passwords are never stored on disk. Each account uses a unique 16-byte cryptographic salt and 210,000 iterations of PBKDF2-HMAC-SHA256.
2. **Constant-Time Hash Verification:** `CRYPTO_memcmp()` prevents timing side-channel attacks during authentication.
3. **Delimiter & NUL-Byte Injection Protection:** Special characters (`%`, `|`, `\n`, `\r`) are percent-encoded, and encoded NUL bytes (`%00`) are explicitly rejected during decoding.
4. **Bounds Checking & Safe I/O:** Input reading and formatting use bounded functions (`fgets()`, `snprintf()`, `vsnprintf()`), and string copies in session management validate string length against buffer capacity beforehand.
5. **Broken Pipe Protection:** Both client and server pass `MSG_NOSIGNAL` to `send()` in `send_all()`, preventing abrupt peer disconnections from terminating the process with `SIGPIPE`.

---

## 9. Limitations & Future Enhancements

### Current Limitations

1. **Unencrypted Transport:** Protocol traffic is transmitted in plaintext over TCP without TLS encryption.
2. **Flat-File Linear Scans:** Looking up users and messages requires $O(N)$ linear scans of `data/users.dat` and `data/messages.dat`, and listing is capped at `MAX_LIST_MESSAGES` (100) records.
3. **Single-Line Interactive Body Input:** The CLI reads the message body using a single `fgets()` call rather than a multi-line editor.
4. **Fixed Field & Recipient Caps:** Protocol fields are bounded to 1024 bytes (`MAIL_BODY_SIZE` is 2048 bytes, `MAIL_SUBJECT_SIZE` is 512 bytes) and `SEND` accepts up to 64 recipients per command.

### Future Enhancements

- **TLS/SSL Encryption:** Wrap client and server sockets with OpenSSL TLS (`SSL_read` / `SSL_write`) to encrypt credentials and messages in transit.
- **Indexed Storage:** Replace flat files with SQLite or an indexed storage engine for $O(1)$ / $O(\log N)$ message lookups and pagination beyond 100 messages.
- **Message Deletion & Attachments:** Add a `DELETE|<id>` command and support Base64-encoded file attachments.

---

## 10. Conclusion & References

### Conclusion

This project implements a complete, concurrent, multithreaded client-server email system in C using POSIX TCP sockets and threads. It fulfills the functional requirements of user registration, PBKDF2-based authentication, active session tracking, single and multi-recipient mail delivery, formatted inbox/sent views, and atomic read-state persistence.

### References

1. W. Richard Stevens, Bill Fenner, and Andrew M. Rudoff. _UNIX Network Programming, Volume 1: The Sockets Networking API_, 3rd Edition, Addison-Wesley, 2003.
2. Michael Kerrisk. _The Linux Programming Interface: A Linux and UNIX System Programming Handbook_, No Starch Press, 2010.
3. OpenSSL Project Documentation: `PKCS5_PBKDF2_HMAC(3)`, `RAND_bytes(3)`, and `CRYPTO_memcmp(3)`.
4. IEEE Std 1003.1-2017 (POSIX.1-2017) Standard for Information Technology – Portable Operating System Interface.
