#include "session.h"

#include <string.h>
#include <pthread.h>

#define MAX_SESSIONS 128

typedef struct {
    int active;
    int socket_fd;
    char username[SESSION_USERNAME_SIZE];
} Session;

static Session sessions[MAX_SESSIONS];

static pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;

int session_login(int socket_fd, const char *username) {
    if (username == NULL || username[0] == '\0' ||
        strlen(username) >= SESSION_USERNAME_SIZE) {
        return -1;
    }

    pthread_mutex_lock(&session_mutex);

    int free_slot = -1;

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active &&
            sessions[i].socket_fd == socket_fd) {
            pthread_mutex_unlock(&session_mutex);
            return SESSION_ALREADY_LOGGED_IN;  // Connection already logged in
        }

        if (sessions[i].active &&
            strcmp(sessions[i].username, username) == 0) {
            pthread_mutex_unlock(&session_mutex);
            return SESSION_USER_ACTIVE;  // User already logged in elsewhere
        }

        if (!sessions[i].active && free_slot == -1)
            free_slot = i;
    }

    if (free_slot == -1) {
        pthread_mutex_unlock(&session_mutex);
        return SESSION_TABLE_FULL;  // Session table full
    }

    sessions[free_slot].active = 1;
    sessions[free_slot].socket_fd = socket_fd;

    strcpy(sessions[free_slot].username, username);

    pthread_mutex_unlock(&session_mutex);

    return 0;
}

int session_logout(int socket_fd) {
    pthread_mutex_lock(&session_mutex);

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active &&
            sessions[i].socket_fd == socket_fd) {
            sessions[i].active = 0;
            sessions[i].socket_fd = -1;
            sessions[i].username[0] = '\0';

            pthread_mutex_unlock(&session_mutex);
            return 0;
        }
    }

    pthread_mutex_unlock(&session_mutex);

    return -1;
}

int session_get_username(
    int socket_fd,
    char *username,
    unsigned long capacity
) {
    if (username == NULL || capacity == 0)
        return -1;

    pthread_mutex_lock(&session_mutex);

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active &&
            sessions[i].socket_fd == socket_fd) {
            size_t length = strlen(sessions[i].username);

            if (length + 1 > capacity) {
                pthread_mutex_unlock(&session_mutex);
                return -1;
            }

            strcpy(username, sessions[i].username);

            pthread_mutex_unlock(&session_mutex);
            return 0;
        }
    }

    pthread_mutex_unlock(&session_mutex);

    return -1;
}
