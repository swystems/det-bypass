use std::fmt;
use common::consts;
use rdma::{cq, device::{self, Mtu}, mr::AccessFlags, pd::ProtectionDomain, poll_cq_attr::PollCQAttr, qp};


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

    pub fn start_poll(&self, attr: &mut PollCQAttr) -> Result<(), PollingError>{
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
        let context = self.device.open()?;
        let pd = ProtectionDomain::alloc(&context)?;

         
        let recv_mr = unsafe {
            rdma::mr::MemoryRegion::register(&pd, self.recv_buf.unwrap(), consts::PACKET_SIZE ,AccessFlags::LOCAL_WRITE, ())?
        }; 
        let send_mr = unsafe {
            rdma::mr::MemoryRegion::register(&pd, self.send_buf.unwrap(), consts::PACKET_SIZE, AccessFlags::LOCAL_WRITE, ())?
        };

        let cq_options = self.cq_options.unwrap_or_default();
        let cq = rdma::cq::CompletionQueue::create(&context, cq_options)?; 

        let mut qp_options = self.qp_options.unwrap_or_default();
        qp_options.send_cq(&cq);
        qp_options.recv_cq(&cq);
        if self.use_pd{
            qp_options.pd(&pd);
        }
        let qp = rdma::qp::QueuePair::create(&context, qp_options)?;

        Ok(PingPongContext{context, pd, recv_mr, send_mr, cq, qp}) 
    }
}
