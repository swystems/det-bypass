pub mod persistence_agent;
pub mod utils;
pub mod common_net;
pub mod bitset;
pub mod consts;


#[macro_export]
macro_rules! barrier {
    () => {
              unsafe{std::arch::asm!("", options(nomem, nostack, preserves_flags))}
           };
}

#[macro_export]
macro_rules! busy_wait {
    ($cond:expr) => {
        while $cond {
            $crate::barrier!();
        }
    };
}
