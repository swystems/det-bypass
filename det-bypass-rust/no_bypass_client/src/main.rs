use clap::{arg, value_parser, Command};



fn main() {
    let matches = Command::new("No bypass client").version("1.0").about("")
        .arg(arg!(-p --packets <PACKETS> "Number of packets to process in the experiment.").value_parser(value_parser!(u64).range(1..)))
        .arg(arg!(-i --interval <INTERVAL> "Interval between each packets in nanoseconds.").value_parser(value_parser!(u32).range(1..)))
        .arg(arg!(-s --server <SERVER> "Server ip address."))
        .arg(arg!(-m --measurament <MaESURAMENT> "Measurament to perform. 0: All Timestamps, 1: Min/Max latency, 2: Buckets.")
        .value_parser(["all", "latency", "buckets"]))
        .get_matches();
    
    let default = &"all".to_string();
    let persistence_flag: &str = matches.get_one::<String>("measurament").unwrap_or(default);
    let iters: u64 = *matches.get_one::<u64>("packets").unwrap();
    let interval: u64 = *matches.get_one::<u64>("interval").unwrap_or(&0);
    let server: &str = matches.get_one::<String>("server").expect("server is a required argument");
    
    no_bypass_lib::run_client(iters, interval, server, persistence_flag); 
    
}
