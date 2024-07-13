const CHAR_BIT: usize = 8;

pub const fn bitset_slots(nb: usize) -> usize{
    (nb+CHAR_BIT-1)/CHAR_BIT
}


pub fn bitset_set(bitset: &mut [u8; 16], bit: usize){
    bitset[bitset_slots(bit)] |= 1 << (bit % CHAR_BIT);
}

pub fn bitset_clear(bitset: &mut [u8; 16], bit: usize){
    bitset[bitset_slots(bit)] &= !(1 << (bit % CHAR_BIT));
}

pub fn bitset_test(bitset: &[u8; 16], bit: usize) -> bool {
    (bitset[bitset_slots(bit)] & (1 << (bit % CHAR_BIT))) != 0
}
