#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../auth/auth.h"
#include "../common/protocol.h"
#include "../mail/mail.h"
#include "../session/session.h"

#define DEFAULT_PORT 8000
#define BACKLOG 10
#define BUFFER_SIZE 4096
#define MAX_LIST_MESSAGES 100

typedef struct {
    int socket_fd;
} ClientArg;

int send_all(int fd, const char *buffer, size_t length) {
    size_t total = 0;

    while (total < length) {
        ssize_t sent = send(fd, buffer + total, length - total, MSG_NOSIGNAL);

        if (sent < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (sent == 0)
            return -1;

        total += (size_t)sent;
    }

    return 0;
}

static int send_response(int fd, const char *fmt, ...) {
    char buffer[BUFFER_SIZE * 2];
    va_list args;

    va_start(args, fmt);
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (len < 0 || (size_t)len >= sizeof(buffer))
        return -1;

    return send_all(fd, buffer, (size_t)len);
}

/* Percent-encode outbound protocol fields so '|', '\n', '\r', '%' are safe. */
static void encode_protocol_field(
    const char *input,
    char *output,
    size_t output_size
) {
    size_t j = 0;

    for (size_t i = 0; input[i] != '\0'; i++) {
        const char *rep = NULL;

        switch (input[i]) {
            case '%': rep = "%25"; break;
            case '|': rep = "%7C"; break;
            case '\n': rep = "%0A"; break;
            case '\r': rep = "%0D"; break;
        }

        if (rep) {
            if (j + 3 >= output_size)
                break;
            output[j++] = rep[0];
            output[j++] = rep[1];
            output[j++] = rep[2];
        } else {
            if (j + 1 >= output_size)
                break;
            output[j++] = input[i];
        }
    }

    output[j] = '\0';
}

ssize_t recv_line(int fd, char *buffer, size_t capacity) {
    size_t used = 0;

    if (capacity < 2)
        return -1;

    while (used < capacity - 1) {
        char ch;
        ssize_t received = recv(fd, &ch, 1, 0);

        if (received < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (received == 0)
            return used == 0 ? 0 : (ssize_t)used;

        if (ch == '\n')
            break;

        if (ch != '\r')
            buffer[used++] = ch;
    }

    if (used == capacity - 1)
        return -2; /* Command too long */

    buffer[used] = '\0';
    return (ssize_t)used;
}

static void handle_register(int client_fd, const ProtocolMessage *msg) {
    AuthResult res = register_user(
        msg->fields[0],
        msg->fields[1],
        msg->fields[2]
    );

    switch (res) {
        case AUTH_SUCCESS:
            send_response(client_fd, "OK|REGISTERED\n");
            break;
        case AUTH_USER_EXISTS:
            send_response(client_fd, "ERROR|USER_EXISTS\n");
            break;
        case AUTH_EMAIL_EXISTS:
            send_response(client_fd, "ERROR|EMAIL_EXISTS\n");
            break;
        case AUTH_INVALID_INPUT:
            send_response(client_fd, "ERROR|INVALID_INPUT\n");
            break;
        default:
            send_response(client_fd, "ERROR|STORAGE_ERROR\n");
            break;
    }
}

static void handle_login(int client_fd, const ProtocolMessage *msg) {
    const char *username = msg->fields[0];
    const char *password = msg->fields[1];

    char current_user[SESSION_USERNAME_SIZE];
    if (session_get_username(client_fd, current_user, sizeof(current_user)) == 0) {
        send_response(client_fd, "ERROR|ALREADY_LOGGED_IN\n");
        return;
    }

    AuthResult auth = authenticate_user(username, password);
    if (auth == AUTH_INVALID_INPUT) {
        send_response(client_fd, "ERROR|INVALID_INPUT\n");
        return;
    }
    if (auth == AUTH_STORAGE_ERROR) {
        send_response(client_fd, "ERROR|STORAGE_ERROR\n");
        return;
    }
    if (auth != AUTH_SUCCESS) {
        send_response(client_fd, "ERROR|INVALID_CREDENTIALS\n");
        return;
    }

    int sres = session_login(client_fd, username);
    if (sres == SESSION_ALREADY_LOGGED_IN) {
        send_response(client_fd, "ERROR|ALREADY_LOGGED_IN\n");
    } else if (sres == SESSION_USER_ACTIVE) {
        send_response(client_fd, "ERROR|USER_ALREADY_ACTIVE\n");
    } else if (sres == SESSION_TABLE_FULL) {
        send_response(client_fd, "ERROR|SERVER_BUSY\n");
    } else if (sres != SESSION_SUCCESS) {
        send_response(client_fd, "ERROR|LOGIN_FAILED\n");
    } else {
        send_response(client_fd, "OK|LOGIN_SUCCESS|%s\n", username);
    }
}

static void handle_logout(int client_fd) {
    if (session_logout(client_fd) == 0) {
        send_response(client_fd, "OK|LOGGED_OUT\n");
    } else {
        send_response(client_fd, "ERROR|NOT_AUTHENTICATED\n");
    }
}

static void handle_send(int client_fd, const ProtocolMessage *msg) {
    char sender[SESSION_USERNAME_SIZE];
    if (session_get_username(client_fd, sender, sizeof(sender)) != 0) {
        send_response(client_fd, "ERROR|NOT_AUTHENTICATED\n");
        return;
    }

    const char *recipient = msg->fields[0];
    const char *subject = msg->fields[1];
    const char *body = msg->fields[2];

    if (!auth_user_exists(recipient)) {
        send_response(client_fd, "ERROR|RECIPIENT_NOT_FOUND\n");
        return;
    }

    unsigned long long message_id = 0;
    if (mail_send(sender, recipient, subject, body, &message_id) != 0) {
        send_response(client_fd, "ERROR|SEND_FAILED\n");
        return;
    }

    send_response(client_fd, "OK|SENT|%llu\n", message_id);
}

static void handle_list(int client_fd, int is_inbox) {
    char username[SESSION_USERNAME_SIZE];
    if (session_get_username(client_fd, username, sizeof(username)) != 0) {
        send_response(client_fd, "ERROR|NOT_AUTHENTICATED\n");
        return;
    }

    MailMessage *messages = calloc(MAX_LIST_MESSAGES, sizeof(MailMessage));
    if (!messages) {
        send_response(client_fd, "ERROR|SERVER_ERROR\n");
        return;
    }

    size_t count = 0;
    int res = is_inbox
        ? mail_get_inbox(username, messages, MAX_LIST_MESSAGES, &count)
        : mail_get_sent(username, messages, MAX_LIST_MESSAGES, &count);

    if (res < 0) {
        free(messages);
        send_response(client_fd, "ERROR|STORAGE_ERROR\n");
        return;
    }

    send_response(
        client_fd,
        "OK|%s|%zu\n",
        is_inbox ? "INBOX" : "SENT",
        count
    );

    for (size_t i = 0; i < count; i++) {
        char encoded_subject[MAIL_SUBJECT_SIZE * 3];
        encode_protocol_field(
            messages[i].subject,
            encoded_subject,
            sizeof(encoded_subject)
        );

        send_response(
            client_fd,
            "MESSAGE|%llu|%lld|%s|%d|%s\n",
            messages[i].id,
            (long long)messages[i].timestamp,
            is_inbox ? messages[i].sender : messages[i].recipient,
            messages[i].is_read,
            encoded_subject
        );
    }

    send_response(client_fd, "END\n");
    free(messages);
}

static void handle_read(int client_fd, const ProtocolMessage *msg) {
    char username[SESSION_USERNAME_SIZE];
    if (session_get_username(client_fd, username, sizeof(username)) != 0) {
        send_response(client_fd, "ERROR|NOT_AUTHENTICATED\n");
        return;
    }

    char *end = NULL;
    unsigned long long id = strtoull(msg->fields[0], &end, 10);
    if (end == msg->fields[0] || *end != '\0' || id == 0) {
        send_response(client_fd, "ERROR|INVALID_MESSAGE_ID\n");
        return;
    }

    MailMessage message = {0};
    if (mail_read_message(username, id, &message) != 0) {
        send_response(client_fd, "ERROR|MESSAGE_NOT_FOUND\n");
        return;
    }

    char encoded_subject[MAIL_SUBJECT_SIZE * 3];
    char encoded_body[MAIL_BODY_SIZE * 3];

    encode_protocol_field(message.subject, encoded_subject, sizeof(encoded_subject));
    encode_protocol_field(message.body, encoded_body, sizeof(encoded_body));

    send_response(
        client_fd,
        "OK|MESSAGE|%llu|%lld|%s|%s|%s|%s\n",
        message.id,
        (long long)message.timestamp,
        message.sender,
        message.recipient,
        encoded_subject,
        encoded_body
    );
}

void *handle_client(void *arg) {
    ClientArg *args = (ClientArg *)arg;
    int client_fd = args->socket_fd;

    free(args);
    pthread_detach(pthread_self());

    printf("[SERVER] Client connected (fd=%d)\n", client_fd);

    if (send_response(client_fd, "WELCOME|Connected to Email Server\n") < 0)
        goto cleanup;

    char buffer[BUFFER_SIZE];

    while (1) {
        ssize_t bytes = recv_line(client_fd, buffer, sizeof(buffer));

        if (bytes == 0) {
            printf("[SERVER] Client disconnected (fd=%d)\n", client_fd);
            break;
        }

        if (bytes == -2) {
            send_response(client_fd, "ERROR|COMMAND_TOO_LONG\n");
            break;
        }

        if (bytes < 0) {
            perror("[SERVER] recv");
            break;
        }

        ProtocolMessage msg;
        if (parse_command(buffer, &msg) < 0) {
            send_response(client_fd, "ERROR|INVALID_COMMAND_FORMAT\n");
            continue;
        }

        switch (msg.type) {
            case CMD_PING:
                send_response(client_fd, "PONG\n");
                break;

            case CMD_HELP:
                send_response(
                    client_fd,
                    "COMMANDS|PING,HELP,REGISTER,LOGIN,LOGOUT,SEND,INBOX,READ,SENT,QUIT\n"
                );
                break;

            case CMD_REGISTER:
                handle_register(client_fd, &msg);
                break;

            case CMD_LOGIN:
                handle_login(client_fd, &msg);
                break;

            case CMD_LOGOUT:
                handle_logout(client_fd);
                break;

            case CMD_SEND:
                handle_send(client_fd, &msg);
                break;

            case CMD_INBOX:
                handle_list(client_fd, 1);
                break;

            case CMD_SENT:
                handle_list(client_fd, 0);
                break;

            case CMD_READ:
                handle_read(client_fd, &msg);
                break;

            case CMD_QUIT:
                send_response(client_fd, "BYE\n");
                goto cleanup;

            case CMD_UNKNOWN:
            default:
                send_response(client_fd, "ERROR|UNKNOWN_COMMAND\n");
                break;
        }
    }

cleanup:
    session_logout(client_fd);
    close(client_fd);
    printf("[SERVER] Connection closed (fd=%d)\n", client_fd);
    return NULL;
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;

    if (argc > 1) {
        char *end;
        long parsed = strtol(argv[1], &end, 10);

        if (*argv[1] == '\0' || *end != '\0' ||
            parsed < 1 || parsed > 65535) {
            fprintf(stderr, "Invalid port number\n");
            return EXIT_FAILURE;
        }

        port = (int)parsed;
    }

    mkdir("data", 0755);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((unsigned short)port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("=================================\n");
    printf("       EMAIL SERVER STARTED      \n");
    printf("=================================\n");
    printf("Listening on port: %d\n", port);
    printf("Waiting for clients...\n\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(
            server_fd,
            (struct sockaddr *)&client_addr,
            &addr_len
        );

        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(
            AF_INET,
            &client_addr.sin_addr,
            client_ip,
            sizeof(client_ip)
        );

        printf("[SERVER] New connection from %s:%d\n",
               client_ip,
               ntohs(client_addr.sin_port));

        ClientArg *args = malloc(sizeof(ClientArg));
        if (args == NULL) {
            perror("malloc");
            close(client_fd);
            continue;
        }

        args->socket_fd = client_fd;

        pthread_t thread;
        int result = pthread_create(&thread, NULL, handle_client, args);

        if (result != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(result));
            free(args);
            close(client_fd);
            continue;
        }
    }

    close(server_fd);
    return EXIT_SUCCESS;
}