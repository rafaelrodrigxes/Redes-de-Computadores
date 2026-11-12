CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -pthread

PROGS   = chat_server chat_client

.PHONY: all clean

all: $(PROGS)

chat_server: chat_server.c
	$(CC) $(CFLAGS) chat_server.c -o chat_server

chat_client: chat_client.c
	$(CC) $(CFLAGS) chat_client.c -o chat_client

clean:
	rm -f $(PROGS)
