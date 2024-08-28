#![no_std]
#![no_main]

use aya_ebpf::{bindings::xdp_action, macros::xdp, programs::XdpContext};
use aya_log_ebpf::info;
use aya_ebpf::helpers::bpf_ktime_get_ns;
use core::mem;
use network_types::{
    eth::{EthHdr, EtherType},
    ip::{IpProto, Ipv4Hdr},
    udp::UdpHdr,
};
use pingpong::PingPongPayload;

const XDP_UDP_PORT: u16 = 1234;


#[xdp]
pub fn pure_client(ctx: XdpContext) -> u32 {
    match try_pure_client(ctx) {
        Ok(ret) => ret,
        Err(_) => {
            xdp_action::XDP_ABORTED
        },
    }
}


#[inline(always)] // (1)
fn ptr_at<T>(ctx: &XdpContext, offset: usize) -> *mut T {
    let start = ctx.data();

    (start + offset) as *mut T
}


fn try_pure_client(ctx: XdpContext) -> Result<u32, u32> {
    info!(&ctx, "received a packet");
    unsafe{
        aya_ebpf::bpf_printk!(b"alpaca");
    }
    let ts = unsafe{bpf_ktime_get_ns()};
    let eth_size = mem::size_of::<EthHdr>();
    let ip_size = mem::size_of::<Ipv4Hdr>() ;
    let udp_size = mem::size_of::<UdpHdr>();
    let payload_size = mem::size_of::<PingPongPayload>();
    if ctx.data() + eth_size +ip_size + udp_size + payload_size > ctx.data_end(){
        info!(&ctx, "Packet too small");
        return Ok(xdp_action::XDP_PASS);
    }

    let eth = ptr_at::<EthHdr>(&ctx, 0);
    let iph = ptr_at::<Ipv4Hdr>(&ctx, eth_size);
    let udp = ptr_at::<UdpHdr>(&ctx, eth_size+ip_size);
    let payload: *mut PingPongPayload= ptr_at::<PingPongPayload>(&ctx, eth_size+ip_size+udp_size);

    if unsafe{(*eth).ether_type} != EtherType::Ipv4{
        info!(&ctx, "Invalid packet");
        return Ok(xdp_action::XDP_PASS);
    }

    if unsafe{(*iph).proto} != IpProto::Udp{
        info!(&ctx, "Invalid packet");
        return Ok(xdp_action::XDP_PASS);
    }

    if unsafe{(*udp).dest} != XDP_UDP_PORT{
        info!(&ctx, "Invalid packet");
        return Ok(xdp_action::XDP_PASS);
    }

    if !unsafe{(*payload).is_valid()}  {
        info!(&ctx, "Invalid packet");
        return Ok(xdp_action::XDP_PASS);
    }

    unsafe{
        (*payload).ts[3] = ts;
    }
    Ok(xdp_action::XDP_PASS)
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    unsafe { core::hint::unreachable_unchecked() }
}
