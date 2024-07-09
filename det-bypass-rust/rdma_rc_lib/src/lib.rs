use std::{net::Ipv4Addr, process, time::{SystemTime, UNIX_EPOCH}};

use std::io::Error;
use common::{persistence_agent::{PersistenceAgent, PingPongPayload}, utils};
use ib_net::ib_device_find_by_name;
use pingpong_context::PingpongContext;

mod ib_net;
mod pingpong_context;



fn initialize_random_number_generator() -> u64 {
    let pid = process::id();
    let time_now = SystemTime::now().duration_since(UNIX_EPOCH)
        .expect("Time went backwards")
        .as_secs();
    pid as u64 * time_now
    
}


pub fn pp_send_single_packet(_: *mut u8, packet_id: u64, _: Ipv4Addr, aux: *mut u8 ){
    let mut ctx = PingpongContext::new().unwrap();
    let mut payload = PingPongPayload::new(packet_id);
    payload.set_ts_value(1, utils::get_time_ns());
    ctx.send_payload(PingPongPayload::new(packet_id));
    ctx.post_send();
}


//int pp_send_single_packet (char *buf __unused, const uint64_t packet_id, struct sockaddr_ll *dest_addr __unused, void *aux)
//{
//    struct pingpong_context *ctx = (struct pingpong_context *) aux;
//    *ctx->send_payload = new_pingpong_payload (packet_id);
//    ctx->send_payload->ts[1] = get_time_ns ();
//
//    return pp_post_send (ctx, NULL);//(const uint8_t *) buf);
//}

pub fn run_client(ib_devname: &str, port_gid_idx: u32, iters: u64, interval: u64, server_ip: &str, pf: &str) -> Result<(), Error> {
    let persistence_flag = common::persistence_agent::pers_measurament_to_flag(pf);
    let _seed = initialize_random_number_generator();

    let device_list = rdma::device::DeviceList::available();
    let device_list: rdma::device::DeviceList = match device_list {
        Err(e) => return Err(Error::new(std::io::ErrorKind::Other, "writing to bucket failed")),
        Ok(dl) => dl
    };

    let persistence = PersistenceAgent::new(Some("rc.dat"), persistence_flag, &(interval as u32));
    
    let device = ib_device_find_by_name(&device_list, ib_devname);
    let device = match device {
        Err(e) => return Err(e),
        Ok(dev) => dev
    };
    
    Ok(())
}

union PingPongUnion{
    buff: u8,
    //payload: PingPongPayload
}

