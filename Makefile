# CFLAGS=-O2 -Wall -W
CFLAGS=-O0 -g -Wall -W

all: test sherry-chat

test: test.c sherry.c
	$(CC) -o test test.c sherry.c $(CFLAGS)

sherry-chat: sherry.c sherry-chat.c
	$(CC) -o sherry-chat sherry-chat.c sherry.c $(CFLAGS)

clean:
	rm -f test
	rm -f sherry-chat
