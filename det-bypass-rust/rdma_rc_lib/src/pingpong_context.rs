use std::{alloc::Layout, io::{Error, ErrorKind}, sync::atomic::AtomicU8};

use std::alloc;
use common::persistence_agent::PingPongPayload;
use rdma::{bindings, mr::AccessFlags, pd::ProtectionDomain};


const PACKET_SIZE: usize = 1024;
const RECEIVE_DEPTH: usize = 500;


union PingPongContextUnion {
    buf: *mut u8,
    payload: std::mem::ManuallyDrop<PingPongPayload>
}

struct PingpongContext {
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
    qpx: bindings::ibv_qp_ex
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

        //let recv_buf = PingPongPayload::new_empty();
        //let send_buf = PingPongPayload::new_empty();
        let mut context = match device.open(){
            Ok(ctx) => ctx,
            Err(e) => return Err(e)
        };
        let pd = match ProtectionDomain::alloc(&context){
            Ok(prot_dom) => prot_dom,
            Err(e) => return Err(e)
        };

        let attrx: rdma::device::DeviceAttr = match rdma::device::DeviceAttr::query(&context){
            Ok(att) => att,
            Err(e) => return Err(Error::new(ErrorKind::Other, "Device doesn't support completion timestamping"))
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

        let options = rdma::cq::CompletionQueue::options();
        options.cqe(RECEIVE_DEPTH+1);
        options.wc_flags(bindings::IBV_WC_EX_WITH_COMPLETION_TIMESTAMP as u64);

        let cq = match rdma::cq::CompletionQueue::create(&context, options){ 
            Ok(cq) => cq,
            Err(e) => return Err(e)
        };
        
        let qp_options: rdma::qp::QueuePairOptions = rdma::qp::QueuePair::options();
        qp_options.send_cq(&cq);
        qp_options.recv_cq(&cq);
        qp_options.cap(rdma::qp::QueuePairCapacity{max_send_wr: 1, max_recv_wr: RECEIVE_DEPTH as u32, max_send_sge: 1, max_recv_sge: 1, max_inline_data: 0});
        qp_options.qp_type(rdma::qp::QueuePairType::RC);
        qp_options.comp_mask(bindings::IBV_QP_INIT_ATTR_PD | bindings::IBV_QP_INIT_ATTR_SEND_OPS_FLAGS);
        qp_options.pd(&pd);
        qp_options.send_ops_flags(bindings::IBV_QP_EX_WITH_SEND);
        let qp = match rdma::qp::QueuePair::create(&context, qp_options){
            Ok(qp) => qp,
            Err(e) => return Err(Error::new(ErrorKind::Other, "Couldn't create qp"))
        };
        
        Ok(PingpongContext{
            pending: 0.into(), send_flags: bindings::IBV_SEND_SIGNALED,
            recv_union: PingPongContextUnion{buf: recv_buf}, send_union: PingPongContextUnion{buf: send_buf},
            context, pd, completion_timestamp_mask, recv_mr, send_mr, cq, qp 
        })
    }
}
