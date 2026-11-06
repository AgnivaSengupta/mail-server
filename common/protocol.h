
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>

#define PROTOCOL_MAX_FIELDS 3
#define PROTOCOL_FIELD_SIZE 1024
#define PROTOCOL_COMMAND_SIZE 32

typedef enum {
    CMD_UNKNOWN,
    CMD_PING,
    CMD_HELP,
    CMD_QUIT,
    CMD_REGISTER,
    CMD_LOGIN,
    CMD_LOGOUT,
    CMD_SEND,
    CMD_INBOX,
    CMD_READ,
    CMD_SENT
} CommandType;

typedef struct {
    CommandType type;
    size_t field_count;
    char fields[PROTOCOL_MAX_FIELDS][PROTOCOL_FIELD_SIZE];
} ProtocolMessage;

/*
 * Parses a received protocol line.
 * Returns 0 on success, -1 on malformed input.
 */
int parse_command(const char *line, ProtocolMessage *message);

#endif
