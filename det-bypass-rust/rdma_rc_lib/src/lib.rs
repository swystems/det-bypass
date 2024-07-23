use std::{process, thread, time::{SystemTime, UNIX_EPOCH}};

use common::utils;

mod ib_net;
mod pingpong_context;
mod ppc_ud;
mod ppc_rc;
mod post_context;
pub mod ud;
pub mod rc;

fn initialize_random_number_generator() -> u64 {
    let pid = process::id();
    let time_now = SystemTime::now().duration_since(UNIX_EPOCH)
        .expect("Time went backwards")
        .as_secs();
    pid as u64 * time_now
    
}


fn send_packets<T, F>(iters: u64, interval: u64, mut context: T, fun: F) -> Result<(), std::io::Error>
    where T: post_context::PostContext,
          F: Fn(u64, &mut T) -> Result<(), std::io::Error>
{
    println!("sending packets {}", iters);
    for id in 1..=iters {
        println!("id is {id}");
        let start = utils::get_time_ns();
        // (data.send_packet)(data.base_packet, id, data.socket_addr, &mut data.socket);
        fun(id, &mut context)?;
        //pp_send_single_packet(id, &mut context);
        println!("start {} now {}", start, utils::get_time_ns());
        let int = utils::get_time_ns() - start;
        if int < interval {
            utils::pp_sleep(interval-int); 
        }
    }
    println!("finished sending packets");
    Ok(())
}


fn start_sending_packets<T, F>(iters: u64, interval: u64, ctx: T, fun: F) -> std::thread::JoinHandle<Result<(), std::io::Error>> 
    where T: post_context::PostContext+'static,
          F: Fn(u64, &mut T) -> Result<(), std::io::Error> + Send+'static
{
    thread::spawn(move || send_packets(iters, interval, ctx, fun))
}

