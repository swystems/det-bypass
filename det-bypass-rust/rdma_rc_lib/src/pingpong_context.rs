use std::{fmt, io::{Error, ErrorKind} };

use rdma::{bindings, cq, device::{self, Mtu}, mr::AccessFlags, pd::ProtectionDomain, poll_cq_attr::PollCQAttr, qp::{self, ModifyOptions, QueuePairState}};


const RECEIVE_DEPTH: usize = 500;
const IB_PORT: u8 = 1;


#[derive(Debug)]
pub enum PollingError{
    Enoent(String),
    Other(String),
}

impl fmt::Display for PollingError{
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PollingError::Enoent(msg) => write!(f, "ENOENT: {msg}"),
            PollingError::Other(msg) => write!(f, "{msg}") 
        }
    }
}


impl std::error::Error for PollingError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            PollingError::Enoent(_) => None,
            PollingError::Other(_) => None,
        }
    }
}


#[derive(Clone)]
pub struct PingPongContext {
    // pending: AtomicU8,
    context: rdma::ctx::Context, 
    pub(crate) pd: rdma::pd::ProtectionDomain,
    pub(crate) recv_mr: rdma::mr::MemoryRegion,
    pub(crate) send_mr: rdma::mr::MemoryRegion,
    pub(crate) cq: rdma::cq::CompletionQueue, 
    pub(crate) qp: rdma::qp::QueuePair, 
}



impl PingPongContext{
    pub fn new (device: & rdma::device::Device, recv_buf: *mut u8, send_buf: *mut u8, recv_size: usize, send_size: usize) -> Result<Self, std::io::Error>{
        let context = match device.open(){
            Ok(ctx) => ctx,
            Err(e) => return Err(e)
        };
        let pd = match ProtectionDomain::alloc(&context){
            Ok(prot_dom) => prot_dom,
            Err(e) => return Err(e)
        };

         
        let recv_mr = unsafe {
            match rdma::mr::MemoryRegion::register(&pd, recv_buf, recv_size, AccessFlags::LOCAL_WRITE, ()){
                Ok(mr) => mr,
                Err(e) => return Err(e)
            }
        }; 
        let send_mr = unsafe {
            match rdma::mr::MemoryRegion::register(&pd, send_buf, send_size, AccessFlags::LOCAL_WRITE, ()){
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
        
        
        let mut modify_options: ModifyOptions = ModifyOptions::default();
        modify_options.qp_state(QueuePairState::Initialize);
        modify_options.pkey_index(0);
        modify_options.port_num(IB_PORT);
        modify_options.qp_access_flags(AccessFlags::empty());
        
        match qp.modify(modify_options){
            Ok(_) => (),
            Err(_) => return Err(Error::new(ErrorKind::Other, "Failed to modify QP to INIT"))
        };

        Ok(PingPongContext{context, pd, recv_mr, send_mr, cq, qp})
    }

    pub fn modify_qp(&mut self, options: qp::ModifyOptions) -> Result<(), std::io::Error> {
        self.qp.modify(options)
    }

    pub fn context(&self) -> &rdma::ctx::Context {
        &self.context
    }
    
    pub fn qp_num(&self) -> u32{
        self.qp.qp_num()
    }
    
    pub fn set_pending(&mut self, _val: u8){
        //self.pending.fetch_or(val, Ordering::Relaxed);
    }

    pub fn start_poll(&self, attr: &PollCQAttr) -> Result<(), PollingError>{
        match self.cq.start_poll(attr){
            0 => Ok(()),
            libc::ENOENT => Err(PollingError::Enoent("ENOENT encountered during poll starting.".to_string())),
            err_code => Err(PollingError::Other(format!("Encountered an error with error code {err_code}").to_string()))
        }
    }

    pub fn end_poll(&self){
        self.cq.end_poll()
    }

    pub fn next_poll(&self) -> Result<(), PollingError>{
        match self.cq.next_poll(){
            0 => Ok(()),
            libc::ENOENT => Err(PollingError::Enoent("ENOENT encountered in next poll.".to_string())),
            err_code => Err(PollingError::Other(format!("Encountered an error with error code {err_code}").to_string()))
        }
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

pub struct PPContextBuilder<'a>{
    device: &'a device::Device,
    recv_buf: Option<*mut u8>,
    recv_size: Option<usize>,
    send_buf: Option<*mut u8>,
    send_size: Option<usize>,
    use_pd: bool,
    cq_options: Option<cq::CompletionQueueOptions>,
    qp_options: Option<qp::QueuePairOptions>
}

impl <'a>PPContextBuilder<'a>{
    pub fn new(device: &'a device::Device) -> PPContextBuilder<'a>{
        PPContextBuilder{device, recv_buf: None, recv_size: None, send_buf: None, send_size:None, cq_options: None, qp_options: None, use_pd: false}
    }

    pub fn recv_buf(mut self, recv_buf: *mut u8, size: usize) -> PPContextBuilder<'a>{
        self.recv_buf = Some(recv_buf);
        self.recv_size = Some(size);
        self
    }

    pub fn send_buf(mut self, send_buf: *mut u8, size: usize) -> PPContextBuilder<'a>{
        self.send_buf = Some(send_buf);
        self.send_size = Some(size);
        self
    }

    pub fn with_cq(mut self, attr: cq::CompletionQueueOptions) -> PPContextBuilder<'a>{
        self.cq_options = Some(attr);
        self
    } 

    pub fn with_qp(mut self, attr: qp::QueuePairOptions, use_pd: bool) -> PPContextBuilder<'a>{
        self.use_pd = use_pd;
        self.qp_options = Some(attr);
        self
    }


    pub fn build(self) -> Result<PingPongContext, std::io::Error>{
         let context = match self.device.open(){
            Ok(ctx) => ctx,
            Err(e) => return Err(e)
        };
        let pd = match ProtectionDomain::alloc(&context){
            Ok(prot_dom) => prot_dom,
            Err(e) => return Err(e)
        };

         
        let recv_mr = unsafe {
            match rdma::mr::MemoryRegion::register(&pd, self.recv_buf.unwrap(), self.recv_size.unwrap(), AccessFlags::LOCAL_WRITE, ()){
                Ok(mr) => mr,
                Err(e) => return Err(e)
            }
        }; 
        let send_mr = unsafe {
            match rdma::mr::MemoryRegion::register(&pd, self.send_buf.unwrap(), self.send_size.unwrap(), AccessFlags::LOCAL_WRITE, ()){
                Ok(mr) => mr,
                Err(e) => return Err(e)
            }
        };

        let cq_options = self.cq_options.unwrap_or_default();
        let cq = match rdma::cq::CompletionQueue::create(&context, cq_options){ 
            Ok(cq) => cq,
            Err(e) => return Err(e)
        };

        let mut qp_options = self.qp_options.unwrap_or_default();
        qp_options.send_cq(&cq);
        qp_options.recv_cq(&cq);
        if self.use_pd{
            qp_options.pd(&pd);
        }
        let qp = match rdma::qp::QueuePair::create(&context, qp_options){
            Ok(qp) => qp,
            Err(_) => return Err(Error::new(ErrorKind::Other, "Couldn't create qp"))
        };

        Ok(PingPongContext{context, pd, recv_mr, send_mr, cq, qp}) 
    }
}