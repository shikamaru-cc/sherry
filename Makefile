# CFLAGS=-O3 -Wall -W
CFLAGS=-O2 -g -Wall -W -Ilibuv/include

LIBS=libuv/.libs/libuv.a

TARGETS=examples/test examples/http-server

DEPS=sherry.c sds.c

all: $(TARGETS)

$(TARGETS): %: %.c $(DEPS)
	$(CC) -o $@ $^ $(LIBS) $(CFLAGS)

clean:
	rm -f $(TARGETS)
