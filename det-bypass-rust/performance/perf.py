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
    intervals = [10, 15, 100, 150, 1000, 1500, 10000, 15000, 100000, 
                 150000, 1000000, 1500000, 10000000, 15000000]
    for interval in intervals:
        thresholds = compute_thresholds(interval)
        data = {"cpu": [],
                "avg": [],
                "std": [],
                "max": [],
                "min": [],
                "cpu_cycles": []}
        for th in thresholds:
            # perf stat -- target/release/performance -t 10 -s 1000_000
            result = subprocess.run(["perf", "stat", "--", "../target/release/performance", "-t", f"{th}" , "-s", f"{interval}"], capture_output=True, text=True)
            th_data = parse_result(result)
            for k, v in data.items():
                data[k].append(th_data[k])
        # print(data)
        print(f"Results for interval {interval} are: ")
        dataframe = pd.DataFrame(data, index=thresholds)
        print(dataframe.to_latex())
        print("")
        print("")
        print("")
        
 


        
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
    data["cpu"] = stdout_lines[1].split(": ")[1]
    data["avg"] = stdout_lines[3].split(": ")[1]
    data["std"] = stdout_lines[4].split(": ")[1]
    data["max"] = stdout_lines[5].split(": ")[1]
    data["min"] = stdout_lines[6].split(": ")[1]
    
    data["cpu_cycles"]=( stderr_lines[7].split(" ")[0])
    return data
    
    
if __name__ == "__main__":
    main()
