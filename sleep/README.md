# CPP sleep implementation

This folder contains a tentative implementation of a sleep function which balances thread sleeping and busy waiting.
The function attempts to choose a threshold between the sleeping and busy waiting automatically given a desired sleep
duration and a maximum acceptable error.

This is achieved by finding the closest threshold in a map which maps a sleep time to pairs of error-threshold.

```cpp
std::map<uint64_t, std::map<uint64_t, uint64_t>> interval_to_err_to_threshold = {
    {10, {{1020, 10.0}, {555410.0, 5.0}, {556980.0, 1.0},{562360.0, 3.0},{571610.0, 7.0}}}
  };
```

In the folder 'det-bypass-rust/performance' there is a python script 'perf.py' which can be used to output a correctly
formatted CPP map to be copy and pasted in the 'sleep.cpp'. If more granularity was needed it is sufficient to use this
script to produce more accurate results.