#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 8000

#define BUFFER_SIZE 8192
#define INPUT_SIZE 2048

static int logged_in = 0;

/* Read user input safely. */
static int read_input(const char *prompt, char *buffer, size_t size) {
    printf("%s", prompt);
    fflush(stdout);

    if (!fgets(buffer, size, stdin))
        return -1;

    buffer[strcspn(buffer, "\n")] = '\0';

    return 0;
}

/* Send all bytes over TCP. */
static int send_all(int fd, const char *buffer, size_t length) {
    size_t total = 0;

    while (total < length) {
        ssize_t sent = send(fd, buffer + total, length - total, 0);

        if (sent <= 0) {
            if (sent < 0 && errno == EINTR)
                continue;
            return -1;
        }

        total += (size_t)sent;
    }

    return 0;
}

/*
 * Receive one newline-terminated response.
 * Returns:
 *   1  on success
 *   0  on clean EOF
 *  -1  on socket error
 *  -2  if response exceeded buffer size (line drained until '\n')
 */
static int recv_line(int fd, char *buffer, size_t size) {
    if (!buffer || size < 2)
        return -1;

    size_t used = 0;

    while (used < size - 1) {
        char c;

        ssize_t received = recv(fd, &c, 1, 0);

        if (received == 0) {
            buffer[used] = '\0';
            return used > 0 ? 1 : 0;
        }

        if (received < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (c == '\n') {
            buffer[used] = '\0';
            return 1;
        }

        if (c != '\r')
            buffer[used++] = c;
    }

    /* Buffer filled before '\n': null-terminate and drain remainder of line. */
    buffer[used] = '\0';

    while (1) {
        char c;
        ssize_t received = recv(fd, &c, 1, 0);

        if (received < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (received == 0 || c == '\n')
            break;
    }

    return -2;
}


/* Percent-encode protocol fields. */
static int encode_field(
    const char *input,
    char *output,
    size_t output_size
) {
    size_t j = 0;

    for (size_t i = 0; input[i] != '\0'; i++) {
        const char *replacement = NULL;

        switch (input[i]) {
            case '%': replacement = "%25"; break;
            case '|': replacement = "%7C"; break;
            case '\n': replacement = "%0A"; break;
            case '\r': replacement = "%0D"; break;
        }

        if (replacement) {
            if (j + 3 >= output_size)
                return -1;

            memcpy(output + j, replacement, 3);
            j += 3;
        } else {
            if (j + 1 >= output_size)
                return -1;

            output[j++] = input[i];
        }
    }

    output[j] = '\0';
    return 0;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void decode_field(const char *input, char *output, size_t output_size) {
    size_t i = 0, j = 0;

    if (!input || output_size == 0)
        return;

    while (input[i] != '\0' && j + 1 < output_size) {
        if (input[i] == '%' && input[i + 1] != '\0' && input[i + 2] != '\0') {
            int hi = hex_value(input[i + 1]);
            int lo = hex_value(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                output[j++] = (char)((hi << 4) | lo);
                i += 3;
                continue;
            }
        }
        output[j++] = input[i++];
    }
    output[j] = '\0';
}

/* Split pipe-delimited line in-place while preserving empty fields. */
static int split_fields(char *line, char *fields[], int max_fields) {
    int count = 0;
    fields[count++] = line;

    for (char *p = line; *p != '\0'; p++) {
        if (*p == '|') {
            *p = '\0';
            if (count < max_fields) {
                fields[count++] = p + 1;
            }
        }
    }
    return count;
}

static void display_mailbox_table(
    int sockfd,
    const char *mailbox,
    const char *person_column
) {
    char response[BUFFER_SIZE];

    printf("\n%s\n", mailbox);

    printf("+------+----------------------+---------------------+----------+------------------------------+\n");
    printf("| ID   | %-20s | Date/Time           | Status   | Subject                      |\n",
           person_column);
    printf("+------+----------------------+---------------------+----------+------------------------------+\n");

    while (1) {
        int result = recv_line(sockfd, response, sizeof(response));

        if (result == -2) {
            printf("Warning: Skipping oversized mailbox entry.\n");
            continue;
        }
        
        if (result <= 0) {
            printf("Server disconnected while reading mailbox.\n");
            break;
        }

        if (strcmp(response, "END") == 0)
            break;

        if (strncmp(response, "MESSAGE|", 8) != 0)
            continue;

        char *fields[6];
        if (split_fields(response, fields, 6) < 6)
            continue;

        char *id = fields[1];
        time_t raw_time = (time_t)atoll(fields[2]);
        char time_str[32];
        struct tm *tm_info = localtime(&raw_time);
        if (tm_info) {
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", tm_info);
        } else {
            snprintf(time_str, sizeof(time_str), "%s", fields[2]);
        }

        char *person = fields[3];
        char *read_status = fields[4];
        char decoded_subject[INPUT_SIZE];
        decode_field(fields[5], decoded_subject, sizeof(decoded_subject));

        printf("| %-4.4s | %-20.20s | %-19.19s | %-8.8s | %-28.28s |\n",
               id,
               person,
               time_str,
               strcmp(read_status, "1") == 0 ? "Read" : "Unread",
               decoded_subject);
    }

    printf("+------+----------------------+---------------------+----------+------------------------------+\n\n");
}

static void display_read_message(char *response) {
    char *fields[8];
    if (split_fields(response, fields, 8) < 8) {
        printf("Malformed message response.\n");
        return;
    }

    char *id = fields[2];
    char *timestamp = fields[3];
    char *sender = fields[4];
    char *recipient = fields[5];

    time_t raw_time = (time_t)atoll(timestamp);
    char time_str[64];
    struct tm *tm_info = localtime(&raw_time);
    if (tm_info) {
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    } else {
        snprintf(time_str, sizeof(time_str), "%s", timestamp);
    }

    char decoded_subject[INPUT_SIZE];
    char decoded_body[BUFFER_SIZE];
    decode_field(fields[6], decoded_subject, sizeof(decoded_subject));
    decode_field(fields[7], decoded_body, sizeof(decoded_body));

    printf("\n");
    printf("============================================================\n");
    printf("                     EMAIL MESSAGE\n");
    printf("============================================================\n");

    printf("Message ID : %s\n", id);
    printf("Date       : %s\n", time_str);
    printf("From       : %s\n", sender);
    printf("To         : %s\n", recipient);
    printf("Subject    : %s\n", decoded_subject);

    printf("------------------------------------------------------------\n");
    printf("Message:\n\n");

    if (decoded_body[0] != '\0')
        printf("%s\n", decoded_body);
    else
        printf("(Empty message)\n");

    printf("============================================================\n\n");
}

/* Send a command and display its response. */
static int execute_command(
    int sockfd,
    const char *command,
    int multiline,
    char *first_response,
    size_t response_size
) {
    char outgoing[BUFFER_SIZE];

    int written = snprintf(
        outgoing, sizeof(outgoing), "%s\n", command
    );

    if (written < 0 || (size_t)written >= sizeof(outgoing)) {
        printf("Command is too long.\n");
        return -1;
    }

    if (send_all(sockfd, outgoing, (size_t)written) < 0) {
        printf("Failed to send command.\n");
        return -1;
    }

    char response[BUFFER_SIZE];

    int result = recv_line(sockfd, response, sizeof(response));

    if (result == -2) {
        printf("Error: Server response exceeded maximum buffer size.\n");
        return -1;
    }

    if (result <= 0) {
        printf("Server disconnected.\n");
        return -1;
    }

    /* Copy first response back to caller (needed by login_user, etc.). */
    if (first_response && response_size > 0) {
        snprintf(first_response, response_size, "%s", response);
    }

    /* Format individual message reading. */
    if (strncmp(command, "READ", 4) == 0) {
        if (strncmp(response, "OK|MESSAGE|", 11) == 0) {
            display_read_message(response);
        } else {
            printf("%s\n", response);
        }
        return 0;
    }

    /* Format INBOX and SENT responses. */
    if (multiline) {
        if (strncmp(command, "INBOX", 5) == 0) {
            if (strncmp(response, "OK|INBOX|", 9) == 0) {
                display_mailbox_table(sockfd, "INBOX", "Sender");
            } else {
                printf("%s\n", response);
            }
            return 0;
        }

        if (strncmp(command, "SENT", 4) == 0) {
            if (strncmp(response, "OK|SENT|", 8) == 0) {
                display_mailbox_table(sockfd, "SENT EMAILS", "Recipient");
            } else {
                printf("%s\n", response);
            }
            return 0;
        }

        /* Handle other multiline responses if needed. */
        while (1) {
            result = recv_line(sockfd, response, sizeof(response));
            
            if (result == -2) {
                printf("Error: Server response line too long.\n");
                continue;
            }

            if (result <= 0) {
                printf("Server disconnected.\n");
                return -1;
            }

            if (strcmp(response, "END") == 0 ||
                strncmp(response, "END|", 4) == 0) {
                break;
            }

            printf("%s\n", response);
        }
        return 0;
    }

    /* Print single-line server response (REGISTER, LOGIN, SEND, LOGOUT). */
    printf("%s\n", response);
    return 0;
}

/* Build a command with three encoded fields. */
static int build_three_field_command(
    const char *operation,
    const char *field1,
    const char *field2,
    const char *field3,
    char *command,
    size_t command_size
) {
    char a[INPUT_SIZE * 3];
    char b[INPUT_SIZE * 3];
    char c[INPUT_SIZE * 3];

    if (encode_field(field1, a, sizeof(a)) != 0 ||
        encode_field(field2, b, sizeof(b)) != 0 ||
        encode_field(field3, c, sizeof(c)) != 0) {
        return -1;
    }

    int written = snprintf(
        command, command_size,
        "%s|%s|%s|%s", operation, a, b, c
    );

    return written < 0 || (size_t)written >= command_size
        ? -1 : 0;
}

static int build_two_field_command(
    const char *operation,
    const char *field1,
    const char *field2,
    char *command,
    size_t command_size
) {
    char a[INPUT_SIZE * 3];
    char b[INPUT_SIZE * 3];

    if (encode_field(field1, a, sizeof(a)) != 0 ||
        encode_field(field2, b, sizeof(b)) != 0) {
        return -1;
    }

    int written = snprintf(
        command,
        command_size,
        "%s|%s|%s",
        operation,
        a,
        b
    );

    return written < 0 || (size_t)written >= command_size
        ? -1 : 0;
}


/* Registration menu action. */
static void register_user(int sockfd) {
    char username[INPUT_SIZE];
    char email[INPUT_SIZE];
    char password[INPUT_SIZE];
    char command[BUFFER_SIZE];
    char response[BUFFER_SIZE];

    if (read_input("Username: ", username, sizeof(username)) ||
        read_input("Email: ", email, sizeof(email)) ||
        read_input("Password: ", password, sizeof(password)))
        return;

    if (build_three_field_command(
            "REGISTER", username, email, password,
            command, sizeof(command)) != 0) {
        printf("Invalid or excessively long input.\n");
        return;
    }

    execute_command(sockfd, command, 0, response, sizeof(response));
}

/* Login menu action. */
static void login_user(int sockfd) {
    char username[INPUT_SIZE];
    char password[INPUT_SIZE];
    char command[BUFFER_SIZE];
    char response[BUFFER_SIZE];

    if (read_input("Username: ", username, sizeof(username)) ||
        read_input("Password: ", password, sizeof(password)))
        return;

    if (build_two_field_command(
            "LOGIN", username, password,
            command, sizeof(command)) != 0) {
        printf("Invalid input.\n");
        return;
    }

    if (execute_command(
            sockfd, command, 0, response, sizeof(response)) == 0) {
        logged_in =
            strncmp(response, "OK|LOGIN_SUCCESS|", 17) == 0;
    }
}

/* Compose and send an email. */
static void compose_email(int sockfd) {
    char recipient[INPUT_SIZE];
    char subject[INPUT_SIZE];
    char body[INPUT_SIZE];
    char command[BUFFER_SIZE];
    char response[BUFFER_SIZE];

    if (read_input("Recipient username(s): ", recipient,
                   sizeof(recipient)) ||
        read_input("Subject: ", subject, sizeof(subject)) ||
        read_input("Message: ", body, sizeof(body)))
        return;

    if (build_three_field_command(
            "SEND", recipient, subject, body,
            command, sizeof(command)) != 0) {
        printf("Input is too long.\n");
        return;
    }

    execute_command(sockfd, command, 0, response, sizeof(response));
}

/* Display inbox or sent mail. */
static void display_mailbox(int sockfd, const char *operation) {
    char response[BUFFER_SIZE];

    execute_command(
        sockfd, operation, 1, response, sizeof(response)
    );
}

/* Read a specific email. */
static void read_email(int sockfd) {
    char id[64];
    char command[128];
    char response[BUFFER_SIZE];

    if (read_input("Enter message ID: ", id, sizeof(id)))
        return;

    char *end = NULL;
    unsigned long long message_id = strtoull(id, &end, 10);

    if (end == id || *end != '\0' || message_id == 0) {
        printf("Invalid message ID.\n");
        return;
    }

    snprintf(command, sizeof(command), "READ|%llu", message_id);

    execute_command(sockfd, command, 0, response, sizeof(response));
}

/* Logout. */
static void logout_user(int sockfd) {
    char confirm[16];
    char response[BUFFER_SIZE];

    if (read_input("Are you sure you want to logout? (y/n): ",
                   confirm, sizeof(confirm)))
        return;

    if (strcmp(confirm, "y") != 0 &&
        strcmp(confirm, "Y") != 0 &&
        strcasecmp(confirm, "yes") != 0) {
        printf("Logout cancelled.\n");
        return;
    }

    if (execute_command(
            sockfd, "LOGOUT", 0, response, sizeof(response)) == 0) {
        if (strcmp(response, "OK|LOGGED_OUT") == 0) {
            logged_in = 0;
        }
    }
}


/* Main menu shown before login. */
static void authentication_menu(int sockfd) {
    char choice[32];

    while (!logged_in) {
        printf("\n========== EMAIL SERVICE ==========\n");
        printf("1. Register\n");
        printf("2. Login\n");
        printf("3. Exit\n");
        printf("===================================\n");

        if (read_input("Select option: ", choice, sizeof(choice)))
            return;

        switch (atoi(choice)) {
            case 1:
                register_user(sockfd);
                break;

            case 2:
                login_user(sockfd);
                break;

            case 3:
                return;

            default:
                printf("Invalid option.\n");
        }
    }
}

/* Main menu shown after login. */
static void email_menu(int sockfd) {
    char choice[32];

    while (logged_in) {
        printf("\n========== EMAIL DASHBOARD ==========\n");
        printf("1. Compose Email\n");
        printf("2. View Inbox\n");
        printf("3. Read Email\n");
        printf("4. View Sent Emails\n");
        printf("5. Logout\n");
        printf("6. Exit\n");
        printf("=====================================\n");

        if (read_input("Select option: ", choice, sizeof(choice)))
            return;

        switch (atoi(choice)) {
            case 1:
                compose_email(sockfd);
                break;

            case 2:
                display_mailbox(sockfd, "INBOX");
                break;

            case 3:
                read_email(sockfd);
                break;

            case 4:
                display_mailbox(sockfd, "SENT");
                break;

            case 5:
                logout_user(sockfd);
                break;

            case 6:
                return;

            default:
                printf("Invalid option.\n");
        }
    }
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);

    const char *server_ip = argc >= 2 ? argv[1] : "127.0.0.1";
    int port = argc >= 3 ? atoi(argv[2]) : 8000;

    if (port < 1 || port > 65535) {
        fprintf(stderr, "Invalid port.\n");
        return EXIT_FAILURE;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_addr = {0};

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid server IP.\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    if (connect(
            sockfd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)) < 0) {
        perror("connect");
        close(sockfd);
        return EXIT_FAILURE;
    }

    char welcome[BUFFER_SIZE];

    if (recv_line(sockfd, welcome, sizeof(welcome)) <= 0) {
        printf("Could not receive server welcome message.\n");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("\n%s\n", welcome);

    while (1) {
        if (!logged_in) {
            authentication_menu(sockfd);

            if (!logged_in)
                break;
        } else {
            email_menu(sockfd);

            if (logged_in) {
                /* Exit selected while still logged in. */
                char response[BUFFER_SIZE];
                execute_command(sockfd, "QUIT", 0, response, sizeof(response));
                logged_in = 0;
                break;
            }

        }
    }

    close(sockfd);

    printf("\nDisconnected from Email Server.\n");

    return EXIT_SUCCESS;
}
