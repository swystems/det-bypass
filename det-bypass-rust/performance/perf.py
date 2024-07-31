import argparse
from pathlib import Path
import pandas as pd
import subprocess
import math


def main():
    parser = argparse.ArgumentParser(prog='Performance benchmark')
    parser.add_argument("--program", type=Path, help="Path to the program to run")
    arguments = parser.parse_args()
    program = arguments.program
    intervals = [10, 15, 100, 150, 1000, 1500, 10000, 
                 15000,30000,50000, 70000, 80000, 90000, 100000]
                 #150000, 1000000, 1500000, 10000000, 15000000]
    for interval in intervals:
        thresholds = compute_thresholds(interval)
        data = {"avg": [],
                "std": [],
                "max": [],
                "min": [],
                "err": [],
                "cpu_cycles": []}
        for th in thresholds:
            # perf stat -- target/release/performance -t 10 -s 1000_000
            result = subprocess.run(["perf", "stat", "--", "../target/release/performance", "-t", f"{th}" , "-s", f"{interval}"], capture_output=True, text=True)
            th_data = parse_result(result)
            avg = unit_to_interval(th_data["avg"])
            err = (avg-interval)*100/interval
            th_data["err"] = f"{err:.3f} %"
            for k, v in data.items():
                data[k].append(th_data[k])

        # print(data)
        print(f"The results for an interval of {interval_to_unit(interval)} are: ")
        thresholds = [interval_to_unit(th) for th in thresholds]
        dataframe = pd.DataFrame(data, index=thresholds)
        print(dataframe.to_latex())
        print("\\\\")
        print("")
        
 
def interval_to_unit(interval):
    if interval < 1000:
        return f"{interval} ns"
    if interval < 1000000:
        return f"{interval/1000} µs"
    if interval < 1000000000:
        return f"{interval/1000000} ms"


def unit_to_interval(unit):
    unit_type = unit[-2:]
    val = float(unit[:-2])
    if unit_type == "µs":
        return val *1000        
    if unit_type == "ms":
        return val * 1000000
    if unit_type == "ns":
        return val

        
def compute_thresholds(val):
    thresholds = [0.1, 0.3, 0.5, 0.7, 1]
    res = []
    for th in thresholds:
        res.append(math.floor(th*val))
    return res

def parse_result(result):
    stdout_lines = result.stdout.splitlines()
    stderr_lines = result.stderr.splitlines()
    stderr_lines = [l.strip() for l in stderr_lines]
    data = {}
    data["avg"] = stdout_lines[1].split(": ")[1]
    data["std"] = stdout_lines[2].split(": ")[1]
    data["max"] = stdout_lines[3].split(": ")[1]
    data["min"] = stdout_lines[4].split(": ")[1]
    
    data["cpu_cycles"]=( stderr_lines[7].split(" ")[0])
    return data
    
    
if __name__ == "__main__":
    main()
