CFLAGS=-O2 -Wall -W

all: test

test: test.c sherry.c
	$(CC) -o test test.c sherry.c $(CFLAGS)

clean:
	rm -f test
