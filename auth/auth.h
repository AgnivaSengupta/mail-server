#ifndef AUTH_H
#define AUTH_H

int auth_user_exists(const char *username);

typedef enum {
    AUTH_SUCCESS = 0,
    AUTH_USER_EXISTS,
    AUTH_EMAIL_EXISTS,
    AUTH_INVALID_CREDENTIALS,
    AUTH_STORAGE_ERROR,
    AUTH_INVALID_INPUT
} AuthResult;

AuthResult register_user(
    const char *username,
    const char *email,
    const char *password
);

AuthResult authenticate_user(
    const char *username,
    const char *password
);

#endif
