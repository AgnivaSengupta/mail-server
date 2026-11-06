#include "auth.h"

#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#include <openssl/rand.h>
#include <openssl/evp.h>

#define USERS_FILE "data/users.dat"
#define SALT_SIZE 16
#define HASH_SIZE 32
#define ITERATIONS 210000

static pthread_mutex_t auth_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char HEX[] = "0123456789abcdef";

static void to_hex(
    const unsigned char *input,
    size_t length,
    char *output
) {
    for (size_t i = 0; i < length; i++) {
        output[i * 2] = HEX[input[i] >> 4];
        output[i * 2 + 1] = HEX[input[i] & 0x0F];
    }

    output[length * 2] = '\0';
}

static int from_hex(
    const char *input,
    unsigned char *output,
    size_t output_size
) {
    if (strlen(input) != output_size * 2)
        return -1;

    for (size_t i = 0; i < output_size; i++) {
        unsigned int value;

        if (sscanf(input + i * 2, "%2x", &value) != 1)
            return -1;

        output[i] = (unsigned char)value;
    }

    return 0;
}

/* Prevent delimiters and control characters in stored identifiers. */
static int valid_identifier(const char *value) {
    if (value == NULL || value[0] == '\0')
        return 0;

    if (strlen(value) >= 1024)
        return 0;

    for (size_t i = 0; value[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)value[i];

        if (ch == '|' || ch == '\n' || ch == '\r' ||
            ch < 32 || ch == 127) {
            return 0;
        }
    }

    return 1;
}

static int derive_hash(
    const char *password,
    const unsigned char *salt,
    unsigned char *hash
) {
    return PKCS5_PBKDF2_HMAC(
        password,
        (int)strlen(password),
        salt,
        SALT_SIZE,
        ITERATIONS,
        EVP_sha256(),
        HASH_SIZE,
        hash
    );
}

int auth_user_exists(const char *username) {
    if (!username || username[0] == '\0')
        return 0;

    pthread_mutex_lock(&auth_mutex);

    FILE *file = fopen("data/users.dat", "r");

    if (!file) {
        pthread_mutex_unlock(&auth_mutex);
        return 0;
    }

    char line[4096];
    int found = 0;

    while (fgets(line, sizeof(line), file)) {
        char *separator = strchr(line, '|');

        if (!separator)
            continue;

        *separator = '\0';

        if (strcmp(line, username) == 0) {
            found = 1;
            break;
        }
    }

    fclose(file);
    pthread_mutex_unlock(&auth_mutex);

    return found;
}

/*
 * Checks whether a username or email already exists.
 * Caller must hold auth_mutex.
 */
static int find_user(
    const char *username,
    const char *email,
    int check_email
) {
    FILE *file = fopen(USERS_FILE, "r");

    if (file == NULL)
        return 0;

    char *line = NULL;
    size_t capacity = 0;
    int found = 0;

    while (getline(&line, &capacity, file) != -1) {
        char *first = strchr(line, '|');

        if (first == NULL)
            continue;

        *first = '\0';

        char *second = strchr(first + 1, '|');

        if (second == NULL)
            continue;

        *second = '\0';

        if (strcmp(line, username) == 0 ||
            (check_email &&
             strcmp(first + 1, email) == 0)) {
            found = 1;
            break;
        }
    }

    free(line);
    fclose(file);

    return found;
}

AuthResult register_user(
    const char *username,
    const char *email,
    const char *password
) {
    if (!valid_identifier(username) ||
        !valid_identifier(email) ||
        password == NULL ||
        password[0] == '\0' ||
        strlen(password) >= 1024) {
        return AUTH_INVALID_INPUT;
    }

    pthread_mutex_lock(&auth_mutex);

    if (find_user(username, email, 0)) {
        pthread_mutex_unlock(&auth_mutex);
        return AUTH_USER_EXISTS;
    }

    if (find_user("", email, 1)) {
        pthread_mutex_unlock(&auth_mutex);
        return AUTH_EMAIL_EXISTS;
    }

    unsigned char salt[SALT_SIZE];
    unsigned char hash[HASH_SIZE];

    if (RAND_bytes(salt, sizeof(salt)) != 1 ||
        derive_hash(password, salt, hash) != 1) {
        pthread_mutex_unlock(&auth_mutex);
        return AUTH_STORAGE_ERROR;
    }

    char salt_hex[SALT_SIZE * 2 + 1];
    char hash_hex[HASH_SIZE * 2 + 1];

    to_hex(salt, SALT_SIZE, salt_hex);
    to_hex(hash, HASH_SIZE, hash_hex);

    FILE *file = fopen(USERS_FILE, "a");

    if (file == NULL) {
        pthread_mutex_unlock(&auth_mutex);
        return AUTH_STORAGE_ERROR;
    }

    int result = fprintf(
        file,
        "%s|%s|%s|%s\n",
        username,
        email,
        salt_hex,
        hash_hex
    );

    int flush_result = fflush(file);
    int close_result = fclose(file);

    pthread_mutex_unlock(&auth_mutex);

    if (result < 0 || flush_result != 0 || close_result != 0)
        return AUTH_STORAGE_ERROR;

    return AUTH_SUCCESS;
}

AuthResult authenticate_user(
    const char *username,
    const char *password
) {
    if (!valid_identifier(username) ||
        password == NULL ||
        password[0] == '\0' ||
        strlen(password) >= 1024) {
        return AUTH_INVALID_INPUT;
    }

    pthread_mutex_lock(&auth_mutex);

    FILE *file = fopen(USERS_FILE, "r");

    if (file == NULL) {
        pthread_mutex_unlock(&auth_mutex);
        return AUTH_STORAGE_ERROR;
    }

    char *line = NULL;
    size_t capacity = 0;
    AuthResult result = AUTH_INVALID_CREDENTIALS;

    while (getline(&line, &capacity, file) != -1) {
        char *saveptr = NULL;

        char *stored_username = strtok_r(line, "|", &saveptr);
        char *stored_email = strtok_r(NULL, "|", &saveptr);
        char *stored_salt = strtok_r(NULL, "|", &saveptr);
        char *stored_hash = strtok_r(NULL, "|\r\n", &saveptr);

        (void)stored_email;

        if (!stored_username || !stored_salt || !stored_hash)
            continue;

        if (strcmp(stored_username, username) != 0)
            continue;

        unsigned char salt[SALT_SIZE];
        unsigned char expected_hash[HASH_SIZE];
        unsigned char actual_hash[HASH_SIZE];

        if (from_hex(stored_salt, salt, SALT_SIZE) < 0 ||
            from_hex(stored_hash, expected_hash, HASH_SIZE) < 0 ||
            derive_hash(password, salt, actual_hash) != 1) {
            result = AUTH_STORAGE_ERROR;
            break;
        }

        if (CRYPTO_memcmp(
                expected_hash,
                actual_hash,
                HASH_SIZE
            ) == 0) {
            result = AUTH_SUCCESS;
        }

        break;
    }

    free(line);
    fclose(file);

    pthread_mutex_unlock(&auth_mutex);

    return result;
}
