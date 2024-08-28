use anyhow::Context;
use aya::programs::{Xdp, XdpFlags};
use aya::{include_bytes_aligned, Bpf, maps};
use aya_log::BpfLogger;
use clap::Parser;
use log::{info, warn, debug};
use libc;
use tokio::signal;
use xsk_rs;
use pingpong::PingPongPayload;


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
#[derive(Clone)]
pub struct Config {
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
pub struct XskUmemInfo {
    umem: xsk_rs::Umem,
    buffer: [u8;common::consts::PACKET_SIZE]
    // struct xsk_ring_prod fq;
    // struct xsk_ring_cons cq;
    // struct xsk_umem *umem;
    // void *buffer;
}

impl XskUmemInfo{
    pub fn new(umem: xsk_rs::Umem) -> Self{
        XskUmemInfo{umem, buffer: [0;common::consts::PACKET_SIZE]}
    }
}

#[repr(C)]
pub struct XskSocketInfo {
    pub xsk:xsk_rs::Socket,
    pub tx:xsk_rs::TxQueue,
    pub rx:xsk_rs::RxQueue,
    pub fq:xsk_rs::FillQueue,
    pub umem_frame_addr:[u64; NUM_FRAMES],
    pub umem_frame_free:usize,
    pub outstanding_tx:u32,
    pub umem:XskUmemInfo,
    // pub xsk_client_lock:std::sync::Mutex<()>
}

impl XskSocketInfo{
    pub fn new(xsk: xsk_rs::Socket,tx: xsk_rs::TxQueue, rx: xsk_rs::RxQueue, fq: xsk_rs::FillQueue, umem: XskUmemInfo) -> Self{
        XskSocketInfo{xsk, umem_frame_addr: [0u64; NUM_FRAMES],
             umem_frame_free: 0, outstanding_tx: 0, umem,
             tx, rx, fq,
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
    let mut persistence = common::persistence_agent::PersistenceAgent::new(Some("pingpong.dat"), persistence_flag, &(opt.interval as u32));
    let index = unsafe{libc::if_nametoindex(opt.iface.as_ptr())};
    let cfg = Config::new(opt.iface.clone(), index, opt.packets, opt.interval);
    let (src_mac, src_ip, dest_mac, dest_ip) = common::common_net::exchange_eth_ip_addresses(index, Some(&opt.server))?;

    let map =  get_map(&mut bpf, "xsk_map".to_string())?;
    let xsk_map: maps::XskMap<&mut maps::MapData> = maps::XskMap::try_from(map)?;

    let mut umem_info = configure_xsk_umem()?;
    let socket = xsk_configure_socket(&cfg, &mut umem_info, &opt.iface, xsk_map)?;
    let start = common::utils::get_time_ns();
    println!("Starting experiment");
    let s = std::sync::Arc::new(std::sync::Mutex::new(socket));
    initialize_client(cfg.clone(), std::sync::Arc::clone(&s), src_mac, dest_mac, src_ip, dest_ip);

    rx_and_process(cfg.xsk_poll_mode, s, opt.packets, &mut persistence)?;
    let end = common::utils::get_time_ns();
    let time_taken = end-start/ 1000000;
    println!("Experiment finished in {time_taken} milliseconds.");

    persistence.close();

    info!("Waiting for Ctrl-C...");
    signal::ctrl_c().await?;
    info!("Exiting...");

    Ok(())
}

pub fn rx_and_process(poll_mode: bool, xsk_socket:std::sync::Arc<std::sync::Mutex<XskSocketInfo>>, iters: u64, pa: &mut common::persistence_agent::PersistenceAgent) -> Result<(), std::io::Error>{
    let mut socket = xsk_socket.lock().unwrap();
    let mut fds = [libc::pollfd{fd: 0, events: 0, revents: 0}; 2];
    let nfds = 1;
    fds[0].fd = socket.xsk.fd_raw();
    fds[0].events = libc::POLLIN;
    loop{
        if poll_mode {
            let ret = unsafe{libc::poll(&mut fds as *mut libc::pollfd, nfds, -1)};
            if ret <= 0 || ret >1{
                continue;
            }
        }
        let interrupt = handle_receive_packets(&mut socket, iters, pa)?;
        if interrupt{
            break;
        }
    }
    Ok(())
}


pub fn handle_receive_packets(xsk: &mut XskSocketInfo, iters: u64, pa: &mut common::persistence_agent::PersistenceAgent) -> Result<bool, std::io::Error>{
    let mut rec = 0;
    
    let stock_frames = xsk.fq.nb_free (xsk.umem_frame_free as u32);
    if stock_frames > 0{
        let mut frames = vec![xsk_rs::FrameDesc::default();stock_frames as usize];
        for i in 0..stock_frames as usize{
            let addr = xsk_alloc_umem_frame(xsk)?;
            frames[i].set_addr(addr as usize);
        
        }
        let recvd = unsafe {xsk.fq.produce(&frames)};
        if recvd == 0{
            return Ok(false);
        }
        rec = recvd;
    }
    let mut inter= false;
    for i in 0..rec{
        unsafe{
            let ring = xsk.rx.get_raw_rx_desc(i as u32);
            let (res, interrupt) = process_packet(xsk, (*ring).addr, (*ring).len, iters, pa);
            inter = interrupt;
            if !res{
                xsk_free_umem_frame(xsk, (*ring).addr);
            }
        }
    }

    complete_tx(xsk)?;
    Ok(inter)
}



pub fn xsk_free_umem_frame(xsk: &mut XskSocketInfo, frame: u64){
    assert!(xsk.umem_frame_free < NUM_FRAMES);

    xsk.umem_frame_addr[xsk.umem_frame_free] = frame;
    xsk.umem_frame_free += 1;
}

pub fn process_packet(xsk: &mut XskSocketInfo, addr: u64, len: u32, iters: u64, pa: &mut common::persistence_agent::PersistenceAgent) -> (bool, bool){
    let receive_timestamp = common::utils::get_time_ns();
    let pkt=  unsafe{libxdp_sys::xsk_umem__get_data( xsk.umem.buffer.as_mut_ptr() as *mut libc::c_void, addr)};
    let eth_len = network_types::eth::EthHdr::LEN as u32;
    let ip_len = network_types::ip::Ipv4Hdr::LEN as u32;
    let pp_len =  PingPongPayload::LEN as u32;
    
    if len < eth_len + ip_len + pp_len{
        eprintln!("Received packet is too small");
        return (false, false);
    }
    let mut interrupt = false;
    let eth = pkt as *const network_types::eth::EthHdr;
    let payload = unsafe{pkt.offset(eth_len as isize+ ip_len as isize)} as *mut PingPongPayload;
    if unsafe{(*eth).ether_type} as u16 != common::consts::ETH_P_PINGPONG{
        eprintln!("Received a non pingpong packet");
        return (false, false);
    }

    if unsafe{*payload}.id >= iters{
        interrupt = true;
    }
    unsafe{
        (*payload).ts[3] = receive_timestamp;
         pa.write(*payload);
    }
    return (false, interrupt);
}


pub fn initialize_client(cfg: Config, socket: std::sync::Arc<std::sync::Mutex<XskSocketInfo>>, src_mac: [u8;6], dest_mac: [u8;6], src_ip: u32, dest_ip: u32 ){
    let (eth, ip) = common::common_net::get_eth_ip(src_mac, src_ip, dest_mac, dest_ip);
    start_sending_packets(cfg, eth, ip, socket);
}


pub fn start_sending_packets(cfg: Config, eth: network_types::eth::EthHdr, ip: network_types::ip::Ipv4Hdr, socket: std::sync::Arc<std::sync::Mutex<XskSocketInfo>>,){
    // let s = std::sync::Arc::new(std::sync::Mutex::new(socket));
    std::thread::spawn(move || send_packets(cfg, eth, ip, socket));
}

pub fn send_packets(cfg: Config, eth: network_types::eth::EthHdr, ip: network_types::ip::Ipv4Hdr, socket: std::sync::Arc<std::sync::Mutex<XskSocketInfo>>) -> Result<(), std::io::Error>{
    let mut socket = socket.lock().unwrap();
    for i in 1..cfg.iters{
        let start = common::utils::get_time_ns();
        client_send_pp_packet(i, ip, eth, &mut socket)?;
        let interval = common::utils::get_time_ns() - start;
        if interval< cfg.interval{
            common::utils::pp_sleep(cfg.interval-interval);
        }
    }
    Ok(())
}


pub fn client_send_pp_packet(packet_id: u64, mut ip: network_types::ip::Ipv4Hdr, eth: network_types::eth::EthHdr, socket: &mut XskSocketInfo) -> Result<(), std::io::Error>{
    ip.id = packet_id as u16;
    let mut payload = PingPongPayload::new(packet_id);
    payload.ts[0] = common::utils::get_time_ns();
    let mut packet = [0u8;common::consts::PACKET_SIZE];
    unsafe{
        let eth_ptr = &eth as *const network_types::eth::EthHdr;
        let ip_ptr = &ip as *const network_types::ip::Ipv4Hdr;
        let payload_ptr = &payload as * const PingPongPayload;
        let eth_len =  network_types::eth::EthHdr::LEN;
        let ip_len =  network_types::ip::Ipv4Hdr::LEN;
        std::ptr::copy_nonoverlapping(eth_ptr as *const u8, packet.as_mut_ptr().add(0), eth_len);
        std::ptr::copy_nonoverlapping(ip_ptr as *const u8, packet.as_mut_ptr().add(eth_len), ip_len);
        std::ptr::copy_nonoverlapping(payload_ptr as *const u8, packet.as_mut_ptr().add(eth_len+ip_len), PingPongPayload::LEN);
    }
    return xsk_send_packet(socket, packet, false, false);
}

pub fn xsk_send_packet(socket: &mut XskSocketInfo, packet: [u8;common::consts::PACKET_SIZE], is_umem_frame: bool, complete: bool)-> Result<(), std::io::Error>{
    let addr = packet.as_ptr();
    let len = packet.len();
    let mut frame_desc = xsk_rs::FrameDesc::default();
    if is_umem_frame{
        frame_desc.set_addr(addr as usize);
    } else {
        let frame_addr = &xsk_alloc_umem_frame(socket)?;
        frame_desc.set_addr(*frame_addr as usize);
    }
    frame_desc.set_length_data(len);
    let count = unsafe{socket.tx.produce_one(&frame_desc)};
    
    unsafe {
        let dst = libxdp_sys::xsk_umem__get_data( socket.umem.buffer.as_mut_ptr() as *mut libc::c_void, frame_desc.addr() as u64);
        let src = addr as *const u8;
        std::ptr::copy_nonoverlapping(src, dst as *mut u8, len);
    }
    
    if count != 1{
        return common::utils::new_error("No more transmit slots");
    }
    socket.outstanding_tx +=1;
    if complete{
        complete_tx(socket)?;
    }
    Ok(())
}

pub fn complete_tx(xsk: &mut XskSocketInfo) -> Result<(), std::io::Error>{
    if xsk.outstanding_tx != 0{
        return Ok(());
    }
    xsk.tx.wakeup()?;
    let mut frame_descs = vec![xsk_rs::FrameDesc::default(); xsk.outstanding_tx as usize];
    let cnt = unsafe {xsk.rx.consume(&mut frame_descs)};
    if cnt > 0{
        xsk.outstanding_tx -= if cnt< xsk.outstanding_tx as usize {cnt as u32} else {xsk.outstanding_tx};
    }
    Ok(())
    
}


pub fn get_map(bpf: &mut Bpf, map_name: String) -> Result<&mut aya::maps::Map, std::io::Error>{
    let map = match bpf.map_mut(&map_name){
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


pub fn configure_xsk_umem() -> Result<XskUmemInfo, std::io::Error>{
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


pub fn xsk_configure_socket(cfg: &Config, umem: &mut XskUmemInfo, 
    ifname: &str, mut xsk_map: maps::XskMap<&mut maps::MapData>) -> Result<XskSocketInfo, std::io::Error> {
    let mut builder = xsk_rs::config::SocketConfig::builder();
    builder.xdp_flags(cfg.xdp_flags);
    builder.bind_flags(cfg.xsk_bind_flags);
    builder.libxdp_flags(xsk_rs::config::LibxdpFlags::XSK_LIBXDP_FLAGS_INHIBIT_PROG_LOAD);
    let config = builder.build();
    let cname = std::ffi::CString::new(ifname).unwrap();
    let interface = xsk_rs::config::Interface::new(cname);
    let (tx, rx, fq_cq, socket) = match unsafe{xsk_rs::Socket::new(config, &umem.umem, &interface, 0)}{
        Ok(s) => s,
        Err(e) => return common::utils::new_error(format!("Error: {e}").as_str())
    };


    let _ = xsk_map.set(0, socket.fd_raw(), 0);
    
    // socket.update_xskmap(xsk_map_fd)?;
    let mut umem_frame_addr = [0u64; NUM_FRAMES]; 
    /* Initialize umem frame allocation */
    for i in   0..NUM_FRAMES{
        umem_frame_addr[i] = i as u64 * FRAME_SIZE;
    } 
  
    let (mut fq, _cq) =  match fq_cq {
        None => return common::utils::new_error("No fill queue available"),
        Some((fq, cq)) => (fq, cq)
    };
    let ret = fq.prod_reserve();
    if ret != libxdp_sys::XSK_RING_PROD__DEFAULT_NUM_DESCS{
        return common::utils::new_error("Prod reserve failed");
    }
    let mut xsk_info = XskSocketInfo::new(socket, tx, rx, fq, umem.to_owned());
    for i in 0..libxdp_sys::XSK_RING_PROD__DEFAULT_NUM_DESCS{
        let val = xsk_alloc_umem_frame(&mut xsk_info)?;
        xsk_info.fq.fill_addr(i, val)
    }
    xsk_info.fq.prod_submit(libxdp_sys::XSK_RING_PROD__DEFAULT_NUM_DESCS);
    Ok(xsk_info)
    
}


pub fn xsk_alloc_umem_frame(xsk: &mut XskSocketInfo)-> Result<u64, std::io::Error> {
    if xsk.umem_frame_free == 0{
        return common::utils::new_error("No free frames");
    }
    xsk.umem_frame_free -= 1;
    let frame = xsk.umem_frame_addr[xsk.umem_frame_free];
    xsk.umem_frame_addr[xsk.umem_frame_free] = INVALID_UMEM_FRAME;
    return Ok(frame);
}

