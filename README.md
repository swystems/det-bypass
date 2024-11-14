# det-bypass
Research on temporal determinism (=low latency, low processing jitter) of the following OS bypass datapaths: RDMA, eBPF's XDP. We also include UDP sockets and NIC hardware timestamps (for comparison).

This repo contains:

- parameterized benchmark programs to extract, timestamps and latency metrics for 
    - `rdma/`: RDMA UD and RC (see `rdma/README.md` for additional info)
    - `xdp/`: eBPF XDP with 2 different implementations: AF_XDP socket client+server application and "XDP Poll", userspace app polling an eBPF map acting as a kernel-user space ring buffer with the XDP program. 
    - `no-bypass/`: basic UDP sockets 
    - `no-bypass/`: hardware timestamps taken by the NIC (e.g. Mellanox Connect-X 4 have this capabiliity)
- `ansible/`: ansible playbooks to run set up the environment and apply a series of performance enhancing Linux / hardware configuration to reduce jitter in packet processing
- `analysis`: Jupyter notebooks for data post-processing, result analysis, plots, experiment description.

## Build
> Make sure to prepare the environment before building. Playbooks `prepare.yaml`, `xdp.yaml` and `rdma.yaml` might be of use. Change the Ansible inventory accordingly. 

The build process follows the same steps as for any CMake project, with a variable SERVER to indicate if the program is the server or the client.

```bash
cd build
cmake -D SERVER=<0,1> ..
make
```

There is the option to build the programs using a `DEBUG` mode, which has extra logging and debugging information:

```bash
cd build
cmake -D DEBUG=1 -D SERVER=<0,1> ..
make
```

## Run
Client and servers might have different options listed as usage string. Run

```sh
cd build/xdp/
./pp_poll
Usage: ..  
```
Note that programs such as XDP require `sudo` and might require `ldconfig` before the starting test. 

## Results and analysis

By default, the results of the experiments are saved in a `.dat` file on the client machine. You can use the `analysis/large-eval/notebook.ipynb` playbook as reference to extract data and plot latency metrics. `analysis/report-0424` contains a summary of our findings. 

## Cloudlab
For the majority of our tests we used CloudLab (cloudlab.us)'s XL170 nodes. 