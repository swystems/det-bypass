use std::net::{Ipv4Addr, SocketAddrV4, UdpSocket};
use std::str::FromStr;

use anyhow::Context;
// use aya::pin::PinError;
use aya::programs::{Xdp, XdpFlags};
use aya::{include_bytes_aligned, Bpf};
use aya_log::BpfLogger;
use clap::Parser;
use log::{info, warn, debug};
use tokio::signal;
use pingpong::PingPongPayload;
use common::utils;
use common::persistence_agent;
use std::thread;

#[derive(Debug, Parser)]
struct Opt {
    #[clap(short, long, default_value = "eth0")]
    dev: String,
    #[clap(short, long)]
    remove: bool,
    #[clap(short, long)]
    packets: u64,
    #[clap(short, long)]
    interval: u64,
    #[clap(short, long)]
    server: String,
    #[clap(short, long)]
    measurament: String
    
}

const XDP_UDP_PORT: u16 = 0;



pub fn send_packet(id: u64, server_addr: &SocketAddrV4, socket: &UdpSocket) -> Result<(), std::io::Error>{
    let  mut ppp = PingPongPayload::new(id);
    ppp.set_ts_value(0, 0);
    let mut buf: [u8; 1024] = [0; 1024];
    let serialized = ppp.serialize();
    for i in 0..48{
        buf[i] = serialized[i];
    }
    socket.send_to(&buf, server_addr)?;
    Ok(())
}

pub fn thread_send_packets(socket: UdpSocket, server_addr: SocketAddrV4, iters: u64, interval: u64)-> Result<(), std::io::Error>{
    println!("sending packets {}", iters);
    for id in 1..= iters {
        let start = utils::get_time_ns();
        send_packet(id, &server_addr, &socket)?;
        let int= utils::get_time_ns() - start;
        if int< interval {
            utils::pp_sleep(interval-int); 
        }
    }
    println!("finished sending packets");
    Ok(())
}

pub fn send_packets(socket: UdpSocket, server_addr: SocketAddrV4, iters: u64, interval: u64){
    thread::spawn(move ||thread_send_packets(socket, server_addr, iters, interval));
}

pub fn receive_packets(recv_socket: &UdpSocket, iters: u64, persistence: & mut persistence_agent::PersistenceAgent)-> Result<(), std::io::Error>{
    let mut buf: [u8; 1024] = [0;1024];
    let mut curr_iter = 0;
    loop{
        if curr_iter >= iters{
            break;
        }
        let (_size, _addr) = recv_socket.recv_from(&mut buf)?;
        let payload = PingPongPayload::deserialize(&buf);
        persistence.write(payload);
        curr_iter = std::cmp::max(curr_iter, payload.id);
    }
    Ok(())
}


pub fn start_client(server_ip: &str, iters: u64, interval: u64, mut persistence:  persistence_agent::PersistenceAgent) -> Result<(), std::io::Error>{
    let socket_addr = SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, XDP_UDP_PORT);
    let socket = std::net::UdpSocket::bind(socket_addr)?;
    let ip_addr =  match Ipv4Addr::from_str(server_ip){
        Ok(e) => e,
        _ => return Err(std::io::Error::new(std::io::ErrorKind::Other, "Couldn't create address"))
    };
    let server_addr = SocketAddrV4::new(ip_addr, XDP_UDP_PORT);
    send_packets(socket.try_clone()?, server_addr, iters, interval);
    receive_packets(&socket, iters, &mut persistence)?;
    persistence.close();
    Ok(())
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
        "../../target/bpfel-unknown-none/debug/pure_client"
    ))?;
    #[cfg(not(debug_assertions))]
    let mut bpf = Bpf::load(include_bytes_aligned!(
        "../../target/bpfel-unknown-none/release/pure_client"
    ))?;
    if let Err(e) = BpfLogger::init(&mut bpf) {
        // This can happen if you remove all log statements from your eBPF program.
        warn!("failed to initialize eBPF logger: {}", e);
    }
    let program: &mut Xdp = bpf.program_mut("pure_client").unwrap().try_into()?;
    program.load()?;
    program.attach(&opt.dev, XdpFlags::SKB_MODE)
        .context("failed to attach the XDP program with default flags - try changing XdpFlags::default() to XdpFlags::SKB_MODE")?;
    let pa = persistence_agent::PersistenceAgent::new(Some("pingpong_pure.dat"), persistence_agent::pers_measurament_to_flag(&opt.measurament), &(opt.interval as u32));

    let _ = start_client(&opt.server, opt.packets, opt.interval, pa);
    info!("Waiting for Ctrl-C...");
    signal::ctrl_c().await?;
    info!("Exiting...");

    Ok(())
}
