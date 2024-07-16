use common::{consts, persistence_agent, utils};
use rdma::{bindings, device, poll_cq_attr};

use crate::{ib_net, pingpong_context, post_context, ppc_rc};

const IB_MTU: u32 = bindings::IBV_MTU_1024;

pub fn run_server(ib_devname: &str, port_gid_idx: i32, iters: u64) -> Result<(), std::io::Error>{
    let  mut ctx  = initialize(ib_devname, port_gid_idx, None)?;
  println!("after initialize");
    poll(iters, &mut ctx, &mut None)?;
    Ok(())
}

pub fn run_client(ib_devname: &str, port_gid_idx: i32, iters: u64, interval: u64, server_ip: &str, pf: &str) -> Result<(), std::io::Error>{
    let persistence_flag = common::persistence_agent::pers_measurament_to_flag(pf);
    let mut persistence = persistence_agent::PersistenceAgent::new(Some("rc.dat"), persistence_flag, &(interval as u32));
    let mut ctx  =initialize(ib_devname, port_gid_idx, Some(server_ip))?;
    crate::start_sending_packets(iters, interval, ctx.clone(), pp_send_single_packet);
    
    poll(iters, &mut ctx, &mut Some(&mut persistence))?;
    persistence.close();
    Ok(())
}



fn initialize(ib_devname: &str, port_gid_idx: i32, server_ip: Option<&str>) -> Result<ppc_rc::RCContext, std::io::Error>{
    let seed = crate::initialize_random_number_generator();

    let device_list = rdma::device::DeviceList::available();
    let device_list: rdma::device::DeviceList = match device_list {
        Err(_) => return utils::new_error("writing to bucket failed"),
        Ok(dl) => dl
    };

    
    let device = ib_net::ib_device_find_by_name(&device_list, ib_devname);
    let device: & device::Device = match device {
        Ok(Some(dev)) => dev,
        _ => return utils::new_error("IB device {ib_devname} not found"),
    };
    
    let ctx = match ppc_rc::RCContext::new(device){
        Ok(ctx) => ctx,
        _ => return utils::new_error("Couldn't initialize context")
    }; 
    
    let local_info = match ib_net::IbNodeInfo::new(ctx.base_context(), consts::IB_PORT, port_gid_idx, seed){
        Ok(li) => li,
        _ => return utils::new_error("Couldn't get local info")
    };
    local_info.print();

    let (_, buf) = common::common_net::exchange_data(server_ip, &local_info.serialize())?;
    let remote_info = ib_net::IbNodeInfo::deserialize(&buf);
    remote_info.print();


    match ctx.pp_ib_connect(consts::IB_PORT, local_info.psn, pingpong_context::u32_to_mtu(IB_MTU).unwrap(), 
        consts::PRIORITY, &remote_info, (port_gid_idx as usize).try_into().unwrap()){
        Ok(()) => (),
        _ => return utils::new_error("Couldn't connect")
    }

    Ok(ctx)
}


fn poll(iters: u64, ctx: &mut ppc_rc::RCContext, persistence: &mut Option<&mut persistence_agent::PersistenceAgent>) -> Result<(), std::io::Error>{
    let mut available_receive: u32 = ctx.post_recv(consts::RECEIVE_DEPTH);
    let mut recv_count: u64 = 0;
    
    while recv_count < iters {
        let attr = poll_cq_attr::PollCQAttr::new_empty();
        let mut res = ctx.start_poll(&attr); 
        while let Err(pingpong_context::PollingError::Enoent(_)) = res{
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

pub fn pp_send_single_packet<T: post_context::PostContext>(packet_id: u64, context:  &mut T){
    let mut payload = persistence_agent::PingPongPayload::new(packet_id);
    payload.set_ts_value(1, utils::get_time_ns());
    context.set_send_payload(persistence_agent::PingPongPayload::new(packet_id));
    let _ = context.post_send(post_context::PostOptions { queue_idx: None, lkey:0 , buf: std::ptr::null_mut()});
}
