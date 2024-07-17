use common::utils;
use rand::rngs::StdRng;
use rand::Rng;
use rand::SeedableRng;
use rdma::device;
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


#[derive(Clone)]
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
            return utils::new_error("Couldn't get port info");
        }
        let gid = match Gid::query(context.context(), ib_port, gidx){
            Ok(gid) => gid,
            Err(e) => return Err(e)
        };
        let mut r = StdRng::seed_from_u64(seed);
        let psn = r.gen::<u32>() & 0xffffff; 
        
        Ok(IbNodeInfo{lid, gid, qpn: context.qp_num(), psn})
    }
    
    pub fn serialize(&self) -> [u8; 26]{
        let lid = self.lid.to_le_bytes();
        let qpn = self.qpn.to_le_bytes();
        let psn = self.psn.to_le_bytes();
        let gid = self.gid.as_bytes();
        let mut res = [0; 26];
        for i in 0..26{
            if i < 2 {
                res[i] = lid[i];
            } else if i < 6{
                res[i] = qpn[i-2];
            } else if i< 10{
                res[i] = psn[i-6];
            } else {
                res[i] = gid[i-10];
            }
        }
        res
    }
    
    pub fn deserialize(bytes: &[u8]) -> Self{
        let lid = u16::from_le_bytes(bytes[..2].try_into().unwrap());
        let qpn = u32::from_le_bytes(bytes[2..6].try_into().unwrap());
        let psn = u32::from_le_bytes(bytes[6..10].try_into().unwrap());
        let gid = Gid::from_bytes(bytes[10..26].try_into().unwrap());
        IbNodeInfo{lid, qpn, psn, gid}
    }

    pub fn print(&self){
        let res = self.gid.to_ipv6_addr(); 
        println!("Address: LID 0x{:04x}, QPN 0x{:06x}, PSN 0x{:06x}, GID {}", self.lid, self.qpn, self.psn, res);
    }
}

