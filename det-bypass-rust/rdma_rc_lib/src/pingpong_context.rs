use std::{alloc::Layout, io::{Error, ErrorKind}, sync::atomic::{AtomicU8, Ordering}};

use std::alloc;
use common::persistence_agent::PingPongPayload;
use rdma::{ah::AddressHandleOptions, bindings, device::Mtu, mr::AccessFlags, pd::ProtectionDomain, qp::{ModifyOptions, QueuePairState}, wr::{RecvRequest, Sge}};
use crate::ib_net::IbNodeInfo; 


const PACKET_SIZE: usize = 1024;
const RECEIVE_DEPTH: usize = 500;
const IB_PORT: u8 = 1;
const PINGPONG_RECV_WRID: u64 = 1;
const PINGPONG_SEND_WRID: u64 = 2;


union PingPongContextUnion {
    buf: *mut u8,
    payload: std::mem::ManuallyDrop<PingPongPayload>
}

pub struct PingpongContext {
    pending: AtomicU8,
    send_flags: u32,
    recv_union: PingPongContextUnion,
    send_union: PingPongContextUnion,
    context: rdma::ctx::Context, 
    pd: rdma::pd::ProtectionDomain,
    completion_timestamp_mask: u64,
    recv_mr: rdma::mr::MemoryRegion,
    send_mr: rdma::mr::MemoryRegion,
    cq: rdma::cq::CompletionQueue, 
    qp: rdma::qp::QueuePair, 
    qpx: rdma::qp_ex::QueuePairEx 
}

impl Drop for PingpongContext{
    fn drop(&mut self){
        unsafe{
            std::mem::ManuallyDrop::drop(&mut self.recv_union.payload);
            std::mem::ManuallyDrop::drop(&mut self.send_union.payload);
            let layout = Layout::from_size_align(PACKET_SIZE, std::mem::align_of::<u8>()).unwrap();
            alloc::dealloc(self.recv_union.buf, layout);
            alloc::dealloc(self.send_union.buf, layout);
        }
    }
}


impl PingpongContext{
    pub fn new (device: & mut rdma::device::Device) -> Result<Self, std::io::Error>{
        let layout = Layout::from_size_align(PACKET_SIZE, std::mem::align_of::<u8>()).unwrap();
        let recv_buf: *mut u8 = unsafe { 
            alloc::alloc(layout) 
        };
        if recv_buf.is_null(){
            return Err(Error::new(ErrorKind::Other, "Couldn't allocate memory for recv_buf"));
        } 
        let send_buf: *mut u8 = unsafe { 
            alloc::alloc(layout) 
        };
        if send_buf.is_null(){
            return Err(Error::new(ErrorKind::Other, "Couldn't allocate memory for send_buf"));
        } 

        let context = match device.open(){
            Ok(ctx) => ctx,
            Err(e) => return Err(e)
        };
        let pd = match ProtectionDomain::alloc(&context){
            Ok(prot_dom) => prot_dom,
            Err(e) => return Err(e)
        };

        let attrx: rdma::device::DeviceAttr = match rdma::device::DeviceAttr::query(&context){
            Ok(att) => att,
            Err(_) => return Err(Error::new(ErrorKind::Other, "Device doesn't support completion timestamping"))
        }; 
        let completion_timestamp_mask = attrx.completion_timestamp_mask();
        let recv_mr = unsafe {
            match rdma::mr::MemoryRegion::register(&pd, recv_buf, PACKET_SIZE, AccessFlags::LOCAL_WRITE, ()){
                Ok(mr) => mr,
                Err(e) => return Err(e)
            }
        }; 
        let send_mr = unsafe {
            match rdma::mr::MemoryRegion::register(&pd, send_buf, PACKET_SIZE, AccessFlags::LOCAL_WRITE, ()){
                Ok(mr) => mr,
                Err(e) => return Err(e)
            }
        };

        let mut options = rdma::cq::CompletionQueue::options();
        options.cqe(RECEIVE_DEPTH+1);
        options.wc_flags(bindings::IBV_WC_EX_WITH_COMPLETION_TIMESTAMP as u64);

        let cq = match rdma::cq::CompletionQueue::create(&context, options){ 
            Ok(cq) => cq,
            Err(e) => return Err(e)
        };
        
        let mut qp_options: rdma::qp::QueuePairOptions = rdma::qp::QueuePair::options();
        qp_options.send_cq(&cq);
        qp_options.recv_cq(&cq);
        qp_options.cap(rdma::qp::QueuePairCapacity{max_send_wr: 1, max_recv_wr: RECEIVE_DEPTH as u32, max_send_sge: 1, max_recv_sge: 1, max_inline_data: 0});
        qp_options.qp_type(rdma::qp::QueuePairType::RC);
        qp_options.comp_mask(bindings::IBV_QP_INIT_ATTR_PD | bindings::IBV_QP_INIT_ATTR_SEND_OPS_FLAGS);
        qp_options.pd(&pd);
        qp_options.send_ops_flags(bindings::IBV_QP_EX_WITH_SEND);
        let qp = match rdma::qp::QueuePair::create(&context, qp_options){
            Ok(qp) => qp,
            Err(_) => return Err(Error::new(ErrorKind::Other, "Couldn't create qp"))
        };
        
        let qpx = match qp.to_qp_ex() {
                Ok(qpx) => qpx,
                Err(_) => return Err(Error::new(ErrorKind::Other, "Couldn't create qp_ex from qp"))
        };
        
        let mut modify_options: ModifyOptions = ModifyOptions::default();
        modify_options.qp_state(QueuePairState::Initialize);
        modify_options.pkey_index(0);
        modify_options.port_num(IB_PORT);
        modify_options.qp_access_flags(AccessFlags::empty());
        
        match qp.modify(modify_options){
            Ok(_) => (),
            Err(_) => return Err(Error::new(ErrorKind::Other, "Failed to modify QP to INIT"))
        };

        Ok(PingpongContext{
            pending: 0.into(), send_flags: bindings::IBV_SEND_SIGNALED,
            recv_union: PingPongContextUnion{buf: recv_buf}, send_union: PingPongContextUnion{buf: send_buf},
            context, pd, completion_timestamp_mask, recv_mr, send_mr, cq, qp, qpx 
        })
    }

    pub fn send_payload(&mut self, payload: PingPongPayload){
        unsafe{
            *self.send_union.payload = payload;
        }
    }

    pub fn pp_ib_connect(&self, port: u8, local_psn: u32, mtu: Mtu, sl: u8, dest: &IbNodeInfo, gid_idx: u8) -> Result<(), Error>{
        let mut modify_options = ModifyOptions::default();
        modify_options.qp_state(QueuePairState::ReadyToReceive);
        modify_options.path_mtu(mtu);
        modify_options.dest_qp_num(dest.qpn);
        modify_options.rq_psn(dest.psn);
        modify_options.max_dest_rd_atomic(1);
        modify_options.min_rnr_timer(12);
        let mut ah_option = AddressHandleOptions::default();
        ah_option.dest_lid(dest.lid);
        ah_option.service_level(sl);
        ah_option.port_num(port);
        if dest.gid.interface_id() != 0 {
            ah_option.global_route_header(rdma::ah::GlobalRoute { dest_gid: dest.gid, flow_label: 0,
                sgid_index: gid_idx, hop_limit: 1, traffic_class: 0 });
        }
        modify_options.ah_attr(ah_option);
        match self.qp.modify(modify_options){
            Ok(_) => (),
            Err(_) => return Err(Error::new(ErrorKind::Other, "Failed to modify QP to RTR"))
        };

        let mut modify_options = ModifyOptions::default(); 
        modify_options.qp_state(QueuePairState::ReadyToSend);
        modify_options.timeout(31);
        modify_options.retry_cnt(7);
        modify_options.rnr_retry(7);
        modify_options.sq_psn(local_psn);
        modify_options.max_rd_atomic(1);
         match self.qp.modify(modify_options){
            Ok(_) => (),
            Err(_) => return Err(Error::new(ErrorKind::Other, "Failed to modify QP to RTS"))
        };
        Ok(())
    }

    pub fn post_recv(&self, n: u32) -> Result<(), Error>{
        let sge = unsafe{Sge{addr: self.recv_union.buf as u64, length: PACKET_SIZE as u32, lkey: self.recv_mr.lkey()}}; 
        let mut wr = RecvRequest::zeroed();
        wr.id(PINGPONG_RECV_WRID);
        wr.sg_list(&[sge]);

        for i in 0..n{
            unsafe{
                if let Ok(()) = self.qp.post_recv(&wr) {
                    println!("Posted {i} receives");
                    break
                }
            }   
        }
        
        Ok(())
    }
    

    pub fn post_send(&mut self) -> Result<(), Error>{
        self.qpx.start_wr(); 
        self.qpx.wr_id(PINGPONG_SEND_WRID);
        self.qpx.wr_flags(self.send_flags);
        let _ = self.qpx.post_send();
        
        let buf = unsafe{self.send_union.buf};
        self.qpx.set_sge(self.send_mr.lkey(), buf as u64, PACKET_SIZE as u32);
        match self.qpx.wr_complete(){
            Ok(()) => (),
            Err(e) => return Err(e)
        };
        self.pending.fetch_or(PINGPONG_SEND_WRID.try_into().unwrap(), Ordering::Relaxed);

       Ok(()) 
    }
}

pub fn u32_to_mtu(mtu: u32) -> Option<Mtu>{
    match mtu {
        1 => Some(Mtu::Mtu256),
        2 => Some(Mtu::Mtu512),
        3 => Some(Mtu::Mtu1024),
        4 => Some(Mtu::Mtu2048),
        5 => Some(Mtu::Mtu4096),
        _ => None
    }
}

