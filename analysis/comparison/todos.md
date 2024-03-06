# Benchmarks
> Should we pick the best between XDP poll and XSK for extensive benchmarks or 
> keep both?

## NIC timestamp (after cluster delivery)
Objective: in depth analysis on where the latency spikes occur

## Different sending intervals
- how fast can we go?
- Are the periodic peaks changing according to the interval?

## Different isolation settings (after cluster delivery)
- How does isolation affect random and periodic peaks?

## Network and processing load injection (later)

## Scalability 
> consider limiting this to "best" approach only to avoid
> too many benchmarks
 
- 1 to N
- N to 1
- N to N


## Percentile graphs
Use the "latency buckets" approach to extract latency distributions over long runs.

Should we use RTT latency or relative?

- 24h.
- 48h?
- how long should we go? the longest the better but D:

Set buffer min / max and resolution accordingly. e.g. 0 to 10ms with 20k buckets,
consider differently sized buckets on rare occurrences like high peaks

Mind buffer size, use `uint_64` to avoid maxing out counters. 


## Long min/max testing (depends on above)
Depends on how similar the results are with percentile benchmarks. If we see dramatic
improvements (e.g. some effect related to storing the data), we should carry on
otherwise not.

What are the min and max latency of our approaches when running pingpong benchmarks for

- 24h
- 48h
- how long should we go? the longest the better but D: