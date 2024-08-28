use common::{consts, persistence_agent, utils};
use rdma::wc;

use crate::{ib_net, post_context, ppc_ud};


pub fn run_client(ib_devname: &str, port_gid_idx: i32, iters: u64, interval: u64, server_ip: &str, pf: &str) -> Result<(), std::io::Error>{
    let persistence_flag = common::persistence_agent::pers_measurament_to_flag(pf);
    let mut persistence = persistence_agent::PersistenceAgent::new(Some("ud.dat"), persistence_flag, &(interval as u32));
    let mut ctx  = initialize(ib_devname, port_gid_idx, Some(server_ip))?;

    crate::start_sending_packets(iters, interval, ctx.clone(), send_single_packet);
    
    poll_client(iters, &mut ctx, &mut persistence)?;
    persistence.close();
    Ok(())
}


pub fn run_server(ib_devname: &str, port_gid_idx: i32, iters: u64) -> Result<(), std::io::Error>{
    let mut ctx = initialize(ib_devname, port_gid_idx, None)?;
    poll_server(iters, &mut ctx)?;
    Ok(())
}


pub fn initialize(ib_devname: &str, port_gid_idx: i32, server_ip: Option<&str>) -> Result<ppc_ud::UDContext, std::io::Error>{
    let seed = crate::initialize_random_number_generator();
    let device_list = rdma::device::DeviceList::available()?;
    
    let device = ib_net::ib_device_find_by_name(&device_list, ib_devname)?;
    let device = match device {
        Some(dev) => dev,
        _ => return utils::new_error("IB device {ib_devname} not found")
    };
    let mut ctx = ppc_ud::UDContext::new(device)?;
    let local_info = ib_net::IbNodeInfo::new(ctx.base_context(), consts::IB_PORT, port_gid_idx, seed)?;
    let (_, buf) = common::common_net::exchange_data(server_ip, &local_info.serialize())?;
    ctx.set_remote_info(ib_net::IbNodeInfo::deserialize(&buf));
    local_info.print();
    match &ctx.remote_info{
        Some(ri) => ri.print(),
        None => ()
    };

    ctx.connect(port_gid_idx, &local_info)?;
    println!("Connected");

    for i in 0..consts::QUEUE_SIZE{
        match ctx.post_recv(i){
            Ok(()) => (),
            Err(_) => {return utils::new_error("Couldn't post receive");}
        }
    }
    Ok(ctx)
}


pub fn poll_client(iters: u64, ctx: &mut ppc_ud::UDContext, persistence: &mut persistence_agent::PersistenceAgent)-> Result<(), std::io::Error>{
    let mut recv_idx = 0;
    while recv_idx < iters {
        const UNINIT_WC: std::mem::MaybeUninit<wc::WorkCompletion> = std::mem::MaybeUninit::uninit();
        let mut wc_buf = [UNINIT_WC; 2];
        let mut wcs = ctx.base_context().cq.poll(&mut wc_buf); 
        while wcs.is_err() {
            wcs = ctx.base_context().cq.poll(&mut wc_buf);
        }
        let wcs = wcs?;
        for wc in wcs{
            ctx.parse_single_wc_client(wc, persistence)?;
            if wc.wr_id() >= consts::QUEUE_SIZE as u64{
                let id = ctx.recv_payloads[wc.wr_id() as usize- consts::QUEUE_SIZE].id;
                recv_idx = u64::max(recv_idx, id);
            }
        }
    }
    Ok(())
}


pub fn poll_server(iters: u64, ctx: &mut ppc_ud::UDContext)-> Result<(), std::io::Error>{
    let mut recv_idx = 0;
    while recv_idx < iters {
        const UNINIT_WC: std::mem::MaybeUninit<wc::WorkCompletion> = std::mem::MaybeUninit::uninit();
        let mut wc_buf = [UNINIT_WC; 2];
        let mut wcs = ctx.base_context().cq.poll(&mut wc_buf); 
        while wcs.is_err() {
            wcs = ctx.base_context().cq.poll(&mut wc_buf);
        }
        let wcs = wcs?;
        for wc in wcs{
            ctx.parse_single_wc_server(wc)?;
            if wc.wr_id() >= consts::QUEUE_SIZE as u64{
                let id = ctx.recv_payloads[wc.wr_id() as usize- consts::QUEUE_SIZE].id;
                recv_idx = u64::max(recv_idx, id);
            }
        }
    }
    Ok(())
}


pub fn send_single_packet<T: post_context::PostContext>(packet_id: u64, context: &mut T) -> Result<(), std::io::Error>{
    context.set_pending_send_bit(0);
    //bitset::bitset_set(&mut context.pending_send, 0);
    let  mut payload = pingpong::PingPongPayload::new(packet_id);
    payload.set_ts_value(0, utils::get_time_ns());
    context.set_send_payload(pingpong::PingPongPayload::new(packet_id));
    let options = post_context::PostOptions{queue_idx: None, lkey: context.base_context().recv_mr.lkey(), buf: context.get_send_buf()};
    context.post_send(options)
}
