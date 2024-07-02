use std::time::Instant;

pub fn get_time_ns() -> u64 {
    let start = Instant::now();
    let duration = start.elapsed();
    duration.as_secs() * 1_000_000_000 + u64::from(duration.subsec_nanos())
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
