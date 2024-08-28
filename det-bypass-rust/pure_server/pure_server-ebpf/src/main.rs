#![no_std]
#![no_main]

use aya_ebpf::{bindings::xdp_action, macros::xdp, programs::XdpContext};
use aya_log_ebpf::info;
use aya_ebpf::helpers::bpf_ktime_get_ns;
use aya_ebpf::helpers::bpf_csum_diff;
use core::mem;
use network_types::{
    eth::{EthHdr, EtherType},
    ip::{IpProto, Ipv4Hdr},
    udp::UdpHdr,
};
use pingpong::PingPongPayload;

#[inline(always)]
pub fn csum_fold_helper(mut csum: u64)-> u16{
    for _i in 0..4{
        if csum >> 16 != 0{
            csum = (csum & 0xffff)+(csum >> 16);
        }
    }
    return !csum as u16;
}

const XDP_UDP_PORT: u16 = 1234;


#[inline(always)]
pub fn ipv4_csum(data_start: *mut u32, data_size: u32, csum: *mut u64){
    unsafe{
        (*csum) = bpf_csum_diff(core::ptr::null_mut(), 0, data_start, data_size, csum as u32) as u64;
        (*csum) = csum_fold_helper(csum as u64) as u64;
    }
}


#[xdp]
pub fn xdp_test(ctx: XdpContext) -> u32 {
    info!(&ctx, "AAAAAA");
    match try_xdp_test(ctx) {
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


fn try_xdp_test(ctx: XdpContext) -> Result<u32, u32> {
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
        return Ok(xdp_action::XDP_TX);
    }

    let eth = ptr_at::<EthHdr>(&ctx, 0);
    let iph = ptr_at::<Ipv4Hdr>(&ctx, eth_size);
    let udp = ptr_at::<UdpHdr>(&ctx, eth_size+ip_size);
    let payload: *mut PingPongPayload= ptr_at::<PingPongPayload>(&ctx, eth_size+ip_size+udp_size);

    if unsafe{(*eth).ether_type} != EtherType::Ipv4{
        info!(&ctx, "Invalid packet");
        return Ok(xdp_action::XDP_TX);
    }

    if unsafe{(*iph).proto} != IpProto::Udp{
        info!(&ctx, "Invalid packet");
        return Ok(xdp_action::XDP_TX);
    }

    if unsafe{(*udp).dest} != XDP_UDP_PORT{
        info!(&ctx, "Invalid packet");
        return Ok(xdp_action::XDP_TX);
    }

    if !unsafe{(*payload).is_valid()}{
        info!(&ctx, "Invalid packet");
        return Ok(xdp_action::XDP_TX);
    }
    unsafe{
        (*payload).ts[1] = ts;

    // swap the mac address
    let dest = (*eth).dst_addr;
    (*eth).dst_addr = (*eth).src_addr;
    (*eth).src_addr = dest;
    

    // Swap the IP addresses
    let daddr = (*iph).dst_addr;
    (*iph).dst_addr = (*iph).src_addr;
    (*iph).src_addr = daddr;
    (*iph).check = 0;
    let mut  csum: u64 = 0;
    ipv4_csum(iph as *mut u32, ip_size as u32, &mut csum as *mut u64);
    (*iph).check = csum as u16;
    (*iph).ttl = 64;

    (*udp).source = XDP_UDP_PORT; 
    (*udp).dest = XDP_UDP_PORT;
    (*udp).check = 0;

    (*payload).ts[2] = bpf_ktime_get_ns ();
    // Send the packet back
    // return  Ok(xdp_action::XDP_TX);
    }
    Ok(xdp_action::XDP_TX)
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    unsafe { core::hint::unreachable_unchecked() }
}
