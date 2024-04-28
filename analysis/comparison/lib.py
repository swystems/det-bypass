from dataclasses import dataclass
from enum import Enum
from math import floor, sqrt

import matplotlib.pyplot as plt
import numpy as np


class Measurement(Enum):
    ALL = 0
    BUCKETS = 2


@dataclass
class BucketsInfo:
    min_val: int
    max_val: int
    num_buckets: int
    bucket_size: float

    @staticmethod
    def from_list(vals: list[int]):
        return BucketsInfo(vals[0],vals[1], 0, 0)


@dataclass
class BucketsHeader:
    tot_packets: int
    rel_info: BucketsInfo
    abs_info: BucketsInfo
    min_packet: np.array
    max_packet: np.array


def parse_timestamps(filename: str, warmup=100) -> np.ndarray:
    """
    Read the content of filename and parse the timestamps it contains.
    The content should follow the format:
    <packet id> <timestamp 1> <timestamp 2> <timestamp 3> <timestamp 4>
    """
    data = np.fromfile(filename, sep=" ", dtype=int)
    arr = np.reshape(data, (len(data) // 5, 5))
    return arr


def parse_buckets(filename: str) -> tuple[BucketsHeader, np.ndarray, np.ndarray]:
    with open(filename, "r") as file:
        tot = int(file.readline().split(" ")[1])
        rel_info = BucketsInfo.from_list([int(x) for x in file.readline().split(" ")[1:]])
        abs_info = BucketsInfo.from_list([int(x) for x in file.readline().split(" ")[1:]])
        min_packet = [int(x) for x in file.readline().split(" ")[1:]]
        max_packet = [int(x) for x in file.readline().split(" ")[1:]]

        header = BucketsHeader(tot, rel_info, abs_info, np.array(min_packet), np.array(max_packet))

        # get all_buckets from the rest of the file
        all_buckets = np.fromfile(file, sep=" ", dtype=int)
        all_buckets = all_buckets.reshape((all_buckets.size//5, 5))
        
        rel_buckets = all_buckets[:, :4]
        rel_info.num_buckets = rel_buckets.shape[0]-2
        rel_info.bucket_size = (rel_info.max_val-rel_info.min_val)/rel_info.num_buckets
        abs_buckets = all_buckets[:, 4]
        abs_info.num_buckets = abs_buckets.shape[0]-2
        abs_info.bucket_size = (abs_info.max_val-rel_info.min_val)/abs_info.num_buckets

        return header, rel_buckets, abs_buckets


def compute_latency(ts) -> int:
    return ((ts[3] - ts[0]) - (ts[2] - ts[1])) // 2


def compute_latencies(timestamps: np.ndarray) -> np.ndarray:
    """
    Compute the latency of each packet.
    Given the timestamps ts1,ts2,ts3,ts4 of the packet, latency is approximated as:
    L = ((ts4-ts1)-(ts3-ts2))/2
    """
    lat = lambda round: np.array([round[0], compute_latency(round[1:])])
    return np.array([lat(round) for round in timestamps])


def compute_diffs(timestamps: np.ndarray) -> np.ndarray:
    return np.array([np.array([timestamps[i][0], *((timestamps[i] - timestamps[i - 1])[1:])]) for i in
                     range(1, len(timestamps))])  # if timestamps[i][0] == timestamps[i-1][0]+1])


def compute_jitter(latencies: np.ndarray) -> np.ndarray:
    """
    Compute jitter from latencies of the packets.
    Jitter is computed as the average deviation from the mean of the latencies.
    """
    N = len(latencies)
    avg = np.mean(latencies[:, 1])
    s = np.sum(np.abs(latencies[:, 1] - avg))
    return np.sqrt(s / N)


def compute_peaks(diffs: np.ndarray, threshold: int = 0) -> list[np.array]:
    """
    Analyze the diffs and detect the indices of packets identified as peaks.

    A peak is a packet that in a specific segment of the path SENDER->RECEIVER->SENDER
    got considerably delayed (> `threshold`). 

    This function returns a list of 3 `np.array`, where each list contains the packet
    that got delayed in that segment. This means that if in the return list the first 
    array contains the packet `5`, that packet had a delay while traveling from the
    SENDER to the RECEIVER.

    :param diffs: The difference of each packet in each segment. np.ndarray of size
                  number of packets x 4.
    :param threshold: The threshold which makes a packet be a "peak". If 0 or negative,
                      4*STD will be used.
    :return a list of 3 np.array, where array `i` contains the packets that got delayed
            from `diffs[i-1]` to `diffs[i]`. The values are the indices in the given diffs,
            not the packet id.
    """

    if threshold <= 0:
        s = 0
        for phase in range(1, 4):
            s += 5 * np.std(diffs[:, phase + 1] - diffs[:, phase])
        threshold = s / 3
        print(f"Using threshold {threshold}ns")

    done = set()
    result = [[], [], []]
    for phase in range(1, 4):
        anomalies = np.argwhere(abs(diffs[:, phase + 1] - diffs[:, phase]) > threshold).flatten()
        for i in anomalies:
            done.add(i)
        result[phase - 1] = anomalies
    return result


def compute_percentile(values: np.array, threshold: int) -> float:
    mean = np.mean(values)
    return len(np.argwhere(abs(values - mean) < threshold)) * 100 / len(values)


def plot_multi_scatter(ax, xvals, *args):
    for arg, name in args:
        y = np.full(xvals, None)
        for val in arg:
            y[val[0] - 1] = val[1]
        ax.scatter(x=range(xvals), y=y, s=1, label=name)


def plot_latencies(*args, iters=0):
    fig, ax = plt.subplots(1, 1)
    ax.grid(linestyle="--")
    ax.set_xlabel("Packet id")
    ax.set_ylabel("Packet latency (ns)")
    ax.set_title(f"Latency comparison for {', '.join([a[1] for a in args])}")
    plot_multi_scatter(ax, iters, *args)
    ax.legend()

def plot_diffs(diffs: np.ndarray, iters=0, with_peaks=True, peaks_threshold=0):
    fig, axs = plt.subplots(2, 2, figsize=(12, 6), layout="constrained")
    fig.get_layout_engine().set(w_pad=0.15, h_pad=0.15, hspace=0, wspace=0)
    #fig.suptitle("Timestamp difference between consequent packets")

    peaks = None
    if with_peaks:
        peaks = compute_peaks(diffs, peaks_threshold)

    for i in range(2 * 2):
        ax = axs[i // 2][i % 2]
        ax.grid(linestyle="-")
        ax.set_xlabel("Packet index")
        ax.set_ylabel("Timestamp difference (ns)")
        ax.set_title(f"{'PING' if i < 2 else 'PONG'} at {'RECV' if i & 1 else 'SEND'}")
        ax.scatter(x=diffs[:, 0], y=diffs[:, i + 1], s=1)
        if with_peaks and i > 0 and len(peaks[i - 1]) > 0:
            ax.scatter(x=diffs[peaks[i - 1], 0], y=diffs[peaks[i - 1], i + 1], s=1, c="red")

    return fig


def plot_diffs_top(diffs: np.ndarray, title='', with_peaks=True, peaks_threshold=0):
    fig, axs = plt.subplots(1,2, figsize=(12, 3), layout="constrained")
    fig.get_layout_engine().set(w_pad=0.15, h_pad=0.15, hspace=0, wspace=0)
    # fig.suptitle(title)

    peaks = None
    if with_peaks:
        peaks = compute_peaks(diffs, peaks_threshold)

    axs[0].grid(linestyle="-")
    axs[1].grid(linestyle="-")
    axs[0].set_xlabel("Packet index")
    axs[1].set_xlabel("Packet index")
    axs[0].set_ylabel("Timestamp difference (ns)")
    axs[0].scatter(x=diffs[:, 0], y=diffs[:, 1], s=1)
    axs[1].scatter(x=diffs[:, 0], y=diffs[:, 2], s=1)
    # if with_peaks and len(peaks[0]) > 0:
    #         axs[1].scatter(x=diffs[peaks[0], 0], y=diffs[peaks[0], 1], s=1, c="red")
    
    return fig


def plot_buckets(buckets: np.ndarray, info: BucketsInfo, send_rate: int = 0, suptitle: str = "", titles: list[str] = []):
    num = buckets.shape[1] if buckets.ndim > 1 else 1
    rows = int(np.sqrt(num))
    cols = num//rows

    if num > 1:
        fig,axs = plt.subplots(rows, cols, figsize=(12, 6), layout="constrained")
    else: 
        fig,axs = plt.subplots(rows, cols, layout="constrained")
    
    if suptitle:
        fig.suptitle(suptitle)

    for i in range(num):
        x = np.arange(info.min_val + info.bucket_size//2, info.max_val, info.bucket_size)
        if buckets.ndim > 1:
            y = buckets[1:-1,i]
        else:
            y = buckets[1:-1]
        ax = axs[i//cols,i%cols] if buckets.ndim > 1 else axs
        ax.set_yscale("log")
        ax.set_ylabel("Number of packets")
        ax.set_xlabel("Latency (ns)")
        if len(titles) > i:
            ax.set_title(titles[i])

        #ax.axvline(x=send_rate, color='r')
        
        ax.bar(x=x,height=y,width=info.bucket_size,color='tab:blue')

        if buckets.ndim > 1:
            outliers = [buckets[0,i],buckets[-1,i]]
        else:
            outliers = [buckets[0],buckets[-1]]
        outliers_x = [-5*info.bucket_size, info.max_val + 5*info.bucket_size]
        ax.bar(x=outliers_x,height=outliers,width=9*info.bucket_size,color='tab:red')



def copy_remote_file(remote_addr: str, local_path: str):
    import os
    if not os.path.isfile(local_path):
        os.system(f"rsync -r {remote_addr} {local_path}")
    else:
        print(f"File {local_path} exists, skipping copy from remote machine...")


def compute_experiment_data(remote_file, local_file):
    """
    Utility function to retrieve and compute all data about an experiment run.
    It just consists in a combination of functions in this file.

    :return a tuple (timestamps, diffs, latency, jitter)
    """
    copy_remote_file(remote_file, local_file)
    ts = parse_timestamps(local_file)
    diffs = compute_diffs(ts)
    lat = compute_latencies(ts)
    jitter = compute_jitter(lat)
    return (ts, diffs, lat, jitter)


def to_unit(value: int, base_unit: str, mult: int = 0) -> str:
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
    idx = 3 * (l // 3)
    letter = units[idx]
    val = value / (10 ** (idx - mult))
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
        kwarg_strs = '-'.join(f"{key}_{value}" for key, value in self.kwargs.items())
        parts = [to_unit(self.iters, ''), to_unit(self.interval, 's', -9)]
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
        kwarg_strs = ', '.join(f"{key.capitalize()} {value}" for key, value in self.kwargs.items())
        parts = [f"Iterations {to_unit(self.iters, '')}", f"Interval {to_unit(self.interval, 's', -9)}"]
        if len(self.kwargs) > 0:
            parts.append(kwarg_strs)
        return f"Settings: {', '.join(parts)}"
