use clap::{arg, value_parser, Command };



fn main() {
    let matches = Command::new("No bypass client").version("1.0").about("")
        .arg(arg!(-p --packets <PACKETS> "Number of packets to process in the experiment.").value_parser(value_parser!(u32).range(1..)))
        .get_matches();
    
    let iters: u64 = *matches.get_one::<u64>("packets").unwrap_or(&1);
    no_bypass_lib::run_server(iters);
}

