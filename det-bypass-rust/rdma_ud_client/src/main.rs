use clap::{arg, value_parser, Command };


fn main() {
    let matches = Command::new("UD PingPong client").version("1.0").about("")
        .arg(arg!(-p --packets <PACKETS> "Number of packets to process in the experiment.").value_parser(value_parser!(u64).range(1..)).required(true))
        .arg(arg!(-d --dev <NAME> "Interface to attach XDP program to.").required(true))
        .arg(arg!(-g --gidx <IDX> "Group index to attach the XDP program to").value_parser(value_parser!(i32)).required(true))
        .arg(arg!(-i --interval <INTERVAL> "Interval between each packets in nanoseconds.").value_parser(value_parser!(u64).range(1..)).required(true))
        .arg(arg!(-s --server <SERVER> "Server ip address.").required(true))
        .arg(arg!(-m --measurament <MaESURAMENT> "Measurament to perform. 0: All Timestamps, 1: Min/Max latency, 2: Buckets.")
        .value_parser(["all", "latency", "buckets"]).required(true))
        .get_matches();
    let ib_devname: &str = matches.get_one::<String>("dev").expect("dev is a required argument");
    let gidx: i32 = *matches.get_one::<i32>("gidx").unwrap();
    let interval: u64 = *matches.get_one::<u64>("interval").unwrap();
    let iters: u64 = *matches.get_one::<u64>("packets").unwrap_or(&1);
    let server_ip = matches.get_one::<String>("server").unwrap();
    let persistence_flag = matches.get_one::<String>("measurament").unwrap();
    if let Err(e) = rdma_rc_lib::ud::run_client(ib_devname, gidx, interval, iters, server_ip, persistence_flag){
        println!("{e}");
    } 
}

