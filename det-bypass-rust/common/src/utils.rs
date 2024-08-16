use std::{io::{Error, ErrorKind} , time::SystemTime};

pub fn get_time_ns() -> u64 {
    let duration = SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .expect("SystemTime before UNIX EPOCH!");
    duration.as_nanos() as u64
}


pub fn pp_sleep(mut ns: u64){
    if ns==0{
        return;
    }
    let start = std::time::Instant::now();

    if ns > 1_000_000{
        let sleep_duration = ns - 1_000_000;
        std::thread::sleep(std::time::Duration::from_nanos(sleep_duration));
        let now = std::time::Instant::now();
        // One might expect the diff to always be 1_000_000 nanoseconds,
        // but given that thread::sleep might sleep for more than the requested time
        // it might not always be the case.
        let diff =  now.duration_since(start);
        ns -= diff.as_nanos() as u64;
    }
    let new_start = get_time_ns();
    while get_time_ns() - new_start< ns {
        std::hint::spin_loop();
    }
        
}

pub fn new_error<T>(msg: &str) -> Result<T, Error>{
    Err(Error::new(ErrorKind::Other, msg))
}
