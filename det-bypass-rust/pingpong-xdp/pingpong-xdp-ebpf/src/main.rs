#![no_std]
#![no_main]

use aya_ebpf::{bindings::xdp_action, macros::xdp, programs::XdpContext, maps::array::Array};
use aya_log_ebpf::info;
use aya_ebpf::bindings::bpf_spin_lock;
use aya_ebpf::helpers::bpf_spin_lock;
use aya_ebpf::helpers::bpf_spin_unlock;
use network_types::{
    eth::EthHdr,
    ip::Ipv4Hdr,
};
use core::mem;

use pingpong::PingPongPayload;

const PACKETS_MAP_SIZE: u32= 128;

static LAST_PAYLOAD: Array::<PingPongPayload> = Array::<PingPongPayload>::pinned(PACKETS_MAP_SIZE, 1024);

struct LockMapElement{
    value: bpf_spin_lock,
    index: u32
}

static LCK: Array::<LockMapElement> = Array::<LockMapElement>::with_max_entries(1, 0);


#[inline(always)]
pub fn add_packet_to_map(ctx: &XdpContext, payload: *mut PingPongPayload) -> u32 {
    let lock = match LCK.get_ptr_mut(0){
        Some(v) => v,
        _ => {
                // info!(ctx, "failed to lookup lock element");
                return 1
            }
    };

    unsafe{
        bpf_spin_lock(&mut (*lock).value);
        let key = (*lock).index;
        (*lock).index = ((*lock).index+1)%PACKETS_MAP_SIZE;
        bpf_spin_unlock(&mut (*lock).value);
    
        let old_payload = match LAST_PAYLOAD.get_ptr_mut(key){
            Some(v) => v,
            _ => {
                // info!(ctx, "failed to lookup element");
                return 1
            }
        };

        if (*old_payload).is_valid(){
            // info!(ctx, "Map is full. Dropping packet at index:{}", key);
            bpf_spin_lock(&mut (*lock).value);
            (*lock).index = key;
            bpf_spin_unlock(&mut (*lock).value);
            return 1;
        }
        (*payload).magic = (*payload).magic & 0xFFFFFFFE;
        (*old_payload) = *payload;
        (*old_payload).magic |= 1;
    }
    0
}

#[xdp]
pub fn pingpong_xdp(ctx: XdpContext) -> u32 {
    match try_pingpong_xdp(ctx) {
        Ok(ret) => ret,
        Err(_) => xdp_action::XDP_ABORTED,
    }
}

#[inline(always)] // (1)
fn ptr_at<T>(ctx: &XdpContext, offset: usize) -> *mut T {
    let start = ctx.data();

    (start + offset) as *mut T
}



fn try_pingpong_xdp(ctx: XdpContext) -> Result<u32, u32> {
    info!(&ctx, "received a packet");
    let eth_size = mem::size_of::<EthHdr>();
    let ip_size = mem::size_of::<Ipv4Hdr>() ;
    let payload_size = mem::size_of::<PingPongPayload>();
    if ctx.data() + eth_size +ip_size + payload_size > ctx.data_end(){
        info!(&ctx, "Packet too small");
        return Ok(xdp_action::XDP_PASS);
    }
    let eth = ptr_at::<EthHdr>(&ctx, 0);
    let _iph = ptr_at::<Ipv4Hdr>(&ctx, eth_size);
    let payload: *mut PingPongPayload= ptr_at::<PingPongPayload>(&ctx, eth_size+ip_size);
    if unsafe{(*eth).ether_type} as u16 != 0x2002{
        info!(&ctx, "Invalid eth protocol");
        return Ok(xdp_action::XDP_PASS);
    }

    if !unsafe{(*payload).is_valid()}  {
        info!(&ctx, "Invalid packet");
        return Ok(xdp_action::XDP_PASS);
    }

    // if add_packet_to_map(&ctx, payload) != 0{
            // info!(&ctx, "Could not add packet to map");
    // }
    add_packet_to_map(&ctx, payload);

    Ok(xdp_action::XDP_PASS)
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    unsafe { core::hint::unreachable_unchecked() }
}
