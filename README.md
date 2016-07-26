[![Build Status](https://travis-ci.org/aep/afunix_polyfill.svg?branch=master)](https://travis-ci.org/aep/afunix_polyfill)
[![NoDependencies](http://aep.github.io/images/no-dependencies.svg)](#)

afunix-polyfill - AF_UNIX fixed in userspace
============================

afunix-polyfill is a complete linux IPC solution, compared to dbus it is:

- just a socket
- serverless
- single header file
- data format agnostic
- dependency-free
- using unix permissions instead of custom policies
- theoretically posix compliant
- bullshit and NIH free

A word of warning: this is an experiment or proof-of-concept or RFC. If you need something usable, check back 2017.

The backstory originates from ubus (aep-ubus, not openwrt-ubus),
which supports aproximatly the same messaging directives as dbus without a server.

But when you think really hard,  
almost every IPC problem people have in linux can be reduced to:

- send message to a group identified by name
- optionally respond to that message

which could easily be implemented with DGRAM on AF_UNIX plus SO_REUSEPORT and maybe SO_BROADCAST.  
but none of these things work in linux.  
So here is a 'polyfill' for that, which emulates it, until it gets fixed in linux.    
As soon as it's fixed, you can seemlessly transition to the proper posix api, by just removing the afunix_ prefix.

Use Case Examples in plain posix
-------------------------------------------

here's some pseudo code of how to implement any of the dbus
functionality in just plain posix.  
At least you could, if linux didn't suck.

1) "remote procedure call returning void"

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

2) "publish/subscribe" or "notification"

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

3) "remote procedure call returning string"

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

Using
--------------

everything is in a single header file 'afunix_polyfill.h',
prefferably add a git submodule to your project and just include that header.

Limitations and Gotchas
--------------

- using any of the standard posix functions instead of the prefixed afunix ones
  is undefined behaviour. there is no way to detect that either.
- two threads doing afunix_recvfrom() on the same chan at the same time,
  is undefined behaviour, because passing the address is thread unaware.
- same for afunix_send()
- mixing afunix_recvfrom and recv on the same socket is undefined behaviour
- the whole thing isnt thread safe yet, but that can be done later.
  its more in proof of concept state
- unlike with real DGRAM, there is no way to transport a 0 bytes package.
  to prevent inconsistent behaviour, sending 0 bytes will close the channel


High level functionality
-----------------------

In addition to just fixing af_unix, this polyfill has optional high level apis,
as options to connect and bind:A

- AFUNIX_PATH_CONVENTION
  in addition to an absolute file path,
  take a shorter service name that is matched to an absolute path
  according to conventions described below
- AFUNIX_MAKE_PATH
  when using service names, create the real paths automatically

path conventions
----------------

there's a system bus and a session bus.
system is in /var/run/unixbus/<service>/<method>.seqpacket
service is in $XDG_RUNTIME_DIR/unixbus/<service>/<method>.seqpacket

short service names are identified as <system|session>:<service>:<method>,
for example:

```C
afunix_bind(fd, "service:xlock:lock", AFUNIX_PATH_CONVENTION | AFUNIX_MAKE_PATH);
```
will create $XDG_RUNTIME_DIR/unixbus/xlock/lock.seqpacket


commandline tool
----------------

The 'unixbus' commandline tool is in cmd.c and built with 'make' by default.
The main use cases are covered from a cient perspective:

1) "remote procedure call returning void"
```bash
$ unixbus invoke session:myservice:dostuff
$
```

2) "publish/subscribe" or "notification"
```bash
$ unixbus listen session:myservice:thatevent
hey everyone, things happened!
hey everyone, things happened again!
wow su much happen!
```

3) "remote procedure call returning string"
```bash
$ unixbus call session:myservice:do these things
i did these things!
$
```

There's also sever commands, although they're a bit excotic:

```bash
$ unixbus listen session:myservice:do
derp
merp
```
other shell:

```bash
$ unixbus call session:myservice:do derp
$ unixbus call session:myservice:do merp
```

xargs is pretty fun

```bash
$ echo 'echo $1 | tr a e' > /tmp/foo
$ unixbus xargs session:myservice:echo sh /tmp/foo
```

other shell:
```bash
$ unbixbus call session:myservice:echo amazing
emezing
$
```
