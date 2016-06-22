BINS=unixbus

ALL: $(BINS)

clean:
	rm -f $(BINS)

unixbus: cmd.c
	$(CC) $(CFLAGS) $^ -o $@ -lpthread
