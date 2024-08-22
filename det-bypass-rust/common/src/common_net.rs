use std::io::Read;
use std::mem::offset_of;
use std::net::{SocketAddrV4, UdpSocket, Ipv4Addr};
use std::str::FromStr;
use std::thread::{self, JoinHandle};


use crate::consts;
use crate::utils::{self, new_error};


pub fn setup_raw_socket() -> Result<i32, std::io::Error>{
    unsafe{
        let sock = libc::socket (libc::AF_PACKET, libc::SOCK_RAW, libc::IPPROTO_RAW);
        if sock < 0
        {
            return new_error("Couldn't create raw socket");
        }

        Ok(sock)
    }
}



struct SenderData<F> 
    where F: FnMut(Option<[char; 1024]>, u64, Option<SocketAddrV4>, &UdpSocket),
        F: Send +'static
    {
    iters: u64,
    interval: u64,
    base_packet: Option<[char; 1024]>,
    socket:  UdpSocket,
    socket_addr: Option<SocketAddrV4>,
    send_packet: F 

}

impl <F> SenderData<F>
    where F: FnMut(Option<[char; 1024]>, u64, Option<SocketAddrV4>, &UdpSocket) +Send +'static
{
    pub fn new(iters: u64, interval: u64, base_packet: Option<[char; 1024]>,
        socket:  UdpSocket, socket_addr: Option<SocketAddrV4>, send_packet: F) -> SenderData<F>
        where F: FnMut(Option<[char; 1024]>, u64, Option<SocketAddrV4>, &UdpSocket) + Send +'static
    {
        SenderData{iters, interval, base_packet, socket, socket_addr, send_packet}
    }
}


pub fn new_sockaddr(ip: &str, port: u16) -> SocketAddrV4{
    let ip_addr = Ipv4Addr::from_str(ip).unwrap_or_else(|err| {
        eprintln!("Invalid IP address: {}", err);
        std::process::exit(1);
    });
    SocketAddrV4::new(ip_addr, port)
}


fn thread_send_packets<F>(mut data: SenderData<F>)
    where F: FnMut(Option<[char; 1024]>, u64, Option<SocketAddrV4>, & UdpSocket) + Send +'static
{
    println!("sending packets {}", data.iters);
    for id in 1..=data.iters {
        println!("id is {id}");
        let start = utils::get_time_ns();
        (data.send_packet)(data.base_packet, id, data.socket_addr, &mut data.socket);
        println!("start {} now {}", start, utils::get_time_ns());
        let interval = utils::get_time_ns() - start;
        if interval < data.interval {
            utils::pp_sleep(data.interval-interval); 
        }
    }
    println!("finished sending packets");
}


pub fn start_sending_packets<F>(iters: u64, interval: u64, base_packet: Option<[char; 1024]>,
    send_packet: F, socket: UdpSocket, socket_addr: Option<SocketAddrV4>) -> JoinHandle<()>
    where F: FnMut(Option<[char; 1024]>, u64, Option<SocketAddrV4>, &UdpSocket),
        F: Send +'static
        
{
    println!("start sending packets");
    
    let sender_data = SenderData::new(iters, interval, base_packet, socket, socket_addr, send_packet);
    thread::spawn(move || thread_send_packets(sender_data)) 
}


pub fn exchange_data(server_ip: Option<&str>, buffer: &[u8]) -> Result<(usize, [u8;1024]), std::io::Error>{
    let port = if server_ip.is_none() {1234} else {1235};
    let socket_addr = SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, port);
    let socket = std::net::UdpSocket::bind(socket_addr)?;
    let mut out_buffer = [0; 1024];
    match server_ip{
        Some(sip) => {
            let server_addr = new_sockaddr(sip, 1234);
            socket.send_to(buffer, server_addr)?;
            let (size, _) = socket.recv_from(&mut out_buffer)?;
            Ok((size, out_buffer))
        }
        None => {
            println!("before receive");
            let (size, client_addr) = socket.recv_from(&mut out_buffer)?;
            socket.send_to(buffer, client_addr)?; 
            Ok((size, out_buffer))
        }
    }
}


pub fn retrieve_local_ip(ifindex: u32) -> Result<u32, std::io::Error>{
    unsafe{
        let mut ifname: [u8; libc::IF_NAMESIZE] = [0; libc::IF_NAMESIZE];
        if libc::if_indextoname (ifindex, ifname.as_mut_ptr()).is_null() {
            return new_error("Couldn't resolve interface name");
        }

        let sock = libc::socket (libc::AF_INET, libc::SOCK_DGRAM, 0);
        if sock < 0 {
            return new_error("Socket error");
        }

        let mut ifr: libc::ifreq = std::mem::zeroed();
        for (i, &b) in ifname.iter().enumerate() {
            ifr.ifr_name[i] = b as u8;
        }

        let ret = libc::ioctl (sock, libc::SIOCGIFADDR, &ifr);
        if ret < 0 {
            return new_error("Ioctl");
        }

        libc::close(sock);
        let addr = &ifr.ifr_ifru.ifru_addr as *const libc::sockaddr as *const libc::sockaddr_in;
        Ok(u32::from_be((*addr).sin_addr.s_addr))
    }
}

pub fn retrieve_local_mac(ifindex: u32) -> Result<[u8;6], std::io::Error>{
    unsafe{
        let mut ifname: [u8; libc::IF_NAMESIZE] = [0; libc::IF_NAMESIZE];
        if libc::if_indextoname (ifindex, ifname.as_mut_ptr()).is_null() {
            return new_error("Couldn't resolve interface name");
        }
        let ifname= match std::ffi::CStr::from_ptr(ifname.as_ptr() as *const u8).to_str() {
            Ok(name) => name,
            Err(e) => return new_error(format!("Error: {}", e).as_str())
        };
        
        let path = format!("/sys/class/net/{}/address", ifname);

        let mut mac_str = String::new();
        std::fs::File::open(&path)?.read_to_string(&mut mac_str)?;
        let mut mac = [0u8; 6];
        println!("Mac {}", mac_str);
        let parts: Vec<&str> = mac_str.trim().split(':').collect();
        if parts.len() == 6 {
            for (i, part) in parts.iter().enumerate() {
                mac[i] = match u8::from_str_radix(part, 16){
                    Ok(p) => p,
                    Err(e) => return new_error(format!("Error {} ", e).as_str())
                };
            }
        } else {
            return new_error("Error sscanf");
        }
        Ok(mac)
    }
}

pub fn exchange_eth_ip_addresses(ifindex: u32, server_ip: Option<&str>) -> Result<([u8;6], u32, [u8; 6], u32), std::io::Error>{
    let src_mac = retrieve_local_mac(ifindex)?;
    let src_ip = retrieve_local_ip(ifindex)?;
    let mut buffer = [0u8; consts::ETH_IP_INFO_PACKET_SIZE];
    const ETH_ALEN: usize = 6;
    buffer[..ETH_ALEN].copy_from_slice(&src_mac[..]);
    buffer[ETH_ALEN..].copy_from_slice(&src_ip.to_le_bytes());

    let (_size, out_buffer )= exchange_data(server_ip, &buffer)?;

    let dest_mac: [u8; 6] = match out_buffer[..ETH_ALEN].try_into(){
        Ok(v) => v,
        Err(e) => return new_error(format!("Error: {}", e).as_str())
    };
    let dest_ip_bytes: [u8;4] =  match out_buffer[ETH_ALEN..ETH_ALEN+4].try_into(){
        Ok(v) => v,
        Err(e) => return new_error(format!("Error: {}", e).as_str())
    };
    let dest_ip = u32::from_be_bytes(dest_ip_bytes);

    Ok((src_mac, src_ip, dest_mac, dest_ip))
}


pub fn get_eth_ip(src_mac: [u8; 6], src_ip: u32, dest_mac: [u8;6], dest_ip: u32) -> (network_types::eth::EthHdr, network_types::ip::Ipv4Hdr){
    let eth = network_types::eth::EthHdr{dst_addr: dest_mac, src_addr: src_mac, ether_type: network_types::eth::EtherType::Loop};
    unsafe{
        let eth_ptr = &eth as *const network_types::eth::EthHdr;
        let ether_type_ptr = eth_ptr.cast::<u8>().add(offset_of!(network_types::eth::EthHdr, ether_type)) as *mut u16;
        *ether_type_ptr = consts::ETH_P_PINGPONG;
    }
    let ip= network_types::ip::Ipv4Hdr{
        _bitfield_align_1: [],
        _bitfield_1: network_types::bitfield::BitfieldUnit::new([0;1]),
        tos: 0,
        tot_len: 0,
        id:0,
        frag_off:0,
        ttl: 64,
        proto: network_types::ip::IpProto::Reserved,
        check: 0,
        src_addr: src_ip,
        dst_addr: dest_ip
    };
    (eth, ip)
}


pub fn build_base_packet(src_mac: [u8; 6], src_ip: u32, dest_mac: [u8;6], dest_ip: u32) -> [u8;consts::PACKET_SIZE]{
    let mut buf = [0u8; consts::PACKET_SIZE];    
    // eth packet
    let eth = buf[..network_types::eth::EthHdr::LEN].as_mut_ptr() as *mut  network_types::eth::EthHdr;
    unsafe{
        (*eth).dst_addr = dest_mac;
        (*eth).src_addr = src_mac;
        let ether_type_ptr = eth.cast::<u8>().add(offset_of!(network_types::eth::EthHdr, ether_type)) as *mut u16;
        *ether_type_ptr = consts::ETH_P_PINGPONG;
    }
    let ip = buf[network_types::eth::EthHdr::LEN..].as_mut_ptr() as *mut network_types::ip::Ipv4Hdr;
    unsafe{
        (*ip).set_ihl(5);
        (*ip).set_version(4);
        (*ip).tos = 0;
        (*ip).tot_len = consts::PACKET_SIZE as u16 - network_types::eth::EthHdr::LEN as u16;
        (*ip).id = 0;
        (*ip).frag_off = 0;
        (*ip).ttl = 64;
        (*ip).proto = network_types::ip::IpProto::Reserved;
        (*ip).check = 0;
        (*ip).src_addr= src_ip;
        (*ip).dst_addr= dest_ip;
    }
    buf
}


pub fn build_sockaddr(ifindex: i32, dest_mac: [u8;6]) -> libc::sockaddr_ll{
    let mut sock_addr: libc::sockaddr_ll = unsafe { std::mem::zeroed() };

    sock_addr.sll_ifindex = ifindex;
    sock_addr.sll_halen = 6;

    for i in 0..6 {
        sock_addr.sll_addr[i] = dest_mac[i];
    }

    sock_addr
}

