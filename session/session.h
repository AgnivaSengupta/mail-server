#ifndef SESSION_H
#define SESSION_H

#define SESSION_USERNAME_SIZE 1024
#define SESSION_SUCCESS 0
#define SESSION_ALREADY_LOGGED_IN -1
#define SESSION_USER_ACTIVE -2
#define SESSION_TABLE_FULL -3

int session_login(int socket_fd, const char *username);

int session_logout(int socket_fd);

int session_get_username(
    int socket_fd,
    char *username,
    unsigned long capacity
);

#endif
