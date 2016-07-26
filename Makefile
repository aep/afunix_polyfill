BINS=unixbus minimal-invoke.h

ALL: $(BINS)

clean:
	rm -f $(BINS)

CFLAGS=-std=c99 -D_POSIX_C_SOURCE=1 -Wall

CFLAGS  += -g
LDFLAGS += -lpthread -g

unixbus: cmd.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test: test.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

minimal-invoke.h: minimal-invoke.h.in
	echo "/*unibus version 1 absolute minimal invoke */" > $@
	./minifier.py $^ >> $@
