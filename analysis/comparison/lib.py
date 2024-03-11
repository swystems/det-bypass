import numpy as np
import matplotlib.pyplot as plt

def parse_timestamps(filename: str, warmup=100) -> np.ndarray:
    """
    Read the content of filename and parse the timestamps it contains.
    The content should follow the format:
    <packet id> <timestamp 1> <timestamp 2> <timestamp 3> <timestamp 4>
    """
    data = np.fromfile(filename, sep=" ", dtype=int)
    arr = np.reshape(data, (len(data)//5,5))
    return arr

def compute_latency(ts) -> int:
    return ((ts[3]-ts[0])-(ts[2]-ts[1]))//2


def compute_latencies(timestamps: np.ndarray) -> np.ndarray:
    """
    Compute the latency of each packet.
    Given the timestamps ts1,ts2,ts3,ts4 of the packet, latency is approximated as:
    L = ((ts4-ts1)-(ts3-ts2))/2
    """
    lat = lambda round: np.array([round[0],compute_latency(round[1:])])
    return np.array([lat(round) for round in timestamps])

def compute_diffs(timestamps: np.ndarray) -> np.ndarray:
    return np.array([np.array([timestamps[i][0],*((timestamps[i]-timestamps[i-1])[1:])]) for i in range(1,len(timestamps))])# if timestamps[i][0] == timestamps[i-1][0]+1])


def compute_jitter(latencies: np.ndarray) -> np.ndarray:
    """
    Compute jitter from latencies of the packets.
    Jitter is computed as the average deviation from the mean of the latencies.
    """
    N = len(latencies)
    avg = np.mean(latencies[:,1])
    s = np.sum(np.abs(latencies[:,1]-avg))
    return np.sqrt(s/N)


def plot_multi_scatter(ax, xvals, *args):
    for arg,name in args:
        y = np.full(xvals, None)
        for val in arg:
            y[val[0]-1] = val[1]
        ax.scatter(x=range(xvals), y=y, s=1, label=name)


def plot_latencies(*args, iters=0):
    fig,ax = plt.subplots(1,1)
    ax.grid(linestyle="--")
    ax.set_xlabel("Packet id")
    ax.set_ylabel("Packet latency (ns)")
    ax.set_title(f"Latency comparison for {', '.join([a[1] for a in args])}")
    plot_multi_scatter(ax, iters, *args)
    ax.legend()


def plot_diffs(diffs: np.ndarray, iters=0):
    fig,axs = plt.subplots(2,2,figsize=(12,6),layout="constrained")
    fig.get_layout_engine().set(w_pad=0.15, h_pad=0.15, hspace=0, wspace=0)
    fig.suptitle("Timestamp difference between consequent packets")
    values = np.full([iters,4], None)
    for diff in diffs:
        values[diff[0]-1] = diff[1:]
    for i in range(2*2):
        ax = axs[i//2][i%2]
        ax.grid(linestyle="-")
        ax.set_xlabel("Packet index")
        ax.set_ylabel("Timestamp difference (ns)")
        ax.set_title(f"{'PING' if i<2 else 'PONG'} at {'RECV' if i&1 else 'SEND'}")
        ax.scatter(x=range(iters), y=values[:,i], s=1)


def copy_remote_file (remote_addr: str, local_path: str):
    import os
    if not os.path.isfile(local_path):
        os.system(f"rsync -r {remote_addr} {local_path}")
    else:
        print(f"File {local_path} exists, skipping copy from remote machine...")


def compute_experiment_data (remote_file, local_file):
    """
    Utility function to retrieve and compute all data about an experiment run.
    It just consists in a combination of functions in this file.

    :return a tuple (timestamps, diffs, latency, jitter)
    """
    copy_remote_file (remote_file, local_file)
    ts = parse_timestamps (local_file)
    diffs = compute_diffs(ts)
    lat = compute_latencies(ts)
    jitter = compute_jitter (lat)
    return (ts,diffs,lat,jitter)


def to_unit (value: int, base_unit: str, mult: int = 0) -> str:
    """
    Convert value to the correct unit. For example, 1000m should become 1km.

    :param value: the value to transform to string
    :param base_unit: the base unit of the value, e.g. 's' (seconds), 'm' (meters), etc.
    :param mult: optional parameter, represents the value of the current unit in the base unit, expressed as the exponent of 10.
    :return the string with the unit that makes the value the smallest.

    For example, to transform 'ns', the function can be called as:
    > to_unit (10000, 's', -9)
    > "10us"
    Multiplier is 1e-9 because 1ns is 1e-9s.
    """
    from math import floor
    units = {-9: 'n', -6: 'u', -3: 'm', 0: '', 3: 'k', 6: 'M', 9: 'G'}
    l = floor(np.log10(value)) + mult
    idx = 3*(l//3)
    letter = units[idx]
    val = value/(10**(idx-mult))
    if floor(val) == val:
        val = floor(val)
    return f"{val}{letter}{base_unit}"
    
    
class Settings:
    def __init__(self, iters: int, interval: int, **kwargs):
        """
        Create a settings instance with the given data.
        
        :param iters: Number of iterations of the experiment
        :param interval: Interval in ns of packet send
        :param kwargs: Any other option passed using kwargs will be used in the string generation
        """
        self.iters = int(iters)
        self.interval = int(interval)
        self.kwargs = kwargs

    def __str__(self):
        """
        Generate a string representing the settings to be used in filenames.
        
        Example:
        > Settings(iters=100, interval=1000000, testkey='testvalue').pretty_format()
        100-1ms-testkey_testvalue
        """
        kwarg_strs = '-'.join(f"{key}_{value}" for key,value in self.kwargs.items())
        parts = [to_unit (self.iters, ''), to_unit (self.interval, 's', -9)]
        if len(self.kwargs) > 0:
            parts.append(kwarg_strs)
        return '-'.join(parts)

    def pretty_format(self):
        """
        Generate a string representing the settings to be human-readable
        
        Example:
        > Settings(iters=100, interval=1000000).pretty_format()
        Settings: Iterations 100, Interval 1ms
        """
        kwarg_strs = ', '.join(f"{key.capitalize()} {value}" for key,value in self.kwargs.items())
        parts = [f"Iterations {to_unit (self.iters, '')}", f"Interval {to_unit (self.interval, 's', -9)}"]
        if len(self.kwargs) > 0:
            parts.append(kwarg_strs)
        return f"Settings: {', '.join(parts)}"