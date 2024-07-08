use std::io::Error;
use std::io::ErrorKind;

use rand::rngs::StdRng;
use rand::Rng;
use rand::SeedableRng;
use rdma::device;
use rdma::bindings;



pub fn ib_device_find_by_name<'a>(device_list: &'a device::DeviceList, name: &str) -> Result<Option<&'a device::Device>, std::io::Error>{
    let mut dev: Option<&rdma::device::Device> = None;
    for device in device_list.as_slice() {
        if name == device.name(){
            dev = Some(device.clone());
        }
    }
    Ok(dev)
}


struct IbNodeInfo {
    lid: u16,
    qpn: u32,
    psn: u32,
    gid: bindings::ibv_gid
}


pub fn ib_get_local_info(context: *mut bindings::ibv_context, ib_port: u8, gidx: i32, qp: bindings::ibv_qp, out: &mut IbNodeInfo, seed: u64) -> Result<(), std::io::Error>{
    let port_info: *mut bindings::ibv_port_attr = std::ptr::null_mut();
    unsafe{
        if bindings::ibv_query_port(context, ib_port, port_info) != 0{ 
            eprintln!("Couldn't get port info");
            return Err(std::io::Error::new(std::io::ErrorKind::Other, "writing to bucket failed"));
        }
    
        out.lid = (*(port_info)).lid;
        if (*port_info).link_layer as u32 != bindings::IBV_LINK_LAYER_ETHERNET && out.lid == 0{
            eprintln!("Couldn't get local LID");
            return Err(Error::new(ErrorKind::Other, "Couldn't get port info"));

        }
        if bindings::ibv_query_gid(context, ib_port, gidx, &mut out.gid) != 0{
            eprintln!("Couldn't get local GID");
            return Err(Error::new(ErrorKind::Other, "Couldn't get local GID"));
        }
        out.qpn = qp.qp_num;
        let mut r = StdRng::seed_from_u64(seed);
        out.psn = r.gen::<u32>() & 0xffffff; 
    }
    Ok(())
}


pub fn ib_print_node_info(info: &IbNodeInfo){
    unsafe {
        let res = std::net::Ipv6Addr::from(info.gid.raw);
        println!("Address: LID 0x{:04x}, QPN 0x{:06x}, PSN 0x{:06x}, GID {}", info.lid, info.qpn, info.psn, res);
    }
}

