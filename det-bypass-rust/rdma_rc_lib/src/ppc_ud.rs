use crate::{ib_net, pingpong_context, IB_PORT, PRIORITY};
use common::{bitset, persistence_agent, utils};
use rdma::{ah, bindings::{self, IBV_SEND_INLINE}, cq, device, mr, qp::{self, QueuePairType}};

const QUEUE_SIZE: usize = 128;
const PACKET_SIZE: usize = 1024;

pub struct UDContext{
    base_context: pingpong_context::PingPongContext,
    pending_send: [u8; common::bitset::bitset_slots(QUEUE_SIZE)],
    send_flags: u32,
    ah: Option<ah::AddressHandle>,
    pub(crate) remote_info: Option<ib_net::IbNodeInfo>,
    send_buf: *mut u8,
    send_payload: persistence_agent::PingPongPayload, 
    recv_bufs: *mut u8,
    recv_payloads: [persistence_agent::PingPongPayload; QUEUE_SIZE]
}


impl UDContext{
    pub fn new(device: &device::Device) -> Result<Self, std::io::Error> {
        let pending_send = [0; bitset::bitset_slots(QUEUE_SIZE)];
        let mut send_flags = bindings::IBV_SEND_SIGNALED; 
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

                    send_flags |= bindings::IBV_SEND_INLINE;
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
}


