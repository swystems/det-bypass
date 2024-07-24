
fn main() {
     let matches = clap::Command::new("Performance test").version("1.0").about("")
        .arg(clap::arg!(-s --sleep <SLEEP_TIME> "Time to sleep for in usec.").required(true))
        .arg(clap::arg!(-t --threshold <THRESHOLD> "Threshold").required(true))
        .get_matches();

    let sleep_time = matches.get_one::<String>("sleep").unwrap().replace('_', "");
    let threshold= matches.get_one::<String>("threshold").unwrap().replace('_', "");
    let threshold = threshold.parse::<u64>().expect("String not parsable");
    let mut durations = Vec::new();
    let attempts = 1000;
    let mut system = sysinfo::System::new_all();
    system.refresh_all();
    let initial_cpu_usage = system.global_cpu_info().cpu_usage();
    for _ in 0..attempts{
        let start = std::time::Instant::now();    
    
        common::utils::pp_sleep(sleep_time.parse::<u64>().expect("String not parsable"), threshold);
        let end = start.elapsed();
        durations.push(end);
        //println!("Time to sleep {}, Elapsed {:?}", sleep_time, end);
    }
    system.refresh_all();
    let final_cpu_usage = system.global_cpu_info().cpu_usage();

    println!("Initial CPU usage: {:.2}%", initial_cpu_usage);
    println!("Final CPU usage: {:.2}%", final_cpu_usage);
    
    let avg = std::time::Duration::from_secs_f64(mean(&durations));
    let std = std::time::Duration::from_secs_f64(std(&durations));
    println!("Desired sleep duration of {sleep_time} nanoseconds: {attempts} runs");
    println!("Avg: {:?}", avg);
    println!("Std: {:?}", std);
}

fn mean(durations: &[std::time::Duration]) -> f64{
    let dur = durations.iter().sum::<std::time::Duration>()/ durations.len() as u32;    
    dur.as_secs_f64()
}

fn std(durations: &[std::time::Duration]) -> f64{
    let mean = mean(durations);    
    let variance = durations.iter().map(|el| {
            let diff = el.as_secs_f64()- mean;
            diff*diff
        }
     ).sum::<f64>()/durations.len() as f64;
     variance.sqrt()
}
