CC = gcc
CFLAGS = -Wall -Wextra -g -pthread
LDLIBS = -lcrypto

SERVER_SRC = \
    server/server.c \
    common/protocol.c \
    auth/auth.c \
    session/session.c \
    mail/mail.c

CLIENT_SRC = client/client.c

all: email_server email_client

email_server: $(SERVER_SRC)
	$(CC) $(CFLAGS) $(SERVER_SRC) -o email_server $(LDLIBS)

email_client: $(CLIENT_SRC)
	$(CC) $(CFLAGS) $(CLIENT_SRC) -o email_client

clean:
	rm -f email_server email_client

.PHONY: all clean