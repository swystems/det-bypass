use std::net::{self, Ipv4Addr, SocketAddrV4};

use common::persistence_agent;
use common::persistence_agent::PersistenceAgent;
use common::persistence_agent::PingPongPayload;

mod common_net;
mod utils;


const XDP_UPD_PORT: u16 = 1234;
const PACKET_SIZE: usize = 1024;


pub fn run_client(iters: u64, interval: u64, server_ip: &str, persistence_flag: &str){
    let persistence_flag = persistence_agent::pers_measurament_to_flag(persistence_flag);

    let mut persistence_agent = persistence_agent::PersistenceAgent::new(Some("no-bypass.dat"), persistence_flag, &(interval as u32));
    
    start_client(iters, interval, server_ip, &mut persistence_agent);
    persistence_agent.close();
}


fn send_single_packet (_: Option<[char;PACKET_SIZE]> , packet_idx: u64, socket_addr: Option<SocketAddrV4> ,socket: &net::UdpSocket) {
    let mut payload = PingPongPayload::new(packet_idx);
    payload.set_ts_value(0, utils::get_time_ns());
    let res = socket.send_to(&payload.serialize(), socket_addr.unwrap());
    println!("sending packet");
    if let Err(e) = res {
        panic!("Couldn't send payload, got error: {e}")
    }
}

fn start_client(iters: u64, interval: u64, server_ip: &str, persistence_agent: &mut PersistenceAgent){
    // the ip "0.0.0.0" means to bind to any available ip
    let  socket = net::UdpSocket::bind("0.0.0.0:0").unwrap();
    let socket_addr = common_net::new_sockaddr(server_ip, XDP_UPD_PORT);
    common_net::start_sending_packets(iters, interval, None, send_single_packet , socket.try_clone().unwrap(), Some(socket_addr));
    let mut recv_buf = [0; PACKET_SIZE];
    let mut last_idx: u64 = 0;
    while last_idx < iters {
        println!("before");
        let res = socket.recv_from(&mut recv_buf);
        println!("after");
        match res {
            Ok(r) => r,
            Err(e) => panic!("{e}")
        }; 
        let mut payload = PingPongPayload::deserialize(&recv_buf);
        payload.set_ts_value(3, utils::get_time_ns());
        last_idx = u64::max(last_idx, payload.id); 
        persistence_agent.write(payload); 
    }
}

pub fn run_server(iters: u64) {
    let socket_addr = SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, XDP_UPD_PORT);
    let socket = net::UdpSocket::bind(socket_addr).unwrap();
    
    let mut last_idx: u64 = 0;
    let mut recv_buf = [0; PACKET_SIZE];
    while last_idx < iters {
        let (_, client_addr) = match socket.recv_from(&mut recv_buf){
            Ok((s, sa)) => (s, sa),
            Err(e) => panic!("{e}")
        };
        let ts = utils::get_time_ns();
        let mut payload = PingPongPayload::deserialize(&recv_buf);
        payload.set_ts_value(1, ts);
        last_idx = u64::max(last_idx, payload.id);
        payload.set_ts_value(2, utils::get_time_ns());
        let res = socket.send_to(&payload.serialize(), client_addr);
        match res {
            Ok(_) => (),
            Err(e) => panic!("{e}")
        };
    }
    
}

