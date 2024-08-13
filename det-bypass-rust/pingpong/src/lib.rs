#![no_std]

pub static PINGPONG_MAGIC: u32 =  0x8badbeef;


#[derive(Debug, Clone, Copy)]
pub struct PingPongPayload {
    pub id: u64,
    pub ts: [u64; 4],
    pub phase: u32,
    pub magic: u32
}


impl PingPongPayload{
    pub fn new(id: u64) -> PingPongPayload{
        PingPongPayload{id, phase:0, ts: [0;4], magic: PINGPONG_MAGIC}
    }

    pub fn new_empty() -> PingPongPayload{
        PingPongPayload::new(0)
    }

    pub fn is_valid(&self) -> bool{
        self.magic == PINGPONG_MAGIC 
    }

    pub fn compute_latency(&self) -> u64{
        ((self.ts[3] - self.ts[0])-(self.ts[2]-self.ts[1]))/2
    }
    
    pub fn set_ts_value(&mut self, idx: usize, new_value: u64){
        self.ts[idx] = new_value;
    }
    
    pub fn serialize(&self) -> [u8; 48]{
        let id = self.id.to_le_bytes();
        let mut ts: [u8; 32] = [0;32];
        let mut i = 0;
        for el in self.ts{
            let bytes = el.to_le_bytes();            
            for byte in bytes{
                ts[i] = byte;
                i += 1;
            }
        } 
        // let ts: Vec<u8> = self.ts.map(|el| el.to_le_bytes()).into_iter().flatten().collect();
        let phase = self.phase.to_le_bytes();
        let magic = self.magic.to_le_bytes();
        let mut res: [u8; 48] = [0;48];
        for i in 0..48{
            if i<8 {
                res[i] = id[i];
            } else if i< 40 {
                res[i] = ts[i-8];
            } else if i< 44{
                res[i] = phase[i-40];
            } else{
                res[i] = magic[i-44];
            }
        }
        res
    }

    pub fn deserialize(bytes: &[u8]) -> PingPongPayload {
        let id: u64 = u64::from_le_bytes(bytes[..8].try_into().unwrap());
        let phase: u32 = u32::from_le_bytes(bytes[40..44].try_into().unwrap());
        let magic: u32 = u32::from_le_bytes(bytes[44..48].try_into().unwrap());
        let mut ts: [u64; 4] = [0; 4];
        ts[0] = u64::from_le_bytes(bytes[8..16].try_into().unwrap());
        ts[1] = u64::from_le_bytes(bytes[16..24].try_into().unwrap());
        ts[2] = u64::from_le_bytes(bytes[24..32].try_into().unwrap());
        ts[3] = u64::from_le_bytes(bytes[32..40].try_into().unwrap());
        PingPongPayload{id, ts, phase, magic}
    }
}

