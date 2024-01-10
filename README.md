# det-bypass
Research on temporal determinism (stability) of current OS bypass methods


## Evaluation framework
Tools:
- RDMA
- DPDK
- eBPF XDP

timeframes:
- packet numbers: 10k,1mil, more
- absolute time: up to days

how to test: 
- Endhost settings: {no isol, isol}, {intervals}, {interrupts shielding} {memory
/paging}
- network settings: p2p, scale up

what to test: 
- end-to-end latency: latency_buckets, update_max, packet_dump
    - metrics: max, avg, 99.x percentile  
- jitter locality: TX stack, RX stack, network