use std::net::{SocketAddrV4, UdpSocket, Ipv4Addr};
use std::str::FromStr;
use std::thread::{self, JoinHandle};

use crate::utils;





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
    for id in 1..=data.iters {
        let start = utils::get_time_ns();
        (data.send_packet)(data.base_packet, id, data.socket_addr, &mut data.socket);
        let interval = utils::get_time_ns() - start;
        if interval < data.interval {
            utils::pp_sleep(data.interval-interval); 
        }
    }
}


pub fn start_sending_packets<F>(iters: u64, interval: u64, base_packet: Option<[char; 1024]>,
    send_packet: F, socket: UdpSocket, socket_addr: Option<SocketAddrV4>) -> JoinHandle<()>
    where F: FnMut(Option<[char; 1024]>, u64, Option<SocketAddrV4>, &UdpSocket),
        F: Send +'static
        
{
    
    let sender_data = SenderData::new(iters, interval, base_packet, socket, socket_addr, send_packet);
    thread::spawn(move || thread_send_packets(sender_data)) 
}
