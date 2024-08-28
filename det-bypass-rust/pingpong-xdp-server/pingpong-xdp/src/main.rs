use anyhow::Context;
use aya::programs::{Xdp, XdpFlags};
use aya::{include_bytes_aligned, Bpf};
use aya_log::BpfLogger;
use clap::Parser;
use log::{info, warn, debug};
use tokio::signal;
use common::utils;
use common::common_net;
use common::consts;
use pingpong::PingPongPayload;



#[derive(Debug, Parser)]
struct Opt {
    #[clap(short, long, default_value = "eth0")]
    iface: String,
    #[clap(short, long)]
    server: String,
    #[clap(short, long)]
    packets: u64
}

#[tokio::main]
async fn main() -> Result<(), anyhow::Error> {
    let opt = Opt::parse();

    env_logger::init();

    // Bump the memlock rlimit. This is needed for older kernels that don't use the
    // new memcg based accounting, see https://lwn.net/Articles/837122/
    let rlim = libc::rlimit {
        rlim_cur: libc::RLIM_INFINITY,
        rlim_max: libc::RLIM_INFINITY,
    };
    let ret = unsafe { libc::setrlimit(libc::RLIMIT_MEMLOCK, &rlim) };
    if ret != 0 {
        debug!("remove limit on locked memory failed, ret is: {}", ret);
    }

    // This will include your eBPF object file as raw bytes at compile-time and load it at
    // runtime. This approach is recommended for most real-world use cases. If you would
    // like to specify the eBPF program at runtime rather than at compile-time, you can
    // reach for `Bpf::load_file` instead.
    #[cfg(debug_assertions)]
    let mut bpf = Bpf::load(include_bytes_aligned!(
        "../../target/bpfel-unknown-none/debug/pingpong-xdp"
    ))?;
    #[cfg(not(debug_assertions))]
    let mut bpf = Bpf::load(include_bytes_aligned!(
        "../../target/bpfel-unknown-none/release/pingpong-xdp"
    ))?;
    if let Err(e) = BpfLogger::init(&mut bpf) {
        // This can happen if you remove all log statements from your eBPF program.
        warn!("failed to initialize eBPF logger: {}", e);
    }
    let program: &mut Xdp = bpf.program_mut("pingpong_xdp").unwrap().try_into()?;
    program.load()?;
    program.attach(&opt.iface, XdpFlags::default())
        .context("failed to attach the XDP program with default flags - try changing XdpFlags::default() to XdpFlags::SKB_MODE")?;

    let index = unsafe{libc::if_nametoindex(opt.iface.as_ptr())};
    start_pingpong(index, &opt.server, opt.packets, &mut bpf, "map")?;
     // start_pingpong(ifindex: u32, server_ip: &str, iters: u64, interval: u64, loaded_xdp_obj: & mut aya::Bpf, mapname: &str) 
    info!("Waiting for Ctrl-C...");
    signal::ctrl_c().await?;
    info!("Exiting...");

    Ok(())
}




pub fn poll_next_payload(map_ptr: &aya::maps::Array<&mut aya::maps::MapData, i32> , next_index: u32) -> Result<(PingPongPayload, u32), std::io::Error>{
    let elem = match map_ptr.get(&next_index, 0){
        Ok(a) => a,
        Err(e) => return utils::new_error(format!("Error {e}").as_str())
    };
    
    let payload = elem as *mut PingPongPayload;

    unsafe{
        common::busy_wait!(!(*payload).is_valid());
        
    
        let to_return = *payload;
        let size = std::mem::size_of::<PingPongPayload>();
        std::ptr::write_bytes(payload,0, size);
        Ok((to_return, (next_index +1) % consts::PACKETS_MAP_SIZE as u32))
    }
}


pub fn start_server(iters: u64, map_ptr: aya::maps::Array<&mut aya::maps::MapData, i32> ,
                   mut buf: [u8; consts::PACKET_SIZE], client_addr: libc::sockaddr_ll )-> Result<(), std::io::Error>{
    let socket = crate::common_net::setup_raw_socket()?;    
    let payload = buf[network_types::eth::EthHdr::LEN+network_types::ip::Ipv4Hdr::LEN..].as_mut_ptr() as *mut PingPongPayload;

    let mut current_id = 0;
    let mut next_map_index = 0;
    while current_id < iters{
        let (pl, mi) = poll_next_payload(&map_ptr, next_map_index)?;
        next_map_index = mi;
        unsafe{
        (*payload) = pl;
            if (*payload).phase != 0{
                return utils::new_error(format!("Err: expected phase 0, got {}", (*payload).phase).as_str());
            }
            (*payload).ts[1] = utils::get_time_ns();
            current_id = std::cmp::max(current_id, (*payload).id);
            (*payload).phase = 2;
            (*payload).ts[2] = utils::get_time_ns();
            let addr = &client_addr as *const libc::sockaddr_ll as *const libc::sockaddr;
            let ret = libc::sendto(socket, buf.as_ptr() as *const std::ffi::c_void, consts::PACKET_SIZE,0, addr, std::mem::size_of::<libc::sockaddr_ll>() as u32);
    
            if ret != 0{
                return utils::new_error("Sendto");
            }
            if current_id > iters{
                break;
            }
        }
    }

    
    Ok(())
}




pub fn start_pingpong(ifindex: u32, server_ip: &str, iters: u64, loaded_xdp_obj: & mut aya::Bpf, mapname: &str) -> Result<(), std::io::Error> {
    println!("Exchanging addresses..");
    let (src_mac, src_ip, dest_mac, dest_ip) = crate::common_net::exchange_eth_ip_addresses(ifindex, Some(server_ip))?;
    println!("Ok");
    let map_ptr = match loaded_xdp_obj.map_mut(mapname){
        Some(m) => m,
        None => return utils::new_error(format!("Couldn't find map named {}", mapname).as_str())
    };
    let map_array: aya::maps::Array<&mut aya::maps::MapData, i32> = match aya::maps::Array::try_from(map_ptr){
        Ok(m) => m,
        Err(e) => return utils::new_error(format!("Error: {e}").as_str())
    };
    let buf: [u8; consts::PACKET_SIZE] = common_net::build_base_packet(src_mac,src_ip ,dest_mac ,dest_ip);
    let sock_addr = common_net::build_sockaddr(ifindex as i32, dest_mac);
    start_server(iters, map_array, buf, sock_addr)?;
    Ok(())
}

