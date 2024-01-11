# RDMA pingpongs

This directory contains a set of RDMA pingpong tests from
the [rdma-core](https://github.com/linux-rdma/rdma-core/tree/master/libibverbs/examples) repository. Credits to the original authors.

The original code was modified to allow storing the timestamp of each packet exchanged during the pingpong.

## Available programs

- [rc_pingpong](./rc_pingpong.c): A RDMA pingpong using the Reliable Connection (RC) transport.

  A Reliable Connection (RC) is a connection-oriented transport that provides reliable, in-order delivery of messages.
  This transport type is very similar to TCP in the TCP/IP stack.

- [uc_pingpong](./uc_pingpong.c): A RDMA pingpong using the Unreliable Connection (UC) transport.

  An Unreliable Connection (UC) is a connection-oriented transport that provides unreliable, unordered delivery of
  messages.
  It is basically a middle ground between RC and UD; a connection is established between QPs, but the delivery of
  messages is unreliable.

- [ud_pingpong](./ud_pingpong.c): A RDMA pingpong using the Unreliable Datagram (UD) transport.

  An Unreliable Datagram (UD) is a connectionless transport that provides unreliable, unordered delivery of messages.
  This transport type is very similar to UDP in the TCP/IP stack.

The following programs are available, since they were available in the original repository, but their functionalities
and characteristics are not well known:

- [xrc_pingpong](./xrc_pingpong.c): A RDMA pingpong using the Extended Reliable Connection (XRC) transport.
- [srq_pingpong](./srq_pingpong.c): A RDMA pingpong using the Shared Receive Queue (SRQ) transport.

## Build

The build process is the same as for any CMake project:

```bash
mkdir build
cd build
cmake ..
make
```

This will build all the pingpong programs available.

## Run

The programs require at least two nodes to run, one acting as the server and the other as the client.

The server will wait for the client to connect, and then the pingpong will start.

The CLI interface of the programs is the same as the original ones, so you can use the `--help` flag to see the
available options.

A basic example of running the pingpong is:

```bash
# On the server node
./rc_pingpong -d <device> -g <port_gid_index>

# On the client node
./rc_pingpong -d <device> -g <port_gid_index> <server_ip>
```

The number of packets sent can be specified with the `-n` flag.

## Results

The programs are customized to store the timestamp of each message sent and received.

The raw timestamps and the calculated latencies are stored in two .txt files under the `results` directory on each 
client node.

Note that for each packet, the first and last timestamps are computed on one node, while the second and third
timestamps are computed on the other node; for this reason, the values are not directly comparable, as the clocks
of the two computers may not be synchronized.
