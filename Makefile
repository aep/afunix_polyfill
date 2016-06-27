BINS=unixbus minimal-invoke.h

ALL: $(BINS)

clean:
	rm -f $(BINS)

unixbus: cmd.c
	$(CC) $(CFLAGS) $^ -o $@ -lpthread

minimal-invoke.h: minimal-invoke.h.in
	echo "/*unibus version 1 absolute minimal invoke */" > $@
	./minifier.py $^ >> $@
