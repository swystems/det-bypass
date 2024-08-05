import argparse
import json
from pathlib import Path
import pandas as pd
import subprocess
import math
import bisect


def main():
    parser = argparse.ArgumentParser(prog='Performance benchmark')
    parser.add_argument("--program", type=Path, help="Path to the program to run")
    arguments = parser.parse_args()
    program = arguments.program
    intervals = [10, 15, 100, 150, 1000, 1500, 10000, 
                 15000,30000,50000, 70000, 80000, 90000, 100000]
                 #150000, 1000000, 1500000, 10000000, 15000000]
    intervals = [10, 15]
    interval_to_err_to_threshold = {}
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
        tmp = dataframe["err"]
        tmp.index = tmp.index.map(lambda el: float(el.split(" ")[0]))
        tmp = tmp.apply(lambda el: float(el.split(" %")[0]))
        tmp = tmp.sort_values()
        tmp = pd.Series(tmp.index, index=tmp)
        interval_to_err_to_threshold[interval] = tmp.to_dict()
        print(dataframe.to_latex())
        print("\\\\")
        print("")
    print(interval_to_err_to_threshold)
    output_cpp_map(interval_to_err_to_threshold)
    
    print(get_suggested_threshold(15, 556000))
    return


def output_cpp_map(interval_to_err):
  #    {
  #   {10, {{1020, 10.0}, {555410.0, 5.0}, {556980.0, 1.0},{562360.0, 3.0},{571610.0, 7.0}}},
  #   {15, {{1020, 10.0}, {555410.0, 5.0}, {556980.0, 1.0},{562360.0, 3.0},{571610.0, 7.0}}}
  # };
    output = open("interval_to_threshold.txt", "w")
    output.write("{\n")
    for interval, err in interval_to_err.items():
        output.write(f"{{ {interval}, {{")
        for error, th in err.items():
            output.write(f"{{ {error}, {th} }},")
        output.write(f"}}}},\n")
    output.write("};\n")
    output.close()
    
 
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
    


def get_suggested_threshold(interval, err):
    interval_to_err_to_threshold =  {10: {1020.0: 10.0, 555410.0: 5.0, 556980.0: 1.0, 562360.0: 3.0, 571610.0: 7.0}}
    intervals = list(interval_to_err_to_threshold.keys())
    closest_interval = bisect.bisect_left(intervals, interval)
    if closest_interval == len(intervals):
        closest_interval -= 1
    print(closest_interval)
    inter = intervals[closest_interval]
    if inter > interval and closest_interval != 0:
        inter = intervals[closest_interval-1]
    errors = list(interval_to_err_to_threshold[inter].keys())
    closest_err = bisect.bisect_left(errors, err)
    if closest_err == len(errors):
        closest_err -= 1
    print(closest_err)
    error = errors[closest_err]
    if error > err and closest_err != 0:
        error = errors[closest_err-1]
    return interval_to_err_to_threshold[inter][error]


    
    
if __name__ == "__main__":
    main()
