
use common::{consts, persistence_agent::{self, PingPongPayload}, utils};
use rdma::{ah, bindings, device, mr, poll_cq_attr, qp, qp_ex, wr};

use crate::{ib_net, pingpong_context::{self, PingPongContext}};
use crate::post_context;
use crate::post_context::PostContext;
use libc;

const PACKET_SIZE: usize = 1024;
const RECEIVE_DEPTH: usize = 500;
const PINGPONG_RECV_WRID: u64 = 1;
const PINGPONG_SEND_WRID: u64 = 2;
const IB_PORT: u8 = 1;


union PingPongContextUnion {
    buf: *mut libc::c_void,
    payload: std::mem::ManuallyDrop<PingPongPayload>
}

impl Clone for PingPongContextUnion{
    fn clone(&self) -> Self{
        unsafe{
            PingPongContextUnion{buf: self.buf}
        }
    }
}

unsafe impl Send for PingPongContextUnion{}
unsafe impl Sync for PingPongContextUnion{}



#[derive(Clone)]
pub struct RCContext{
    base_context: PingPongContext,
    //pending: AtomicU8,
    send_flags: u32,
    _completion_timestamp_mask: u64,
    qpx: qp_ex::QueuePairEx,
    recv_union: PingPongContextUnion,
    send_union: PingPongContextUnion
}

impl RCContext{
    pub fn new(device: &device::Device)-> Result<Self, std::io::Error>{
        // let layout = std::alloc::Layout::from_size_align(PACKET_SIZE, std::mem::align_of::<u8>()).unwrap();
        // let recv_buf: *mut u8 = unsafe { 
        //     std::alloc::alloc(layout) 
        // };
        // if recv_buf.is_null(){
        //     return utils::new_error("Couldn't allocate memory for recv_buf");
        // } 
        // let send_buf: *mut u8 = unsafe { 
        //     std::alloc::alloc(layout) 
        // };
        // if send_buf.is_null(){
        //     return utils::new_error("Couldn't allocate memory for send_buf");
        // }
        
        let recv_buf = unsafe{ libc::malloc(consts::PACKET_SIZE)};
        let send_buf = unsafe{libc::malloc(consts::PACKET_SIZE)};

        let mut cq_options = rdma::cq::CompletionQueue::options();
        cq_options.cqe(RECEIVE_DEPTH+1);
        cq_options.wc_flags(bindings::IBV_WC_EX_WITH_COMPLETION_TIMESTAMP as u64);
        let mut qp_options: rdma::qp::QueuePairOptions = rdma::qp::QueuePair::options();
        qp_options.cap(rdma::qp::QueuePairCapacity{max_send_wr: 1, max_recv_wr: RECEIVE_DEPTH as u32, max_send_sge: 1, max_recv_sge: 1, max_inline_data: 0});
        qp_options.qp_type(rdma::qp::QueuePairType::RC);
        qp_options.comp_mask(bindings::IBV_QP_INIT_ATTR_PD | bindings::IBV_QP_INIT_ATTR_SEND_OPS_FLAGS);
        qp_options.send_ops_flags(bindings::IBV_QP_EX_WITH_SEND);


        let builder = pingpong_context::PPContextBuilder::new(device)
            .recv_buf(recv_buf, PACKET_SIZE)
            .send_buf(send_buf, PACKET_SIZE)
            .with_cq(cq_options)
            .with_qp(qp_options, true);

        let mut base_context = builder.build()?;
        let mut modify_options: qp::ModifyOptions = qp::ModifyOptions::default();
        modify_options.qp_state(qp::QueuePairState::Initialize);
        modify_options.pkey_index(0);
        modify_options.port_num(IB_PORT);
        modify_options.qp_access_flags(mr::AccessFlags::empty());

        let qpx = base_context.qp.to_qp_ex()?;
        base_context.modify_qp(modify_options)?;
        //let pending = 0.into(); 
        let send_flags = bindings::IBV_SEND_SIGNALED;
        let context = device.open()?;
        let attrx: rdma::device::DeviceAttr = match rdma::device::DeviceAttr::query(&context){
            Ok(att) => att,
            Err(_) => return utils::new_error("Device doesn't support completion timestamping") 
        }; 
        let completion_timestamp_mask = attrx.completion_timestamp_mask();
        
       
        Ok(RCContext{
            base_context,  send_flags, _completion_timestamp_mask: completion_timestamp_mask,
            qpx, recv_union: PingPongContextUnion{buf: recv_buf},
            send_union: PingPongContextUnion{buf: send_buf}
        })
    }

    
    
    // pub fn context(&self) -> &ctx::Context{
    //     return self.base_context.context();
    // }
    
    // pub fn qp_num(&self) -> u32{
    //     self.base_context.qp_num()
    // }
    
    pub fn base_context(&self) -> &PingPongContext{
        &self.base_context
    }
    

    pub fn pp_ib_connect(&self, port: u8, local_psn: u32, mtu: device::Mtu, sl: u8, dest: &ib_net::IbNodeInfo, gid_idx: u8) -> Result<(), std::io::Error>{
        let mut modify_options = qp::ModifyOptions::default();
        modify_options.qp_state(qp::QueuePairState::ReadyToReceive);
        modify_options.path_mtu(mtu);
        modify_options.dest_qp_num(dest.qpn);
        modify_options.rq_psn(dest.psn);
        modify_options.max_dest_rd_atomic(1);
        modify_options.min_rnr_timer(12);
        let mut ah_option = ah::AddressHandleOptions::default();
        ah_option.dest_lid(dest.lid);
        ah_option.service_level(sl);
        ah_option.port_num(port);
        if dest.gid.interface_id() != 0 {
            ah_option.global_route_header(rdma::ah::GlobalRoute { dest_gid: dest.gid, flow_label: 0,
                sgid_index: gid_idx, hop_limit: 1, traffic_class: 0 });
        }
        modify_options.ah_attr(ah_option);
        match self.base_context.qp.modify(modify_options){
            Ok(_) => (),
            Err(_) => return utils::new_error("Failed to modify QP to RTR")
        };

        let mut modify_options = qp::ModifyOptions::default(); 
        modify_options.qp_state(qp::QueuePairState::ReadyToSend);
        modify_options.timeout(31);
        modify_options.retry_cnt(7);
        modify_options.rnr_retry(7);
        modify_options.sq_psn(local_psn);
        modify_options.max_rd_atomic(1);
         match self.base_context.qp.modify(modify_options){
            Ok(_) => (),
            Err(_) => return utils::new_error("Failed to modify QP to RTS")
        };
        Ok(())
    }

    pub fn post_recv(&self, n: u32) -> u32{
        let sge = unsafe{wr::Sge{addr: self.recv_union.buf as u64, length: PACKET_SIZE as u32, lkey: self.base_context.recv_mr.lkey()}}; 
        let mut wr = wr::RecvRequest::zeroed();
        wr.id(PINGPONG_RECV_WRID);
        wr.sg_list(&[sge]);
        let mut i = 0;
        while i< n{
            unsafe{
                if let Ok(()) = self.base_context().qp.post_recv(&wr) {
                    println!("Posted {i} receives");
                    break
                }
            } 
            i+=1;
        }
        i    
    }
    
    pub fn start_poll(&self, attr: &mut poll_cq_attr::PollCQAttr) -> Result<(), pingpong_context::PollingError>{
        self.base_context.start_poll(attr)
    }

    pub fn end_poll(&self){
        self.base_context.end_poll()
    }

    pub fn next_poll(&self) -> Result<(), pingpong_context::PollingError>{
        self.base_context.next_poll()
    }

    pub fn parse_single_wc(&mut self, available_recv: &mut u32,  persistence: &mut Option<&mut persistence_agent::PersistenceAgent>) -> Result<(), std::io::Error>{
        let status = self.base_context.cq.status();
        let wr_id = self.base_context.cq.wr_id();
        let ts = self.base_context.cq.read_completion_ts();
        if status != bindings::IBV_WC_SUCCESS{
            println!("Failed: status {status} for wr_id {wr_id}");
            return utils::new_error(format!("Failed: status {} for wr_id {}", status, wr_id).as_str());
        }
        match wr_id {
            PINGPONG_SEND_WRID => println!("Sent packet"),
            PINGPONG_RECV_WRID => {
                println!("Received packet");
                match persistence{
                    Some(persistence) => {
                        unsafe{
                            (*self.recv_union.payload).set_ts_value(3, utils::get_time_ns());
                            persistence.write(*self.recv_union.payload);
                        }
                    }
                    None => {
                        unsafe{
                            (*self.send_union.payload) = *self.recv_union.payload;
                            (*self.send_union.payload).set_ts_value(1, ts);
                            (*self.send_union.payload).set_ts_value(2, utils::get_time_ns());
                        }
                        self.post_send(post_context::PostOptions{queue_idx: None, lkey:0, buf: std::ptr::null_mut()})?
                    }
                }
                *available_recv -= 1;
                if *available_recv <=1{
                    println!("before post_recv");
                    *available_recv += self.post_recv(RECEIVE_DEPTH as u32 - *available_recv);
                    println!("after post_recv");
                    if *available_recv < RECEIVE_DEPTH as u32{
                        println!("Couldn't post enough receives, there are only {available_recv}");
                        return utils::new_error("Couldn't post enough receives, there are only {available_recv}");
                    }
                }
            }
            _ => {
                     return utils::new_error("Completion for unknown wr_id {wr_id}");
                 }
            
        }
        let _wr_id = <u64 as std::convert::TryInto<u8>>::try_into(wr_id).unwrap();
        //self.pending.fetch_and(! wr_id, std::sync::atomic::Ordering::Relaxed);
        Ok(())
    }


}

impl post_context::PostContext for RCContext{
    
    fn set_send_payload(&mut self, payload: PingPongPayload){
        unsafe{
            *self.send_union.payload = payload;
        }
    }

    fn post_send(&mut self, options: post_context::PostOptions) -> Result<(), std::io::Error>{
        self.qpx.start_wr(); 
        self.qpx.wr_id(PINGPONG_SEND_WRID);
        self.qpx.wr_flags(self.send_flags);
        let _ = self.qpx.post_send();
        
        let buf = if options.buf.is_null() {unsafe{self.send_union.buf as *mut u8}} else {options.buf};
        self.qpx.set_sge(self.base_context.send_mr.lkey(), buf as u64, PACKET_SIZE as u32);
        self.qpx.wr_complete()?;
        // self.pending.fetch_or(PINGPONG_SEND_WRID.try_into().unwrap(), Ordering::Relaxed);

       Ok(()) 
    }

    fn base_context(&self) -> &PingPongContext {
        &self.base_context
    }

    fn get_send_buf(&mut self) -> *mut u8 {
        unsafe{
            self.send_union.buf as *mut u8
        }
    }

    fn set_pending_send_bit(&mut self, _bit: usize) {
        
    }
}
