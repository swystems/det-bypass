use std::io::Error;
use std::io::ErrorKind;

use rand::rngs::StdRng;
use rand::Rng;
use rand::SeedableRng;
use rdma::ctx::Context;
use rdma::device;
use rdma::bindings;
use rdma::device::Gid;
use rdma::device::LinkLayer;
use rdma::device::PortAttr;

use crate::pingpong_context::PingPongContext;



pub fn ib_device_find_by_name<'a>(device_list: &'a device::DeviceList, name: &str) -> Result<Option<&'a device::Device>, std::io::Error>{
    let mut dev: Option<&rdma::device::Device> = None;
    for device in device_list.as_slice() {
        if name == device.name(){
            dev = Some(device);
        }
    }
    Ok(dev)
}


pub struct IbNodeInfo {
    pub lid: u16,
    pub qpn: u32,
    pub psn: u32,
    pub gid: Gid 
}


impl IbNodeInfo{
    pub fn new(context:  &PingPongContext, ib_port: u8, gidx: i32, seed: u64) -> Result<Self, std::io::Error>{
        let port_attr = match PortAttr::query(context.context(), ib_port){
            Ok(pa) => pa,
            Err(e) => return Err(e)
        };

        let lid = port_attr.lid();
        if port_attr.link_layer() != LinkLayer::Ethernet && lid ==0{
            eprintln!("Couldn't get local LID");
            return Err(Error::new(ErrorKind::Other, "Couldn't get port info"));
        }
        let gid = match Gid::query(context.context(), ib_port, gidx){
            Ok(gid) => gid,
            Err(e) => return Err(e)
        };
        let mut r = StdRng::seed_from_u64(seed);
        let psn = r.gen::<u32>() & 0xffffff; 
        
        Ok(IbNodeInfo{lid, gid, qpn: context.qp_num(), psn})
    }


    pub fn print(&self){
        let res = self.gid.to_ipv6_addr(); 
        println!("Address: LID 0x{:04x}, QPN 0x{:06x}, PSN 0x{:06x}, GID {}", self.lid, self.qpn, self.psn, res);
    }
}

