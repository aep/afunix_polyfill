afunix-polyfill - AF_UNIX fixed in userspace
============================

afunix-polyfill is a complete linux IPC solution, compared to dbus it is:

- just a socket
- serverless
- data format agnostic
- dependency-free
- using unix permissions instead of custom policies
- theoretically posix compliant
- bullshit and NIH free

The backstory originates from ubus (aep-ubus, not openwrt-ubus),
which supports aproximatly the same messaging directives as dbus without a server.

But when you think really hard,  
almost every IPC problem people have in linux can be reduced to:

- send message to a group identified by name
- optionally respond to that message

which could easily be implemented with DGRAM on AF_UNIX plus SO_REUSEPORT and maybe SO_BROADCAST.  
but none of these things work in linux.  
So here is a 'polyfill' for that, which emulates it, until it gets fixed in linux  
(probably never, considered that IPC is a political minefield)


Use Case Examples in plain posix
-------------------------------------------

here's some pseudo code of how to implement any of the dbus
functionality in just plain posix.  
At least you could, if linux didn't suck.

1. "remote procedure call returning void"

```C
//server
int sock = socket(AF_UNIX, SOCK_DGRAM, 0);
bind(sock, "/var/bus/myservice/dostuff");
for(;;) { char buf[]; recv(socket, buf); printf("let's do stuff"); }

//client
int sock = socket(AF_UNIX, SOCK_DGRAM, 0);
connect(sock, "/var/bus/myservice/dostuff");
send(sock, "do it!");
```

2. "publish/subscribe" or "notification"

```C
//server
int sock = socket(AF_UNIX, SOCK_DGRAM, 0);
bind(sock, "/var/bus/myservice/event");
sendto(sock, "hey everyone, things happened!", BROADCAST);

//client
int sock = socket(AF_UNIX, SOCK_DGRAM, 0);
connect(sock, "/var/bus/myservice/event");
recv(socket, buf);
```

3. "remote procedure call returning string"

```C
//server
int sock = socket(AF_UNIX, SOCK_DGRAM, 0);
bind(sock, "/var/bus/myservice/dostuff");
for(;;) {
    char buf[];
    address sender;
    recv_from(socket, buf, &sender);
    send_to(socket, "ok i did it!", sender);

//client
int sock = socket(AF_UNIX, SOCK_DGRAM, 0);
connect(sock, "/var/bus/myservice/dostuff");
send(sock, "do it!");
recv(socket, buf);
```

And poof, the whole thing is solved. The best part, this is already how AF_INET behaves!  
All it takes is copying the exact behaviour over to AF_UNIX.

But while we wait for hell to freeze over, let's hop over to using afunix_polyfill

Api Design
-----------

should be pretty self explaining,
since it just does exactly what the posix api is supposed to do.

```C
// creates all the polyfill backends and wraps them in a single fd,
// which behaves exactly like a socket
int fd = afunix_socket();

// creates the file, and listens on it
// valid options are:
//  - SO_REUSEPORT round robin messages between all active binds
//                 without that option, you can only have one bind
int ret = afunix_bind(int fd, const char *name, int options);

//receive a datagram
char     buf[128];
uint32_t address
afunix_recvfrom(fd, &buf, AFUNIX_MAX_PACKAGE_SIZE, 0, &address);

//respond
afunix_sendto(fd, &buf, AFUNIX_MAX_PACKAGE_SIZE, 0, address);

//broadcast
afunix_sendto(fd, &buf, AFUNIX_MAX_PACKAGE_SIZE, 0, 0);

//connects to a file
int ret = afunix_connect(int fd, const char *name, int options);

//send to connected service
send(fd, &buf, AFUNIX_MAX_PACKAGE_SIZE, 0);
write(fd, &buf, AFUNIX_MAX_PACKAGE_SIZE, 0);

//receive response
recv(fd, &buf, AFUNIX_MAX_PACKAGE_SIZE, 0);

//close and clean up the polyfill backend
afunix_close(fd);
```

Security
--------

Instead of handling a policy of "who may open which service" in some framework,
you can simply rely on file permissions.

```bash
chmod 600 /var/0chan/
mkdir /var/0chan/cupsd/
chown cups /var/0chan/cupsd/
```

exactly the same for "who may talk to this service"

```bash
chgrp cups /var/0chan/cupsd/something.cmd
```

Limitations and Gotchas
--------------

- using any of the standard posix functions instead of the prefixed afunix ones
  is undefined behaviour. there is no way to detect that either.
- two threads doing afunix_recv() on the same chan at the same time,
  is undefined behaviour, because passing the address is thread unaware.
- same for afunix_send()
- the whole thing isnt thread safe yet, but that can be done later.
  its more in proof of concept state
