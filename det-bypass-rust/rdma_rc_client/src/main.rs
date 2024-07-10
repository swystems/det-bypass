use clap::{arg, value_parser, Command };


fn main() {
    let matches = Command::new("RC PingPong client").version("1.0").about("")
        .arg(arg!(-p --packets <PACKETS> "Number of packets to process in the experiment.").value_parser(value_parser!(u32).range(1..)))
        .arg(arg!(-d --dev <NAME> "Interface to attach XDP program to."))
        .arg(arg!(-g --gidx <IDX> "Group index to attach the XDP program to").value_parser(value_parser!(i32)))
        .arg(arg!(-i --interval <INTERVAL> "Interval between each packets in nanoseconds.").value_parser(value_parser!(u32).range(1..)))
        .arg(arg!(-s --server <SERVER> "Server ip address."))
        .arg(arg!(-m --measurament <MaESURAMENT> "Measurament to perform. 0: All Timestamps, 1: Min/Max latency, 2: Buckets.")
        .value_parser(["all", "latency", "buckets"]))
        .get_matches();
    let ib_devname: &str = matches.get_one::<&String>("dev").expect("dev is a required argument");
    let gidx: i32 = *matches.get_one::<i32>("gidx").unwrap();
    let interval: u64 = *matches.get_one::<u64>("interval").unwrap();
    let iters: u64 = *matches.get_one::<u64>("packets").unwrap_or(&1);
    let server_ip = matches.get_one::<&String>("server").unwrap();
    let persistence_flag = matches.get_one::<&String>("measurament").unwrap();
    let _ = rdma_rc_lib::run_client(ib_devname, gidx, interval, iters, server_ip, persistence_flag);    
}

