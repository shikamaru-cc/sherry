# CFLAGS=-O2 -g -Wall -W
CFLAGS=-O0 -g -Wall -W
LIBS=-luv

TARGETS=examples/test examples/http-server
DEPS=sherry.c sherry.h sds.c sds.h sdsalloc.h

all: $(TARGETS)

$(TARGETS): %: %.c $(DEPS)
	$(CC) $(CFLAGS) $(LIBS) $^ -o $@

clean:
	rm -f $(TARGETS)
