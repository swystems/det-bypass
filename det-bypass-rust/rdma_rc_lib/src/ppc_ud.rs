use crate::{ib_net, pingpong_context, IB_PORT, PRIORITY};
use common::{bitset, consts, persistence_agent, utils};
use rdma::{ah, bindings, cq, device, qp::{self}, wc, wr};

const QUEUE_SIZE: usize = 128;
const PACKET_SIZE: usize = 1024;

pub struct UDContext{
    base_context: pingpong_context::PingPongContext,
    pub(crate) pending_send: [u8; common::bitset::bitset_slots(QUEUE_SIZE)],
    send_flags: wr::SendFlags,
    ah: Option<ah::AddressHandle>,
    pub(crate) remote_info: Option<ib_net::IbNodeInfo>,
    pub(crate) send_buf: *mut u8,
    send_payload: persistence_agent::PingPongPayload, 
    recv_bufs: *mut u8,
    pub(crate) recv_payloads: [persistence_agent::PingPongPayload; QUEUE_SIZE]
}


impl UDContext{
    pub fn new(device: &device::Device) -> Result<Self, std::io::Error> {
        let pending_send = [0; bitset::bitset_slots(QUEUE_SIZE)];
        let mut send_flags = wr::SendFlags::SIGNALED; 
        let layout = std::alloc::Layout::from_size_align(PACKET_SIZE, std::mem::align_of::<u8>()).unwrap();
        let send_buf: *mut u8 = unsafe { 
            std::alloc::alloc(layout) 
        };
        if send_buf.is_null(){
            return utils::new_error("Couldn't allocate send buffer");
        } 

        let send_payload = persistence_agent::PingPongPayload::new_empty();
        let layout = std::alloc::Layout::from_size_align(PACKET_SIZE*QUEUE_SIZE, std::mem::align_of::<u8>()).unwrap();
        let recv_bufs = unsafe {
            std::alloc::alloc(layout)
        };
        if recv_bufs.is_null(){
            return utils::new_error("Couldn't allocate recv_buf");
        }
        let recv_payloads = [persistence_agent::PingPongPayload::new_empty(); QUEUE_SIZE];

        let mut cq_options = cq::CompletionQueue::options();
        cq_options.cqe(QUEUE_SIZE);

        let mut qp_options = qp::QueuePair::options(); 
        qp_options.cap(qp::QueuePairCapacity{max_send_wr: QUEUE_SIZE as u32, max_recv_wr: QUEUE_SIZE as u32, max_send_sge: 1, max_recv_sge: 1, max_inline_data: 0});
        qp_options.qp_type(qp::QueuePairType::UD);
        
        let builder = pingpong_context::PPContextBuilder::new(device)
            .recv_buf(recv_bufs, PACKET_SIZE*QUEUE_SIZE)
            .send_buf(send_buf, PACKET_SIZE)
            .with_cq(cq_options)
            .with_qp(qp_options, false);
        let mut base_context = builder.build()?;

        match base_context.qp.query(qp::QueryOptions::default()){
            Ok(attr) => {
                if attr.cap().unwrap().max_inline_data >= PACKET_SIZE as u32{

                    send_flags = send_flags.union(wr::SendFlags::INLINE);
                } else{
                    println!("Device doesn't support IBV_SEND_INLINE, using sge.");
                }
            },
            Err(_) => {
                return utils::new_error("QP query failed");
            }

        }
        let mut modify_options = qp::ModifyOptions::default();
        modify_options.qp_state(qp::QueuePairState::Initialize);
        modify_options.pkey_index(0);
        modify_options.port_num(IB_PORT);
        modify_options.qkey(0x11111111);
        base_context.modify_qp(modify_options)?;
        Ok(UDContext{base_context, send_flags, pending_send, recv_bufs, send_buf, recv_payloads, send_payload, remote_info: None, ah: None})
    }

    pub fn base_context(&self) ->  &pingpong_context::PingPongContext{
        &self.base_context
    }

    pub(crate) fn set_remote_info(&mut self, remote_info: ib_net::IbNodeInfo) {
        self.remote_info = Some(remote_info);
    }

    pub(crate) fn connect(&mut self, port_gid_idx: i32, local_info: &ib_net::IbNodeInfo) -> Result<(), std::io::Error> {
        let dest = match &self.remote_info{
            Some(ri) => ri,
            None => return utils::new_error("Cannot connect without a remote node info")
        };
        let mut ah_attr = ah::AddressHandleOptions::default();
        ah_attr.dest_lid(dest.lid);
        ah_attr.port_num(IB_PORT);
        ah_attr.service_level(PRIORITY);
        let mut attr = qp::ModifyOptions::default();
        attr.qp_state(qp::QueuePairState::ReadyToReceive);
        self.base_context.modify_qp(attr)?;
        let mut attr = qp::ModifyOptions::default();
        attr.qp_state(qp::QueuePairState::ReadyToSend);
        attr.sq_psn(local_info.psn);
        self.base_context.modify_qp(attr)?;
        if dest.gid.interface_id() !=0 {
            ah_attr.global_route_header(ah::GlobalRoute { dest_gid: dest.gid, flow_label:0 , sgid_index: port_gid_idx as u8, hop_limit:1 , traffic_class: 0 });
        }

        let ah = ah::AddressHandle::create(&self.base_context.pd, ah_attr)?;
        self.ah = Some(ah);
        Ok(())
        
    }

    pub(crate) fn post_recv(&self, queue_index: usize) -> std::io::Result<()>{
        let list = wr::Sge{addr: self.recv_bufs as u64*((queue_index*consts::PACKET_SIZE)as u64), length: consts::PACKET_SIZE as u32, lkey: self.base_context.recv_mr.lkey()};
        let mut wr = wr::RecvRequest::zeroed();
        wr.id((consts::QUEUE_SIZE+queue_index)as u64);
        wr.sg_list(&[list]);
        unsafe{
            self.base_context.qp.post_recv(&wr)?;
        }
        Ok(())

        }


    pub fn post_send(&mut self, queue_idx: Option<usize>, lkey: u32, buf: *mut u8) -> Result<(), std::io::Error>{
        let remote = match &self.remote_info{
            Some(ri) => ri,
            None => {return utils::new_error("Remote info node must be set up when sending")}
        };
        let list = unsafe{wr::Sge{addr: buf.add(40) as u64, length: consts::PACKET_SIZE as u32-40, lkey}};
        let mut wr = wr::SendRequest::zeroed();
        let queue_idx = match queue_idx{
            Some(q) => q,
            None => 0
        };
        wr.id(queue_idx as u64);
        wr.sg_list(&[list]);
        wr.opcode(wr::Opcode::Send);
        wr.send_flags(self.send_flags);
        let ah = match &self.ah {
            Some(ah) => ah,
            None => {
                return utils::new_error("AddressHandle must be set before sending")
            }
        };
        wr.ud_ah(ah);
        wr.ud_remote_qpn(remote.qpn);
        wr.ud_remote_qkey(0x11111111);
        bitset::bitset_set(&mut self.pending_send, queue_idx);
        unsafe {
            self.base_context.qp.post_send(&wr)?;
        }
        Ok(())
    }

    fn check_wc(wc: &mut wc::WorkCompletion) -> Result<(), std::io::Error>{
        if wc.status() != bindings::IBV_WC_SUCCESS{
            unsafe{
                eprintln!("Failed status {:?} ({}) for wr_id {}\n", bindings::ibv_wc_status_str(wc.status()), wc.status(), wc.wr_id()); 
            }
            return utils::new_error(&format!("Failed status for wc: {}", wc.wr_id()));
        }
        if  wc.wr_id() >= consts::QUEUE_SIZE as u64 +consts::QUEUE_SIZE as u64{
            eprintln!("Completion for unknown wr_id {}", wc.wr_id());
            return utils::new_error(&format!("Completion for unknown wr_id {}", wc.wr_id()));
        }
        Ok(())

    }
    
    pub(crate) fn parse_single_wc_client(&mut self, wc: &mut wc::WorkCompletion, persistence: &mut persistence_agent::PersistenceAgent) -> Result<(), std::io::Error> {
        UDContext::check_wc(wc)?;

        if wc.wr_id() < consts::QUEUE_SIZE as u64{
            bitset::bitset_clear(&mut self.pending_send, wc.wr_id() as usize);
            return Ok(())
        }

        let queue_idx = wc.wr_id() as usize - consts::QUEUE_SIZE;
        self.recv_payloads[queue_idx].set_ts_value(3, utils::get_time_ns());
        persistence.write(self.recv_payloads[queue_idx]);
        self.post_recv(queue_idx)?;
        Ok(())
    }

    pub(crate) fn parse_single_wc_server(&mut self, wc: &mut wc::WorkCompletion) -> Result<(), std::io::Error> {
        UDContext::check_wc(wc)?;

        if wc.wr_id() < consts::QUEUE_SIZE as u64{
            bitset::bitset_clear(&mut self.pending_send, wc.wr_id() as usize);
            self.post_recv(wc.wr_id() as usize)?;
        }
        let ts = utils::get_time_ns();
        let queue_idx = wc.wr_id() as usize -consts::QUEUE_SIZE;        
        common::busy_wait!(bitset::bitset_test(&self.pending_send,queue_idx ));
        self.recv_payloads[queue_idx].set_ts_value(1, ts);
        self.recv_payloads[queue_idx].set_ts_value(2, utils::get_time_ns());
        println!("Sending back packet {} for queue {}", self.recv_payloads[queue_idx].id, queue_idx);
        unsafe{
            self.post_send(Some(queue_idx), self.base_context.recv_mr.lkey(), self.recv_bufs.add(queue_idx*consts::PACKET_SIZE))?;
        }
        Ok(())
    }

    pub(crate) fn set_send_payload(&mut self, payload: persistence_agent::PingPongPayload) {
        self.send_payload = payload
    }
}


