ALL: server call broadcast listen

clean:
	rm -f server call broadcast listen

call: test.c
	$(CC) $^ -o $@ -lpthread -DTEST_CALL
server: test.c
	$(CC) $^ -o $@ -lpthread -DTEST_SERVER
broadcast: test.c
	$(CC) $^ -o $@ -lpthread -DTEST_BROADCAST
listen: test.c
	$(CC) $^ -o $@ -lpthread -DTEST_LISTEN
