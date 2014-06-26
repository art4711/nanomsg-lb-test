# Test of the load balancing behavior of Nanomsg #

## The code and how to use it ##

### Build and run ###

    $ make
    cc -o nlt -O2 -Wall -Werror nlt.c -lrt -lnanomsg -lpthread -lanl
    $ ./nlt -s ipc://a &
    [2] 22734
    $ ./nlt -c ipc://a
    client req took 2.005276
    client req took 4.003425
    client req took 6.002435
    [...]

This works on Centos 6. Should probably work on most unix-like systems
with minimal tweaks.

### Server side ###

The server side sets up one socket - `ext_sock` that listens to
incoming messages from clients. Then there's an internal socket
`int_sock` that listens to the "inproc://hej" address. `ext_sock` and
`int_sock` are connected with an `nn_device`.

The server worker threads have a socket each that connect to
`int_sock` (or rather to the "inproc://hej" address).

## The problem ##

This is testing the load balancing behavior of the REQ/REP protocol of
nanomsg. Specifically how well it behaves when it comes to evenly
spreading out messages between servers depending on server load when
the requests have an unpredictable and highly variable cost. Then the
plan was to test and implement some kind of server side pushback
mechanism.

I intended to start up a few servers, make the client connect to them
and start up a firehose of requests with certain requests taking
orders of magnitude more time than the rest and see how well that
behaves.

### The stumbling block ###

I didn't even get as far as starting multiple servers or writing the
client code to talk to multiple servers. Even the first test versions
of the test code showed some unacceptably bad load balancing within
the `inproc:` protocol and only a very brutal and crude workaround
could lessen the problem, but not fix it.

The workaround is to simply reduce the receive buffer on the worker
threads to one byte. Somehow this makes the slow worker thread queue
process three requests (first one I understand it's the one it starts
processing immediately, second one I understand, it's the one that's
queued in the "exactly one message may be buffered in addition to the
data in the receive buffer", but where does the third come from? It
won't fit in the buffer, because the request is three bytes and the
receive buffer is set to 1. Is it queued in the send buffer?

### What's happening? ###

Each client just sends a request then the server worker threads are
set up so that one of them always takes a long time to process request.

In a perfect load balancing system the requests should be sent to the
server that is the least busy. Instead what's happening here is that
the round-robin algorithm sends all the requests evenly to all the
worker threads and then just sits there twiddling its thumbs before
it's time to process the replies and send them back to the requesters.

For a networked load balancer without any additional information this
behavior might make sense. For one that actually has perfect information
like "inproc:" this is crazy. We end up with 4 server threads processing
their requests in a few microseconds, then the last server thread takes
2 seconds each to process each request. 

### What did I miss? ###

Nanomsg documentation isn't very verbose, so it is quite likely that I
messed something up. Maybe what I'm doing is not how things should be
done even though 0mq documentation implied that this is the intended
way to use it.

## Background ##

### Why? ###

I'm evaluating nanomsg as a replacement protocol for two internal
services we run. We desperately need to modernize certain things in
how we talk to our services and as part of it I've decided to find
some ready made solution instead of rolling our own. Our own protocols
do what they need, are scalable and fast, but it's not really our core
business to polish network protocols.

So far nanomsg looks like almost what we need. We would use a
relatively simple REQ/REP model for both the services we run and can
use the internal load balancing with data center priorities (for
our users that have two or more data centers) even though it's
slightly less advanced than what we do today. So far so good. But.

### Clues out there on the net ###

I've read http://250bpm.com/blog:14 and it pretty much talks about
what load balancing we need, except that there's one thing that I
either don't understand, haven't found anywhere in the documentation
or isn't there and is just mentioned in the blog post as a side note
without explaining.

"...if a peer is dead, or it is busy at the moment, it's removed..."
"...after all the priority 1 peers are dead, disconnected or busy..."
"...unless all of them are dead, busy or disconnected..."

What does "busy" mean? I haven't found anywhere in the API
documentation any way to signal that a service is busy.

### Why it matters. ###

Both our services, but especially one, have annoying performance
profiles. One request can take anything between a few microseconds to
several minutes. The majority are in the lower end of the scale and
the servers use less than half the CPUs they have and everyone is
happy. The occasional expensive 1s, 20s, 120s requests mix up nicely
with the normal fire hose of cheap ones. But given enough random
events, we do end up in the unlikely situation where too many
expensive requests are being handled by the same server and we don't
have enough CPU to process things in real time and the requests queue
up. Our most heavily used servers can peak at 10k requests per second
and going from 20us average processing time to 100ms means that we go
from a few dozen clients waiting for a response to thousands or
worse. A waiting client uses a lot of memory, which slows request
processing even more which quickly propagates up the stack and leads
to choking frontend servers and the whole house of cards falls down.
This is not an imagined scenario. Logging shows that some of our users
hit this situation several times per day. Strictly speaking this isn't
necessarily just the requests that are expensive, but can also be caused
by the paging behavior of the machines they run on since we're processing
a lot of data mapped with `mmap` and the operating system can sometimes
decide that some random 

We can't handle this with client side timeouts because the client
rarely knows which requests will be CPU melters and which ones will be
handled by a simple precomputed response. So any reasonable client
timeout must be at least a few seconds (with manual tuning of the
requests that we know can take up to several minutes), which means
unreasonable amount of request queueing before we detect that a server
is busy.

The solution for it we have in our home-brew protocols is to always
reserve a high priority worker thread that handles requests when all
other worker threads are busy. It reads the request, throws it away
and responds "busy, go away" (almost literally). Then there's in the
clients that makes them X% less likely to send a request to a server
if the last response from it was "go away".  But even the simplest
strategy of "resend the request to any other randomly picked server"
works adequately. By spreading the load this way we reduce the
probability of having all CPUs busy at the same time from once an hour
to once a year which is good enough. Random is quite important here
because when doing this round-robin just one small hiccup on one
server means that the next server in the list gets hit with two fire
hoses of requests, overloads, returns "go away", the next server in
line gets all the firehoses and we end up with oscilating servers.
