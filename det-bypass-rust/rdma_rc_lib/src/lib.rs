use std::{process, time::{SystemTime, UNIX_EPOCH}};

use std::io::Error;
use common::{persistence_agent::{PersistenceAgent, PingPongPayload}, utils};
use ib_net::{ib_device_find_by_name, IbNodeInfo};
use pingpong_context::PingPongContext;
use rdma::{bindings, device::Device};

mod ib_net;
mod pingpong_context;

const IB_MTU: u32 = bindings::IBV_MTU_1024; 
const IB_PORT: u8 = 1;
const PRIORITY: u8 = 0;
const PING_PONG_RECV_WRID: u8 = 1;
const RECEIVE_DEPTH: u32 = 500;



fn initialize_random_number_generator() -> u64 {
    let pid = process::id();
    let time_now = SystemTime::now().duration_since(UNIX_EPOCH)
        .expect("Time went backwards")
        .as_secs();
    pid as u64 * time_now
    
}


pub fn pp_send_single_packet(packet_id: u64, mut context:  PingPongContext){
    let mut payload = PingPongPayload::new(packet_id);
    payload.set_ts_value(1, utils::get_time_ns());
    context.send_payload(PingPongPayload::new(packet_id));
    context.post_send();
}


pub fn run_client(ib_devname: &str, port_gid_idx: i32, iters: u64, interval: u64, server_ip: &str, pf: &str) -> Result<(), Error> {
    let persistence_flag = common::persistence_agent::pers_measurament_to_flag(pf);
    let seed = initialize_random_number_generator();

    let device_list = rdma::device::DeviceList::available();
    let device_list: rdma::device::DeviceList = match device_list {
        Err(_) => return utils::new_error("writing to bucket failed"),
        Ok(dl) => dl
    };

    let mut persistence = PersistenceAgent::new(Some("rc.dat"), persistence_flag, &(interval as u32));
    
    let device = ib_device_find_by_name(&device_list, ib_devname);
    let device: & Device = match device {
        Ok(Some(dev)) => dev,
        _ => return utils::new_error("IB device {ib_devname} not found"),
    };
    
    let mut ctx = match PingPongContext::new(device){
        Ok(ctx) => ctx,
        _ => return utils::new_error("Couldn't initialize context")
    }; 
    
    let local_info = match IbNodeInfo::new(&ctx, IB_PORT, port_gid_idx, seed){
        Ok(li) => li,
        _ => return utils::new_error("Couldn't get local info")
    };
    local_info.print();

    let remote_info = match IbNodeInfo::new(&ctx, IB_PORT, port_gid_idx, seed){
        Ok(li) => li,
        _ => return utils::new_error("Couldn't get remote info")
    };
    remote_info.print();


    match ctx.pp_ib_connect(IB_PORT, local_info.psn, pingpong_context::u32_to_mtu(IB_MTU).unwrap(), 
        PRIORITY, &remote_info, (port_gid_idx as usize).try_into().unwrap()){
        Ok(()) => (),
        _ => return utils::new_error("Couldn't connect")
    }

    ctx.set_pending(PING_PONG_RECV_WRID); 
    let available_receive: u32 = ctx.post_recv(RECEIVE_DEPTH);

    let recv_count: u64 = 0;
    let send_count: u64 = 0;
    
    while(recv_count < iters){
        let attr = PollCQAttr::empty();
        let mut res = ctx.start_poll(&attr); 
        while res == ENOENT{
            res = ctx.start_poll(&attr);
        }
        if res != 0{
            println!("Failed to poll CQ");
            return utils::new_error("Failed to poll CQ");
        }
        let res = ctx.parse_single_wc(&mut available_receive);
        match res {
            Err(_) => {
                println!("Failed to parse WC");
                ctx.end_poll();
                return utils::new_error("Failed to parse WC");
            }
        }
        recv_count +=1;
        let ret = ctx.next_poll();
        if !ret {
            ret = ctx.parse_single_wc(&mut available_receive);
            if !ret{
                recv_count += 1;
            }
        }
        ctx.end_poll();
        if ret && ret != ENOENT{
            println!("Failed to poll CQ");
            return utils::new_error("Failed to poll CQ");
        }
    }

    persistence.close();
    Ok(())
    
}

