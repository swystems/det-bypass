# DET-BYPASS RUST PORT
This folder contain the port of the det bypass project from C to RUST.

The file structure is quite similar to the one in the C code, with the biggest difference that the client and 
server portion of each C file was put in a separate file.

Also to follow RUST idioms the core of the computation has been put in a '*_lib' crate while the binaries are 
separate crates. The binary does little more than command line argument parsing and invoking the correct function
from the corresponding '*_lib' crate.

The 'common' crate fulfills a similar role to the 'common' folder in the original C code, containing functionalities
common to multiple crates.

The 'pingpong' crate contains the struct 'PingPongPayload'. This struct can't use the standard library given that it is
also used in a BPF context. Due to a quirk of cargo, even if the file is marked as 
```rust
#![no_std]
```
If it is part of crate that generally uses the standard library it will be considered as using it as well.
Thus I had to move it to its own crate.

# NO-BYPASS
The three crates 'no_bypass_client', 'no_bypass_server' and 'no_bypass_lib' contain the port of the software found in the
'no-bypass' folder on the C side.
There is nothing particular notable about this code.

# RDMA
The crates 'rdma_rc_client', 'rdma_rc_server', 'rdma_ud_client', 'rdma_ud_server' and 'rdma_rc_lib' contain the port of 
the C 'rdma' code. I divided it in multiple crates as to follow suggested RUST practices.

Even tough all of the code for RDMA has been ported it is to note that these crates do not work as intended. 
In fact at line 88 of 'rc.rs', the invocation of 'ctx.parse_single_wc()' always results in a **IBV_WC_LOC_PROT_ERR**.

This error seems to be caused by a misuse of memory. Either by incorrectly registering the needed Memory Region, or by 
using memory outside the registered region.

The use of a forked version of a rdma crate was needed [js-rdma](https://github.com/JacobSalvi/rdma).

After a lot of debugging we couldn't find the root of the issue and a [PR](https://github.com/Nugine/rdma/issues/4) request has been opened with the original author
of the used RDMA crate.

# PERFORMANCE
This crate contains used to gauge the performance of various sleeping techniques.

The 'main.rs' file can be used to get some information about the execution of sleep given a pair of sleep duration and 
threshold. It performs a thousand runs with the given parameters and outputs results such as:
 - The average sleep duration obtained
 - The standard deviation
 - The max sleep duration of all runs
 - The min sleep duration of all runs

The script 'perf.py' can be used to automate the execution of this experiment and to output its result in various forms,
including a correctly formatted **CPP** map, which maps from desired sleep duration to error-threshold pairs.

# PINGPONG-XDP
The crates 'pingpong-xdp' and 'pingpong-xdp-server' contain client and server ports for the 'xdp/pingpong.c' and 'xdp/pp_poll.c'
code.

# PURE_CLIENT
The crates 'pure_client' and 'pure_server' contain the ports for 'xdp/pingpong_pure.c' and 'xdp/pp_pure.c'.

# XSK-PINGPONG
These last crates contain the ports for 'xdp/pingpong_xsk.c' and 'xdp/pp_sock.c'.

It was necessary for me to fork [xsk-rs](https://github.com/JacobSalvi/xsk-rs) and to create some bindings, [libxdp](https://github.com/JacobSalvi/libxdp),
to some C libraries
to make the XSK code work.

# A word on EBPF in RUST
The framework used to write EBPF in rust is [AYA](https://aya-rs.dev/book/). 

This framework has not been very stable, failing on VM and hardware at times, and it contained a number of oddities.

## info!()
The **info!()** macro can be used to log information from a bpf program similar to the **bpf_printk()** C counterpart.
That said its use can cause compiler errors to be thrown, but not always and not consistently.

## Result<T, E>
The use of results can also cause compiler errors. In particular it seems that unwrapping a result causes some issues.

The following code ends in a compilation error because of the use of the '?' operator.
```rust
#[inline(always)] // 
fn ptr_at<T>(ctx: &XdpContext, offset: usize) -> Result<*const T, ()> {
    let start = ctx.data();
    let end = ctx.data_end();
    let len = mem::size_of::<T>();

    if start + offset + len > end {
        return Err(());
    }

    Ok((start + offset) as *const T)
}

fn try_xdp_firewall(ctx: XdpContext) -> Result<u32, ()> {
   let ethhdr: *const EthHdr = ptr_at(&ctx, 0)?;
    Ok(xdp_action::XDP_PASS)
}
```

This code also ends in a compilation error, due to the match which unwraps the result.
```rust
fn try_xdp_firewall(ctx: XdpContext) -> Result<u32, ()> {
   let ethhdr = ptr_at::<*const EthHdr>(&ctx, 0);
   match ethhdr {
     Ok(a) => a,
     Err(_) => return Err(())
   };
   Ok(xdp_action::XDP_PASS)
}
```

On the other hand the following code works, but it does nothing useful:

```rust
fn try_xdp_firewall(ctx: XdpContext) -> Result<u32, ()> {
   let ethhdr: *const EthHdr = ptr_at(&ctx, 0);
    Ok(xdp_action::XDP_PASS)
}
```

## DIFFERENT RETURN VALUES
If the main xdp function return different values (not types) in different branches it will also end in a compiler error.

As an example this causes an error:

```rust
fn try_xdp_firewall(ctx: XdpContext) -> Result<u32, ()> {
    if ctx.data_end() > EthHdr::LEN {
        return Ok(xdp_action::XDP_ABORTED);
    }

    Ok(xdp_action::XDP_PASS)
}

```

But the following two cases do not: 
```rust
fn try_xdp_firewall(ctx: XdpContext) -> Result<u32, ()> {
    if ctx.data_end() > EthHdr::LEN {
        return Ok(xdp_action::XDP_ABORTED);
    }

    Ok(xdp_action::XDP_ABORTED)
}

```
```rust
fn try_xdp_firewall(ctx: XdpContext) -> Result<u32, ()> {
    if ctx.data_end() > EthHdr::LEN {
        return Ok(xdp_action::XDP_PASS);
    }

    Ok(xdp_action::XDP_PASS)
}

```

