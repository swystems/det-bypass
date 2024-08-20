#![no_std]
#![no_main]

use aya_ebpf::{bindings::xdp_action, macros::xdp, programs::XdpContext, maps};
use aya_log_ebpf::info;
use network_types::eth::EthHdr;
use network_types::ip::Ipv4Hdr;
use pingpong::PingPongPayload;

static XSK_MAP: maps::XskMap = maps::XskMap::pinned(1, 0);

#[xdp]
pub fn xsk_pingpong(ctx: XdpContext) -> u32 {
    match try_xsk_pingpong(ctx) {
        Ok(ret) => ret,
        Err(_) => xdp_action::XDP_ABORTED,
    }
}

#[inline(always)] // (1)
fn ptr_at<T>(ctx: &XdpContext, offset: usize) -> *mut T {
    let start = ctx.data();

    (start + offset) as *mut T
}

fn try_xsk_pingpong(ctx: XdpContext) -> Result<u32, u32> {
    info!(&ctx, "received a packet");
    if ctx.data() + EthHdr::LEN + Ipv4Hdr::LEN + core::mem::size_of::<PingPongPayload>() > ctx.data_end(){
        return Ok(xdp_action::XDP_PASS);
    }
    let eth = ptr_at::<EthHdr>(&ctx, 0);
    if unsafe{(*eth).ether_type} as u16 != 0x2002{
        return Ok(xdp_action::XDP_PASS);
    }

    let payload: *mut PingPongPayload= ptr_at::<PingPongPayload>(&ctx, EthHdr::LEN+Ipv4Hdr::LEN);
    if !unsafe{(*payload).is_valid()}{
        return Ok(xdp_action::XDP_PASS);
    }
    let res  = match XSK_MAP.redirect(0, xdp_action::XDP_DROP as u64){
        Ok(r) => r, 
        Err(r) => r
    };
    Ok(res)
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    unsafe { core::hint::unreachable_unchecked() }
}
