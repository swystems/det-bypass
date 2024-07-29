use std::{io::{Error, ErrorKind}, time::SystemTime};

pub fn get_time_ns() -> u64 {
    let duration = SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .expect("SystemTime before UNIX EPOCH!");
    duration.as_nanos() as u64
}


pub fn pp_sleep(mut ns: u64, threshold: u64){
    if ns==0{
        return;
    }
    let start = std::time::Instant::now();

    if ns > threshold{
        let sleep_duration = std::time::Duration::from_nanos(ns - threshold);
        std::thread::sleep(sleep_duration);
        // unsafe { libc::nanosleep(&libc::timespec{tv_sec: sleep_duration.as_secs() as i64, tv_nsec: sleep_duration.subsec_nanos() as i64}, &mut libc::timespec{tv_sec: 0, tv_nsec: 0}) };
        // One might expect the diff to always be 1_000_000 nanoseconds,
        // but given that thread::sleep might sleep for more than the requested time
        // it might not always be the case.
        let diff =  start.elapsed();
       // println!("diff {:?}", diff);
        if diff.as_nanos() as u64 > ns{
            return;
        }
        ns -= diff.as_nanos() as u64;
        // println!("NS is {ns} {:?}", diff);
    }
    let new_start = get_time_ns();
    while get_time_ns() - new_start< ns {
        std::hint::spin_loop();
        //std::thread::yield_now();
    }
        
}

pub fn new_error<T>(msg: &str) -> Result<T, Error>{
    Err(Error::new(ErrorKind::Other, msg))
}
