# RDMA pingpongs

This directory contains pingpong programs based on RDMA.
The [rdma-core](https://github.com/linux-rdma/rdma-core/tree/master/libibverbs/examples) examples were used as a
guideline to develop these programs, which were deeply modified to integrate with the existing codebase.
Credits to the original authors for the original code.

## Pingpong design

The original pingpong programs in `rdma-core` follow a synchronous model, where the client sends a message and waits for
the server to respond before starting the next pingpong round.

However, in order to have a fair and truthful comparison between different technologies, we prefer having an
asynchronous model: the client has a thread that keeps sending packets at a fixed interval.

## Available programs

- [rc_pingpong](rc_pingpong.c): A RDMA pingpong using the Reliable Connection (RC) transport.

  A Reliable Connection (RC) is a connection-oriented transport that provides reliable, in-order delivery of messages.
  This transport type is very similar to TCP in the TCP/IP stack.

  Using RC transport to do the pingpong experiment in the asynchronous way (i.e., the client sends packets at a fixed
  interval), is pretty tricky: the client must wait for the completion of the previous message before sending another
  one (because of the definition of the reliable connection). So, if the ACK for each message is waited, then the send
  interval will not be the wanted one; if the ACK is not waited, then the pingpong becomes unpredictable, hoping that
  the ACK is received within the time interval. For this reason, RC is not the best transport to use in our case.

- [ud_pingpong](ud_pingpong.c): A RDMA pingpong using the Unreliable Datagram (UD) transport.

  An Unreliable Datagram (UD) is a connectionless transport that provides unreliable, unordered delivery of messages.
  This transport type is very similar to UDP in the TCP/IP stack.

  The UD transport is the best transport to use in our case, since it is connectionless and provides unreliable,
  unordered
  delivery of messages. This means that the client can send packets at a fixed interval without waiting for the
  completion of the previous message.

  It also represents the most fair comparison with other technologies which are based on UDP.

### Unavailable programs

The following transport types exist and are available in the original repository, but they were not adapted (yet) to
our use case:

- `uc_pingpong`: A RDMA pingpong using the Unreliable Connection (UC) transport.

  An Unreliable Connection (UC) is a connection-oriented transport that provides unreliable, unordered delivery of
  messages.
  It is basically a middle ground between RC and UD; a connection is established between QPs, but the delivery of
  messages is unreliable.
- `xrc_pingpong`: A RDMA pingpong using the Extended Reliable Connection (XRC) transport.
- `srq_pingpong`: A RDMA pingpong using the Shared Receive Queue (SRQ) transport.

## Build

The build process follows the same steps as for any CMake project, with a variable IS_SERVER to indicate if the program
is the server or the client.

```bash
cd build
cmake -D __SERVER__=<0/1> ..
make
```

There is the option to build the programs using a `DEBUG` mode, which has extra logging and debugging information:

```bash
cd build
cmake -D DEBUG=1 -D __SERVER__=<0/1> ..
make
```

## Run

The programs require at least two nodes to run, one acting as the server and the other as the client.

The following example shows how to run the `ud_pingpong` experiment.

First, the server must be started, which will wait a connection from a client:

```bash
./ud_pingpong -d <ib device name> -g <port gid index> -p <pingpong rounds>
# Example:
# ./ud_pingpong -d rocep65s0f0 -g 0 -p 1000
```

Then, the client must be started, which will connect to the server and start the pingpong:

```bash
./ud_pingpong -d <ib device name> -g <port gid index> -p <pingpong rounds> -i <send interval> -s <server IP>
# Example:
# ./ud_pingpong -d rocep65s0f0 -g 0 -p 1000 -i 1000000 -s 10.10.1.2
```

Note that the CLI interface was adapted to the other programs in the project and is different from the one in the
`rdma-core` examples.

## Results

By default, the results of the experiments are saved in a file named after the transport type (e.g. `ud_pingpong`
creates a file `ud.dat`).
In the code, the "persistence agent" can be modified to print results to stdout, to a different file, or to store
different information about the results.