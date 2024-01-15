# det-bypass
Research on temporal determinism (stability) of current OS bypass methods.

## Objective
We want to evaluate latency and jitter in datacenter communication, using different
kernel bypass methods. Moreover, we want to determine the system factors impacting
jitter (determinism/stabililty). We focus particularly on periodic communication. 

## Methodology
Tools:
- no bypass
- RDMA
- DPDK
- eBPF XDP (to app: map polling, socket)

Timeframes:
- packet numbers: 10k,1mil, more
- absolute time: up to days

Benchmark variables: 
- Endhost settings: {no isol, isol}, {intervals}, {interrupts shielding} {memory
/paging}
- network settings: p2p, scale up

Evaluation methods: 
- single/round trip latency: latency_buckets, update_max, packet_dump
    - metrics: max, avg, 99.x percentile  
- jitter locality: TX stack, RX stack, network
- one way jitter: send and recv difference in periodic protocols
