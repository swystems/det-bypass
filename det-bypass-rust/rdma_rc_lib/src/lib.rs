use std::{process, thread, time::{SystemTime, UNIX_EPOCH}};

use std::io::Error;
use common::{persistence_agent::{PersistenceAgent, PingPongPayload}, utils};
use ib_net::{ib_device_find_by_name, IbNodeInfo};
use rdma::{bindings, device::Device, poll_cq_attr::PollCQAttr};

mod ib_net;
mod pingpong_context;
mod ppc_ud;
mod ppc_rc;


const IB_MTU: u32 = bindings::IBV_MTU_1024; 
const IB_PORT: u8 = 1;
const PRIORITY: u8 = 0;
const RECEIVE_DEPTH: u32 = 500;



fn initialize_random_number_generator() -> u64 {
    let pid = process::id();
    let time_now = SystemTime::now().duration_since(UNIX_EPOCH)
        .expect("Time went backwards")
        .as_secs();
    pid as u64 * time_now
    
}


pub fn pp_send_single_packet(packet_id: u64, context:  &mut ppc_rc::RCContext){
    let mut payload = PingPongPayload::new(packet_id);
    payload.set_ts_value(1, utils::get_time_ns());
    context.set_send_payload(PingPongPayload::new(packet_id));
    let _ = context.post_send();
}

pub fn run_server(ib_devname: &str, port_gid_idx: i32, iters: u64) -> Result<(), Error>{
    let  mut ctx  = match initialize(ib_devname, port_gid_idx, None){
        Err(e) => return Err(e),
        Ok(ctx) =>  ctx
    };
    poll(iters, &mut ctx, &mut None)?;
    Ok(())
}

pub fn run_client(ib_devname: &str, port_gid_idx: i32, iters: u64, interval: u64, server_ip: &str, pf: &str) -> Result<(), Error>{
    let persistence_flag = common::persistence_agent::pers_measurament_to_flag(pf);
    let mut persistence = PersistenceAgent::new(Some("rc.dat"), persistence_flag, &(interval as u32));
    let mut ctx  =initialize(ib_devname, port_gid_idx, Some(server_ip))?;
    start_sending_packets(iters, interval, ctx.clone());
    
    poll(iters, &mut ctx, &mut Some(&mut persistence))?;
    persistence.close();
    Ok(())
}


fn send_packets(iters: u64, interval: u64, mut context: ppc_rc::RCContext){
    println!("sending packets {}", iters);
    for id in 1..=iters {
        println!("id is {id}");
        let start = utils::get_time_ns();
        // (data.send_packet)(data.base_packet, id, data.socket_addr, &mut data.socket);
        pp_send_single_packet(id, &mut context);
        println!("start {} now {}", start, utils::get_time_ns());
        let int = utils::get_time_ns() - start;
        if int < interval {
            utils::pp_sleep(interval-int); 
        }
    }
    println!("finished sending packets");
}


fn start_sending_packets(iters: u64, interval: u64, ctx: ppc_rc::RCContext) -> std::thread::JoinHandle<()> {
    thread::spawn(move || send_packets(iters, interval, ctx))
}


fn initialize(ib_devname: &str, port_gid_idx: i32, server_ip: Option<&str>) -> Result<ppc_rc::RCContext, Error>{
    let seed = initialize_random_number_generator();

    let device_list = rdma::device::DeviceList::available();
    let device_list: rdma::device::DeviceList = match device_list {
        Err(_) => return utils::new_error("writing to bucket failed"),
        Ok(dl) => dl
    };

    
    let device = ib_device_find_by_name(&device_list, ib_devname);
    let device: & Device = match device {
        Ok(Some(dev)) => dev,
        _ => return utils::new_error("IB device {ib_devname} not found"),
    };
    
    let ctx = match ppc_rc::RCContext::new(device){
        Ok(ctx) => ctx,
        _ => return utils::new_error("Couldn't initialize context")
    }; 
    
    let local_info = match IbNodeInfo::new(&ctx.base_context(), IB_PORT, port_gid_idx, seed){
        Ok(li) => li,
        _ => return utils::new_error("Couldn't get local info")
    };
    local_info.print();

    let (_, buf) = common::common_net::exchange_data(server_ip, &local_info.serialize())?;
    let remote_info = IbNodeInfo::deserialize(&buf);
    remote_info.print();


    match ctx.pp_ib_connect(IB_PORT, local_info.psn, pingpong_context::u32_to_mtu(IB_MTU).unwrap(), 
        PRIORITY, &remote_info, (port_gid_idx as usize).try_into().unwrap()){
        Ok(()) => (),
        _ => return utils::new_error("Couldn't connect")
    }

    Ok(ctx)
}


fn poll(iters: u64, ctx: &mut ppc_rc::RCContext, persistence: &mut Option<&mut PersistenceAgent>) -> Result<(), Error>{
    let mut available_receive: u32 = ctx.post_recv(RECEIVE_DEPTH);
    let mut recv_count: u64 = 0;
    
    while recv_count < iters {
        let attr = PollCQAttr::new_empty();
        let mut res = ctx.start_poll(&attr); 
        while let Err(pingpong_context::PollingError::ENOENT(_)) = res{
            res = ctx.start_poll(&attr);
        }
        if let Err(pingpong_context::PollingError::Other(_)) = res {
            println!("Failed to poll CQ");
            return utils::new_error("Failed to poll CQ");
        }
        let res = ctx.parse_single_wc(&mut available_receive, persistence);
        if res.is_err() {
            println!("Failed to parse WC");
            ctx.end_poll();
            return utils::new_error("Failed to parse WC");
        }
        recv_count +=1;
        let mut ret = ctx.next_poll();
        if let Ok(()) = ret {
            let parse_ret = ctx.parse_single_wc(&mut available_receive, persistence);
            match parse_ret{
                Ok(_) => recv_count +=1,
                Err(e) => {
                    ret = Err(pingpong_context::PollingError::Other(e.to_string()));
                } 
            }
        }
        ctx.end_poll();
        if let Err(pingpong_context::PollingError::Other(_)) = ret {
            println!("Failed to poll CQ");
            return utils::new_error("Failed to poll CQ");
        }
    }
    Ok(())
}



pub fn run_client_ud(ib_devname: &str, port_gid_idx: i32, iters: u64, interval: u64, server_ip: &str, pf: &str) -> Result<(), Error>{
    let persistence_flag = common::persistence_agent::pers_measurament_to_flag(pf);
    let mut persistence = PersistenceAgent::new(Some("ud.dat"), persistence_flag, &(interval as u32));
    let mut ctx  = match initialize(ib_devname, port_gid_idx, Some(server_ip)){
        Err(e) => return Err(e),
        Ok( ctx) => ctx 
    };

    start_sending_packets(iters, interval, ctx.clone());
    
    poll(iters, &mut ctx, &mut Some(&mut persistence))?;
    persistence.close();
    Ok(())
}


