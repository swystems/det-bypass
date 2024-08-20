use anyhow::Context;
use aya::programs::{Xdp, XdpFlags};
use aya::{include_bytes_aligned, Bpf, maps};
use aya_log::BpfLogger;
use clap::Parser;
use log::{info, warn, debug};
use libc;
use tokio::signal;
use xsk_rs;
use std::borrow::Borrow;


const NUM_FRAMES: usize = 4096;
const XSK_UMEM__DEFAULT_FRAME_SIZE: usize = 4096;
const FRAME_SIZE: u64 = 4096;
const INVALID_UMEM_FRAME: u64 = u64::MAX;


#[derive(Debug, Parser)]
struct Opt {
    #[clap(short, long, default_value = "eth0")]
    iface: String,
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

#[repr(C)]
struct Config {
    xdp_flags: xsk_rs::config::XdpFlags,
    ifindex: u32,
    ifname: String,
    iters: u64,
    interval: u64,
    xsk_bind_flags: xsk_rs::config::BindFlags,
    xsk_if_queue: i32,
    xsk_poll_mode: bool
}

impl Config {
    pub fn new(ifname: String, ifindex: u32, iters: u64, interval: u64) -> Self{
        let xdp_flags =  xsk_rs::config::XdpFlags::XDP_FLAGS_UPDATE_IF_NOEXIST| xsk_rs::config::XdpFlags::XDP_FLAGS_DRV_MODE;
        let xsk_bind_flags = xsk_rs::config::BindFlags::XDP_ZEROCOPY;
        Config{xdp_flags, ifindex, ifname, iters, interval, xsk_bind_flags, xsk_if_queue: 0, xsk_poll_mode: false}
    }
}


#[repr(C)]
#[derive(Clone)]
struct XskUmemInfo {
    umem: xsk_rs::Umem,
    // struct xsk_ring_prod fq;
    // struct xsk_ring_cons cq;
    // struct xsk_umem *umem;
    // void *buffer;
}

impl XskUmemInfo{
    pub fn new(umem: xsk_rs::Umem) -> Self{
        XskUmemInfo{umem}
    }
}

#[repr(C)]
struct XskSocketInfo {
    xsk: xsk_rs::Socket,
    umem_frame_addr: [u64; NUM_FRAMES],
    umem_frame_free: usize,
    outstanding_tx: u32,
    umem: XskUmemInfo,
    xsk_client_lock: std::sync::Mutex<()>
}

impl XskSocketInfo{
    pub fn new(lock: std::sync::Mutex<()>, xsk: xsk_rs::Socket, umem: XskUmemInfo) -> Self{
        XskSocketInfo{xsk, umem_frame_addr: [0u64; NUM_FRAMES],
             umem_frame_free: 0, outstanding_tx: 0, umem,
             xsk_client_lock: lock
         }
        
    }
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
        "../../target/bpfel-unknown-none/debug/xsk-pingpong"
    ))?;
    #[cfg(not(debug_assertions))]
    let mut bpf = Bpf::load(include_bytes_aligned!(
        "../../target/bpfel-unknown-none/release/xsk-pingpong"
    ))?;
    if let Err(e) = BpfLogger::init(&mut bpf) {
        // This can happen if you remove all log statements from your eBPF program.
        warn!("failed to initialize eBPF logger: {}", e);
    }
    let program: &mut Xdp = bpf.program_mut("xsk_pingpong").unwrap().try_into()?;
    program.load()?;
    program.attach(&opt.iface, XdpFlags::default())
        .context("failed to attach the XDP program with default flags - try changing XdpFlags::default() to XdpFlags::SKB_MODE")?;
    
    let persistence_flag = common::persistence_agent::pers_measurament_to_flag(&opt.measurament);
    let persistence = common::persistence_agent::PersistenceAgent::new(Some("pingpong.dat"), persistence_flag, &(opt.interval as u32));
    let index = unsafe{libc::if_nametoindex(opt.iface.as_ptr())};
    let cfg = Config::new(opt.iface, index, opt.packets, opt.interval);
    let (src_mac, src_ip, dest_mac, dest_ip) = common::common_net::exchange_eth_ip_addresses(index, Some(&opt.server))?;

    let map =  get_map(&mut bpf, "xsk_map".to_string())?;
    let xsk_map: maps::XskMap<&mut maps::MapData> = maps::XskMap::try_from(map)?;
    // let a = xsk_map.set(0, )c
    // let a = xsk_map.borrow().fd();
    // let fd = xsk_map.fd();
    // let fd = xsk_map.inne
    // let xsk_map_fd = libxdp_sys::bpf_object__find_map_fd_by_name (&mut bpf, "xsk_map");


    let packet_buffer_size = NUM_FRAMES*XSK_UMEM__DEFAULT_FRAME_SIZE;
    let packet_buffer = get_packet_buffer(packet_buffer_size)?;

    let mut umem_info = configure_xsk_umem(packet_buffer, packet_buffer_size)?;
    // let socket = xsk_configure_socket(cfg, &mut umem_info, &opt.iface, xsk_map)?;

    info!("Waiting for Ctrl-C...");
    signal::ctrl_c().await?;
    info!("Exiting...");

    Ok(())
}


pub fn get_map(bpf: &mut Bpf, map_name: String) -> Result<&mut aya::maps::Map, std::io::Error>{
    let map = match bpf.map_mut("xsk_map"){
        Some(m) => m,
        None => return common::utils::new_error("a")
    };
    Ok(map)
}


pub fn get_packet_buffer(size: usize) -> Result<*mut u8, std::io::Error>{
    let alignment = unsafe { libc::sysconf(libc::_SC_PAGESIZE) } as usize;
    let layout = std::alloc::Layout::from_size_align(size, alignment)
        .expect("Invalid memory layout");
    let packet_buffer = unsafe { std::alloc::alloc(layout) };

    if packet_buffer.is_null() {
        return common::utils::new_error("Error: Can't allocate buffer memory");
    }
    Ok(packet_buffer)
}


pub fn configure_xsk_umem(buffer: *mut u8, size: usize) -> Result<XskUmemInfo, std::io::Error>{
    let fs = match xsk_rs::config::FrameSize::new(XSK_UMEM__DEFAULT_FRAME_SIZE as u32){
        Ok(fs) => fs,
        Err(e) => return common::utils::new_error(format!("Error is: {e}").as_str())
    };
    let mut builder = xsk_rs::config::UmemConfig::builder();
    builder.frame_size(fs);
    let config = match builder.build(){
        Ok(c) => c,
        Err(e) => return common::utils::new_error(format!("Error: {e}").as_str())
    };
    let (umem, _vec) = match xsk_rs::umem::Umem::new(config, std::num::NonZero::new(NUM_FRAMES as u32).unwrap(), false){
       Ok(u) => u,
       Err(e)  => return common::utils::new_error(format!("Error: {e}").as_str())  
    };
    Ok(XskUmemInfo::new(umem))
    
}


pub fn xsk_configure_socket(cfg: Config, umem: &mut XskUmemInfo, 
    ifname: &str, mut xsk_map: maps::XskMap<&mut maps::MapData>) -> Result<XskSocketInfo, std::io::Error> {
    let mut builder = xsk_rs::config::SocketConfig::builder();
    builder.xdp_flags(cfg.xdp_flags);
    builder.bind_flags(cfg.xsk_bind_flags);
    builder.libxdp_flags(xsk_rs::config::LibxdpFlags::XSK_LIBXDP_FLAGS_INHIBIT_PROG_LOAD);
    let config = builder.build();
    let cname = std::ffi::CString::new(ifname).unwrap();
    let interface = xsk_rs::config::Interface::new(cname);
    let (tx, rx, fq_cq,mut socket) = match unsafe{xsk_rs::Socket::new(config, &umem.umem, &interface, 0)}{
        Ok(s) => s,
        Err(e) => return common::utils::new_error(format!("Error: {e}").as_str())
    };


    xsk_map.set(0, socket.fd_raw(), 0);
    
    // socket.update_xskmap(xsk_map_fd)?;
    let mut umem_frame_addr = [0u64; NUM_FRAMES]; 
    /* Initialize umem frame allocation */
    for i in   0..NUM_FRAMES{
        umem_frame_addr[i] = i as u64 * FRAME_SIZE;
    } 

    
    let umem_frame_free = NUM_FRAMES;

    let (mut fq, cq) =  match fq_cq {
        None => return common::utils::new_error("No fill queue available"),
        Some((fq, cq)) => (fq, cq)
    };
    let ret = fq.prod_reserve();
    if ret != libxdp_sys::XSK_RING_PROD__DEFAULT_NUM_DESCS{
        return common::utils::new_error("Prod reserve failed");
    }
    let lock = std::sync::Mutex::new(());
    let mut xsk_info = XskSocketInfo::new(lock, socket, umem.to_owned());
    for i in 0..libxdp_sys::XSK_RING_PROD__DEFAULT_NUM_DESCS{
        let val = xsk_alloc_umem_frame(&mut xsk_info)?;
        fq.fill_addr(i, val)
    }
    fq.prod_submit(libxdp_sys::XSK_RING_PROD__DEFAULT_NUM_DESCS);
    Ok(xsk_info)
    
}


pub fn xsk_alloc_umem_frame(xsk: &mut XskSocketInfo)-> Result<u64, std::io::Error> {
    let mut frame = 0;
    if xsk.umem_frame_free == 0{
        return common::utils::new_error("No free frames");
    }
    xsk.umem_frame_free -= 1;
    frame = xsk.umem_frame_addr[xsk.umem_frame_free];
    xsk.umem_frame_addr[xsk.umem_frame_free] = INVALID_UMEM_FRAME;
    return Ok(frame);
}

