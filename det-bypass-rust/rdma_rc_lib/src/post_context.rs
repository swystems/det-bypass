use common::persistence_agent;

use crate::pingpong_context;

pub struct PostOptions{
    pub queue_idx: Option<usize>,
    pub lkey: u32,
    pub buf: *mut u8
}

pub trait PostContext: Sync +Send+Clone{
    fn set_send_payload(&mut self, payload: persistence_agent::PingPongPayload);

    fn post_send(&mut self, options: PostOptions) -> Result<(), std::io::Error>;
    fn base_context(&self) -> &pingpong_context::PingPongContext;

    fn get_send_buf(&mut self) -> *mut u8;
    fn set_pending_send_bit(&mut self, bit: usize);
}


