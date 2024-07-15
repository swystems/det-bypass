use clap::{arg, value_parser, Command };


fn main() {
    let matches = Command::new("UD PingPong client").version("1.0").about("")
        .arg(arg!(-p --packets <PACKETS> "Number of packets to process in the experiment.")
            .value_parser(value_parser!(u64).range(1..)).required(true))
        .arg(arg!(-d --dev <NAME> "Interface to attach XDP program to.").required(true))
        .arg(arg!(-g --gidx <IDX> "Group index to attach the XDP program to").value_parser(value_parser!(i32)).required(true))
        .get_matches();
    let ib_devname = matches.get_one::<String>("dev").expect("dev is a required argument");
    let gidx: i32 = *matches.get_one::<i32>("gidx").unwrap();
    let iters: u64 = *matches.get_one::<u64>("packets").unwrap_or(&1);
    let _ = rdma_rc_lib::ud::run_server(ib_devname, gidx, iters);    
}

