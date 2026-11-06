#include "protocol.h"

#include <string.h>

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

/* Decode percent-encoded characters in a field. */
static int decode_field(
    const char *src,
    size_t length,
    char *dest,
    size_t capacity
) {
    size_t i = 0;
    size_t j = 0;

    while (i < length) {
        char ch = src[i++];

        if (ch == '%') {
            if (i + 1 >= length)
                return -1;

            int high = hex_value(src[i]);
            int low = hex_value(src[i + 1]);

            if (high < 0 || low < 0)
                return -1;

            ch = (char)((high << 4) | low);
            i += 2;

            /* NUL bytes are not valid in C strings. */
            if (ch == '\0')
                return -1;
        }

        if (j + 1 >= capacity)
            return -1;

        dest[j++] = ch;
    }

    dest[j] = '\0';
    return 0;
}

static CommandType identify_command(const char *name) {
    if (strcmp(name, "PING") == 0) return CMD_PING;
    if (strcmp(name, "HELP") == 0) return CMD_HELP;
    if (strcmp(name, "QUIT") == 0) return CMD_QUIT;
    if (strcmp(name, "REGISTER") == 0) return CMD_REGISTER;
    if (strcmp(name, "LOGIN") == 0) return CMD_LOGIN;
    if (strcmp(name, "LOGOUT") == 0) return CMD_LOGOUT;
    if (strcmp(name, "SEND") == 0) return CMD_SEND;
    if (strcmp(name, "INBOX") == 0) return CMD_INBOX;
    if (strcmp(name, "READ") == 0) return CMD_READ;
    if (strcmp(name, "SENT") == 0) return CMD_SENT;

    return CMD_UNKNOWN;
}

static int expected_fields(CommandType type) {
    switch (type) {
        case CMD_PING:
        case CMD_HELP:
        case CMD_QUIT:
        case CMD_LOGOUT:
        case CMD_INBOX:
        case CMD_SENT:
            return 0;

        case CMD_LOGIN:
            return 2;
        case CMD_READ:
            return 1; /* LOGIN: username, password; READ: message ID? */

        case CMD_REGISTER:
        case CMD_SEND:
            return 3;

        default:
            return -1;
    }
}

int parse_command(const char *line, ProtocolMessage *message) {
    if (line == NULL || message == NULL)
        return -1;

    memset(message, 0, sizeof(*message));

    const char *separator = strchr(line, '|');
    size_t command_length = separator
        ? (size_t)(separator - line)
        : strlen(line);

    if (command_length == 0 ||
        command_length >= PROTOCOL_COMMAND_SIZE)
        return -1;

    char command[PROTOCOL_COMMAND_SIZE];

    memcpy(command, line, command_length);
    command[command_length] = '\0';

    message->type = identify_command(command);

    if (message->type == CMD_UNKNOWN)
        return 0;

    const char *cursor = separator ? separator + 1 : NULL;

    if (cursor != NULL) {
        while (1) {
            if (message->field_count >= PROTOCOL_MAX_FIELDS)
                return -1;

            const char *next = strchr(cursor, '|');
            size_t field_length = next
                ? (size_t)(next - cursor)
                : strlen(cursor);

            char *destination =
                message->fields[message->field_count];

            if (decode_field(
                    cursor,
                    field_length,
                    destination,
                    PROTOCOL_FIELD_SIZE
                ) < 0) {
                return -1;
            }

            message->field_count++;

            if (next == NULL)
                break;

            cursor = next + 1;
        }
    }

    if ((int)message->field_count != expected_fields(message->type))
        return -1;

    return 0;
}
