CFLAGS=-O2 -g -Wall -W
# CFLAGS=-O0 -g -Wall -W

TARGETS=examples/test
DEPS=sherry.c sherry.h sds.c sds.h sdsalloc.h

all: $(TARGETS)

$(TARGETS): %: %.c $(DEPS)
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -f $(TARGETS)
