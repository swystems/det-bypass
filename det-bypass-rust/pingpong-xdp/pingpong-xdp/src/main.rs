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
use std::thread;
use common::persistence_agent::PersistenceAgent;



#[derive(Debug, Parser)]
struct Opt {
    #[clap(short, long, default_value = "eth0")]
    iface: String,
    #[clap(short, long)]
    server: String,
    #[clap(short, long)]
    interval: u64,
    #[clap(short, long)]
    packets: u64,
    #[clap(short, long)]
    measurament: String
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

    let persistence_flag = common::persistence_agent::pers_measurament_to_flag(&opt.measurament);
    let persistence = common::persistence_agent::PersistenceAgent::new(Some("pingpong.dat"), persistence_flag, &(opt.interval as u32));
    let index = unsafe{libc::if_nametoindex(opt.iface.as_ptr())};
    start_pingpong(index, &opt.server, opt.packets, opt.interval, &mut bpf, "map", persistence)?;
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


pub fn start_client(iters: u64, interval: u64, map_ptr: aya::maps::Array<&mut aya::maps::MapData, i32> ,  mut persistence: PersistenceAgent,
                   buf: [u8; consts::PACKET_SIZE], server_addr: libc::sockaddr_ll )-> Result<(), std::io::Error>{
    let socket = common_net::setup_raw_socket()?;    

    let th = start_sending_packets(iters, interval, socket, server_addr, buf);

    let mut current_id = 0;
    let mut next_map_idx = 0;

    while current_id < iters {
        let (mut payload, mi) = poll_next_payload (&map_ptr, next_map_idx)?;
        next_map_idx = mi;

        if payload.phase != 2 {
            eprintln!("ERR: expected phase 2, got {}", payload.phase);
            eprintln!("Packet: {} {} {} {} {}", payload.id, payload.ts[0], payload.ts[1], payload.ts[2], payload.ts[3]);
            continue;
        }


        payload.ts[3] = utils::get_time_ns ();

        persistence.write(payload);

        current_id = std::cmp::max(current_id, payload.id);
    }
    if th.join().is_err() { return utils::new_error("Couldn't join thread") };

    
    Ok(())
}




pub fn start_pingpong(ifindex: u32, server_ip: &str, iters: u64, interval: u64, loaded_xdp_obj: & mut aya::Bpf, mapname: &str, pa: common::persistence_agent::PersistenceAgent) -> Result<(), std::io::Error> {
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
    start_client(iters, interval, map_array,pa, buf, sock_addr)?;
    Ok(())
}

fn send_packets(iters: u64, interval: u64, socket: i32, server_addr: libc::sockaddr_ll, buf: [u8; consts::PACKET_SIZE]) -> Result<(), std::io::Error> {
    println!("sending packets {}", iters);
    for id in 1..= iters {
        println!("id is {id}");
        let start = utils::get_time_ns();
        send_packet(buf, id, server_addr, socket)?;
        println!("start {} now {}", start, utils::get_time_ns());
        let inter = utils::get_time_ns() - start;
        if inter < interval {
            utils::pp_sleep(interval-inter); 
        }
    }
    println!("finished sending packets");
    Ok(())
}


fn start_sending_packets(iters: u64, interval: u64, socket: i32, server_addr: libc::sockaddr_ll, buf: [u8; consts::PACKET_SIZE]) -> thread::JoinHandle<Result<(), std::io::Error>> {
    println!("start sending packets");
    
    thread::spawn(move || send_packets(iters, interval, socket, server_addr, buf))
}


pub fn send_packet(mut buf: [u8; consts::PACKET_SIZE], id: u64, server_addr: libc::sockaddr_ll, socket: i32) -> Result<(), std::io::Error>{
    unsafe{
        let ip = buf[network_types::eth::EthHdr::LEN..].as_mut_ptr() as *mut network_types::ip::Ipv4Hdr;
        (*ip).id = id as u16;
        // let payload = buf[network_types::eth::EthHdr::LEN + network_types::ip::Ipv4Hdr::LEN..].as_mut_ptr() as *mut PingPongPayload;
        let mut payload = PingPongPayload::new(id);
        payload.ts[0] = utils::get_time_ns();
        buf[network_types::eth::EthHdr::LEN+network_types::ip::Ipv4Hdr::LEN..].copy_from_slice(&payload.serialize());
        let addr = &server_addr as *const libc::sockaddr_ll as *const libc::sockaddr;
        let res = libc::sendto(socket, buf.as_ptr() as *const std::ffi::c_void, consts::PACKET_SIZE,0, addr, std::mem::size_of::<libc::sockaddr_ll>() as u32);
        if res != 0{
            return utils::new_error("Sendto failed");
        }
        
    }
    Ok(())
}

