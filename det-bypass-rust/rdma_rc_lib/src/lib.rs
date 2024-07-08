use std::{process, time::{SystemTime, UNIX_EPOCH}};

use std::io::Error;
use common::persistence_agent::PersistenceAgent;
use ib_net::ib_device_find_by_name;
use rdma::device;

mod ib_net;
mod pingpong_context;



fn initialize_random_number_generator() -> u64 {
    let pid = process::id();
    let time_now = SystemTime::now().duration_since(UNIX_EPOCH)
        .expect("Time went backwards")
        .as_secs();
    pid as u64 * time_now
    
}



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



//struct PingpongContext {
//    pending: AtomicU8,// WID of the pending WR
//
//    send_flags: u32,
//
//    recv: PingPongUnion,
//    send: PingPongUnion,
//
//    struct ibv_context *context;
//
//    struct ibv_pd *pd;
//    uint64_t completion_timestamp_mask;
//
//    struct ibv_mr *recv_mr;
//    struct ibv_mr *send_mr;
//
//    struct ibv_cq_ex *cq;
//
//    struct ibv_qp *qp;
//    struct ibv_qp_ex *qpx;
//};
//

fn pp_init_context(device: & device::Device){
    

}
