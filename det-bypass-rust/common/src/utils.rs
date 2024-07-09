use std::{io::{Error, ErrorKind}, time::SystemTime};

pub fn get_time_ns() -> u64 {
    let duration = SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .expect("SystemTime before UNIX EPOCH!");

    duration.as_secs() * 1_000_000_000 + duration.subsec_nanos() as u64
}


pub fn pp_sleep(ns: u64){
    if ns==0{
        return;
    }
    // 1 ms
    if ns < 1000_000 {
        let start = get_time_ns();
        while get_time_ns() - start < ns {
            std::hint::spin_loop();
        } 
    } else {
        std::thread::sleep(std::time::Duration::from_nanos(ns));
    }
}

pub fn new_error<T>(msg: &str) -> Result<T, Error>{
    Err(Error::new(ErrorKind::Other, msg))
}
