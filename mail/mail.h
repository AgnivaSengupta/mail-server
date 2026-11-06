#ifndef MAIL_H
#define MAIL_H

#include <stddef.h>
#include <time.h>

#define MAIL_USERNAME_SIZE 256
#define MAIL_SUBJECT_SIZE 512
#define MAIL_BODY_SIZE 2048
#define MAIL_MAX_RECORD_SIZE 10000

typedef struct {
    unsigned long long id;
    char sender[MAIL_USERNAME_SIZE];
    char recipient[MAIL_USERNAME_SIZE];
    time_t timestamp;
    int is_read;
    char subject[MAIL_SUBJECT_SIZE];
    char body[MAIL_BODY_SIZE];
} MailMessage;

/* Store a new message. */
int mail_send(
    const char *sender,
    const char *recipient,
    const char *subject,
    const char *body,
    unsigned long long *message_id
);

/* Retrieve messages addressed to a user. */
int mail_get_inbox(
    const char *username,
    MailMessage messages[],
    size_t capacity,
    size_t *count
);

/* Retrieve messages sent by a user. */
int mail_get_sent(
    const char *username,
    MailMessage messages[],
    size_t capacity,
    size_t *count
);

/* Retrieve a message and mark it as read. */
int mail_read_message(
    const char *username,
    unsigned long long message_id,
    MailMessage *message
);

#endif