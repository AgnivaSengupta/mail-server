#include "mail.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define MESSAGE_FILE "data/messages.dat"

static pthread_mutex_t mail_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Encode delimiters and line breaks for file storage. */
static void encode_field(
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
                break;

            output[j++] = replacement[0];
            output[j++] = replacement[1];
            output[j++] = replacement[2];
        } else {
            if (j + 1 >= output_size)
                break;

            output[j++] = input[i];
        }
    }

    output[j] = '\0';
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Decode stored fields. */
static int decode_field(const char *input, char *output, size_t size) {
    size_t i = 0;
    size_t j = 0;

    while (input[i] != '\0') {
        if (input[i] == '%') {
            if (input[i + 1] == '\0' || input[i + 2] == '\0')
                return -1;

            int high = hex_value(input[i + 1]);
            int low = hex_value(input[i + 2]);

            if (high < 0 || low < 0)
                return -1;

            char decoded = (char)((high << 4) | low);

            if (decoded == '\0')
                return -1;

            if (j + 1 >= size)
                return -1;

            output[j++] = decoded;
            i += 3;
        } else {
            if (j + 1 >= size)
                return -1;

            output[j++] = input[i++];
        }
    }

    output[j] = '\0';
    return 0;
}

/* Split a record while preserving empty fields. */
static int split_record(char *line, char *fields[], int max_fields) {
    int count = 0;

    if (max_fields <= 0)
        return -1;

    fields[count++] = line;

    for (char *p = line; *p; p++) {
        if (*p == '|') {
            *p = '\0';

            if (count >= max_fields)
                return -1;

            fields[count++] = p + 1;
        }
    }

    return count;
}

static int parse_message(char *line, MailMessage *message) {
    char *fields[7];

    int count = split_record(line, fields, 7);

    if (count != 7)
        return -1;

    char *end = NULL;

    message->id = strtoull(fields[0], &end, 10);
    if (end == fields[0] || *end != '\0')
        return -1;

    message->timestamp = (time_t)strtoll(fields[1], &end, 10);
    if (end == fields[1] || *end != '\0')
        return -1;

    if (decode_field(fields[2], message->sender,
                     sizeof(message->sender)) != 0)
        return -1;

    if (decode_field(fields[3], message->recipient,
                     sizeof(message->recipient)) != 0)
        return -1;

    if (strcmp(fields[4], "0") == 0)
        message->is_read = 0;
    else if (strcmp(fields[4], "1") == 0)
        message->is_read = 1;
    else
        return -1;

    if (decode_field(fields[5], message->subject,
                     sizeof(message->subject)) != 0)
        return -1;

    if (decode_field(fields[6], message->body,
                     sizeof(message->body)) != 0)
        return -1;

    return 0;
}

static int write_message(FILE *file, const MailMessage *message) {
    char sender[MAIL_USERNAME_SIZE * 3];
    char recipient[MAIL_USERNAME_SIZE * 3];
    char subject[MAIL_SUBJECT_SIZE * 3];
    char body[MAIL_BODY_SIZE * 3];

    encode_field(message->sender, sender, sizeof(sender));
    encode_field(message->recipient, recipient, sizeof(recipient));
    encode_field(message->subject, subject, sizeof(subject));
    encode_field(message->body, body, sizeof(body));

    return fprintf(
        file,
        "%llu|%lld|%s|%s|%d|%s|%s\n",
        message->id,
        (long long)message->timestamp,
        sender,
        recipient,
        message->is_read,
        subject,
        body
    ) < 0 ? -1 : 0;
}

/* Scan the file and find the highest existing message ID. */
static unsigned long long get_next_id(FILE *file) {
    char line[MAIL_MAX_RECORD_SIZE];
    unsigned long long max_id = 0;

    rewind(file);

    while (fgets(line, sizeof(line), file)) {
        char *separator = strchr(line, '|');

        if (!separator)
            continue;

        *separator = '\0';

        char *end = NULL;
        unsigned long long id = strtoull(line, &end, 10);

        if (end != line && *end == '\0' && id > max_id)
            max_id = id;
    }

    return max_id + 1;
}

int mail_send(
    const char *sender,
    const char *recipient,
    const char *subject,
    const char *body,
    unsigned long long *message_id
) {
    if (!sender || !recipient || !subject || !body || !message_id)
        return -1;

    if (strlen(sender) >= MAIL_USERNAME_SIZE ||
        strlen(recipient) >= MAIL_USERNAME_SIZE ||
        strlen(subject) >= MAIL_SUBJECT_SIZE ||
        strlen(body) >= MAIL_BODY_SIZE)
        return -1;

    pthread_mutex_lock(&mail_mutex);

    FILE *file = fopen(MESSAGE_FILE, "a+");

    if (!file) {
        pthread_mutex_unlock(&mail_mutex);
        return -1;
    }

    MailMessage message = {0};

    message.id = get_next_id(file);
    message.timestamp = time(NULL);
    message.is_read = 0;

    snprintf(message.sender, sizeof(message.sender), "%s", sender);
    snprintf(message.recipient, sizeof(message.recipient), "%s", recipient);
    snprintf(message.subject, sizeof(message.subject), "%s", subject);
    snprintf(message.body, sizeof(message.body), "%s", body);

    int result = write_message(file, &message);

    if (result == 0 && fflush(file) != 0)
        result = -1;

    if (result == 0)
        *message_id = message.id;

    fclose(file);
    pthread_mutex_unlock(&mail_mutex);

    return result;
}

/* Shared implementation for inbox and sent-mail listing. */
static int get_messages(
    const char *username,
    MailMessage messages[],
    size_t capacity,
    size_t *count,
    int inbox
) {
    if (!username || !messages || !count)
        return -1;

    *count = 0;

    pthread_mutex_lock(&mail_mutex);

    FILE *file = fopen(MESSAGE_FILE, "r");

    if (!file) {
        pthread_mutex_unlock(&mail_mutex);
        return 0;
    }

    char line[MAIL_MAX_RECORD_SIZE];
    int result = 0;

    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\n")] = '\0';

        MailMessage message = {0};

        if (parse_message(line, &message) != 0)
            continue;

        int matches = inbox
            ? strcmp(message.recipient, username) == 0
            : strcmp(message.sender, username) == 0;

        if (!matches)
            continue;

        if (*count >= capacity) {
            result = 1; /* More messages exist than the supplied capacity. */
            break;
        }

        messages[(*count)++] = message;
    }

    fclose(file);
    pthread_mutex_unlock(&mail_mutex);

    return result;
}

int mail_get_inbox(
    const char *username,
    MailMessage messages[],
    size_t capacity,
    size_t *count
) {
    return get_messages(username, messages, capacity, count, 1);
}

int mail_get_sent(
    const char *username,
    MailMessage messages[],
    size_t capacity,
    size_t *count
) {
    return get_messages(username, messages, capacity, count, 0);
}

int mail_read_message(
    const char *username,
    unsigned long long message_id,
    MailMessage *message
) {
    if (!username || !message)
        return -1;

    pthread_mutex_lock(&mail_mutex);

    FILE *file = fopen(MESSAGE_FILE, "r");

    if (!file) {
        pthread_mutex_unlock(&mail_mutex);
        return -1;
    }

    FILE *temp = fopen("data/messages.tmp", "w");

    if (!temp) {
        fclose(file);
        pthread_mutex_unlock(&mail_mutex);
        return -1;
    }

    char line[MAIL_MAX_RECORD_SIZE];
    int found = 0;
    int result = 0;

    while (fgets(line, sizeof(line), file)) {
        char original[MAIL_MAX_RECORD_SIZE];
        snprintf(original, sizeof(original), "%s", line);

        line[strcspn(line, "\n")] = '\0';

        MailMessage current = {0};

        if (parse_message(line, &current) != 0) {
            fputs(original, temp);
            continue;
        }

        if (current.id == message_id &&
            (strcmp(current.recipient, username) == 0 ||
             strcmp(current.sender, username) == 0)) {

            if (strcmp(current.recipient, username) == 0) {
                current.is_read = 1;
            }
            *message = current;
            found = 1;

            if (write_message(temp, &current) != 0)
                result = -1;
        } else {
            fputs(original, temp);
        }
    }

    fclose(file);

    if (fclose(temp) != 0)
        result = -1;

    if (result == 0 && found) {
        if (rename("data/messages.tmp", MESSAGE_FILE) != 0)
            result = -1;
    } else {
        remove("data/messages.tmp");
    }

    pthread_mutex_unlock(&mail_mutex);

    if (result != 0)
        return -1;

    return found ? 0 : -1;
}