use std::fs::File;
use std::io::{Error, Write};
use std::path::Path;

const NUM_BUCKETS: u32 = 20000;
static OFFSET: u64 = 1000000;
static PINGPONG_MAGIC: u32 =  0x8badbeef;


#[derive(Debug, Clone, Copy)]
pub struct PingPongPayload {
    pub id: u64,
    ts: [u64; 4],
    phase: u32,
    magic: u32
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
        let ts: Vec<u8> = self.ts.map(|el| el.to_le_bytes()).into_iter().flatten().collect();
        let phase = self.phase.to_le_bytes();
        let magic = self.magic.to_le_bytes();
        let a = [phase, magic].concat();
        let mut res: [u8; 48] = [0;48];
        for i in 0..48{
            if i<8 {
                res[i] = id[i];
            }else if i< 40 {
                res[i] = ts[i-8];
            } else{
                res[i] = a[i-40];
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




#[derive(Debug)]
struct MinMaxLatencyData  {
    min: u64,
    max: u64,
    min_payload: Option<PingPongPayload>,
    max_payload: Option<PingPongPayload>
}


#[derive(Debug, Clone, Copy)]
struct Bucket {
    rel_latency: [u64; 4],
    abs_latency: u64
}

union BucketUnion {
    ptr: *mut libc::c_void,
    buckets:  std::mem::ManuallyDrop<[Bucket; NUM_BUCKETS as usize+2]>
}

struct BucketData {
    tot_packets: u64,
    send_interval: u64,
    min_values: Bucket,
    max_values: Bucket,
    buckets: [Bucket; NUM_BUCKETS as usize +2],
    prev_payload: Option<PingPongPayload>
}

enum PData  {
    Latency(MinMaxLatencyData),
    Bucket(BucketData)
}

struct PersBaseData {
    file: Box<dyn Write>,
    aux: Option<PData>
}


pub struct PersistenceAgent {
    data: PersBaseData,
    flags: u32,

}


static PERSISTENCE_F_STDOUT: u32 = 1;
static PERSISTENCE_M_ALL_TIMESTAMPS: u32 = 4;
static PERSISTENCE_M_MIN_MAX_LATENCY: u32 = 8;
static PERSISTENCE_M_BUCKETS: u32 = 16;

pub fn pers_measurament_to_flag(m: &str) -> u32 {
    match m {
        "all" => PERSISTENCE_M_ALL_TIMESTAMPS,
        "latency" => PERSISTENCE_M_MIN_MAX_LATENCY,
        "buckets" => PERSISTENCE_M_BUCKETS,
        _ => PERSISTENCE_M_ALL_TIMESTAMPS
    }
}


fn persistence_init_min_max_latency() -> MinMaxLatencyData{
    MinMaxLatencyData{min: u64::MAX, max:0, min_payload: None, max_payload: None}
}

fn persistence_init_buckets(init_aux: &u32) -> BucketData{
    let interval = *init_aux as u64;
    let min_values = Bucket{rel_latency: [u64::MAX; 4], abs_latency: u64::MAX};
    let max_values = Bucket{rel_latency: [0;4], abs_latency: 0};
    let memory_size = bucket_compute_hugepage_size();
    unsafe{
        //let ptr: *mut libc::c_void = libc::mmap(core::ptr::null_mut(), memory_size as usize, libc::PROT_READ | libc::PROT_WRITE, libc::MAP_PRIVATE | libc::MAP_ANONYMOUS  , -1, 0);
        //let union_data = BucketUnion{ ptr}; 
        let buckets = [Bucket{rel_latency: [0;4], abs_latency: 0}; NUM_BUCKETS as usize +2];
        BucketData{tot_packets: 0, send_interval: interval, min_values, max_values, prev_payload: None, buckets}
    }
}


fn bucket_compute_hugepage_size() -> u32 {
    unsafe {
    let page_size: i64 = libc::sysconf(libc::_SC_PAGESIZE);
    let page_size: u32 = page_size.try_into().unwrap();
    (std::mem::size_of::<u64>() as u32) * (NUM_BUCKETS+2)*5+page_size-1
    }
}


fn persistence_close_min_max(file: & mut Box<dyn Write> ,aux: &MinMaxLatencyData){
    if aux.min != u64::MAX {
        let payload = &aux.min_payload.as_ref().unwrap();
        let res = writeln!(file, "{:X}: {:X} {:X} {:X} {:X} (LATENCY {:X} ns)", payload.id, payload.ts[0], payload.ts[1], payload.ts[2], payload.ts[3], aux.min);
        if let Err(e) = res {
            panic!("{}",e)
        };
    }
    if aux.max != 0 {
        let payload = &aux.max_payload.as_ref().unwrap();
        let res = writeln!(file, "{:X}: {:X} {:X} {:X} {:X} (LATENCY {:X} ns)", payload.id, payload.ts[0], payload.ts[1], payload.ts[2], payload.ts[3], aux.max);
        if let Err(e) = res {
            panic!("{}", e)
        }
    }
}


fn bucket_idx(val: u64, min: u64, max: u64) -> u64{
    if val < min {
        return 0;
    }
    if val > max {
        return NUM_BUCKETS as u64 +1;
    }
    let bucket_size: u64 = (max-min)/NUM_BUCKETS as u64;
    return (val-min)/bucket_size +1;
}


fn bucket_ranges(interval: u64) -> (u64, u64, u64, u64){
    let rel_min: u64; 
    let rel_max: u64; 
    let abs_min: u64 = 0;
    let abs_max: u64 = interval + OFFSET;
    if interval < OFFSET {
        rel_min = 0;
        rel_max = 2 * OFFSET;
    }
    else {
        rel_min = interval - OFFSET;
        rel_max = interval + OFFSET;
    }
    (rel_min, rel_max, abs_min, abs_max)

}

fn persistence_close_buckets(file: & mut Box<dyn Write>, aux: &BucketData){
    let (rel_min, rel_max, abs_min, abs_max) = bucket_ranges(aux.send_interval);
    let rel_bucket_size = (rel_max-rel_min)/(NUM_BUCKETS as u64);
    let abs_bucket_size = (abs_max - abs_min)/(NUM_BUCKETS as u64);
    
    let tot = format!("TOT {}\n", aux.tot_packets);
    let rel = format!("REL {} {} {}\n", rel_min, rel_max, rel_bucket_size);
    let abs = format!("ABS {} {} {} \n", abs_min, abs_max, abs_bucket_size);
    let min = format!("MIN {} {} {} {} {}\n", aux.min_values.rel_latency[0],
        aux.min_values.rel_latency[1], aux.min_values.rel_latency[2],
        aux.min_values.rel_latency[3], aux.min_values.abs_latency);
    let max = format!("MAX {} {} {} {} {}\n", aux.max_values.rel_latency[0],
        aux.max_values.rel_latency[1], aux.max_values.rel_latency[2],
        aux.max_values.rel_latency[3], aux.max_values.abs_latency);
    let res = writeln!(file, "{} {} {} {} {}", tot, rel, abs, min, max);
    if let Err(e) = res {
        panic!("{}", e)
    }
    for b in aux.buckets.iter() {
        let res = writeln!(file, "{} {} {} {} {}", b.rel_latency[0], b.rel_latency[1], b.rel_latency[2], b.rel_latency[3], b.abs_latency);
        if let Err(e) = res {
            panic!("{}", e)
        }
    }
}



impl  PersistenceAgent {
    pub fn new(filename: Option<&str>, flags: u32, aux: & u32) -> PersistenceAgent{
        let use_stdout: bool = match filename {
            Some(_) => (flags & PERSISTENCE_F_STDOUT) != 0,
            None => true
        };
        let file: Box<dyn Write> = if use_stdout {
            Box::new(std::io::stdout())
        } else {
            File::create(Path::new(filename.unwrap())).map(|f| Box::new(f) as Box<dyn Write>).expect("")
        };
         
        if (flags & PERSISTENCE_M_MIN_MAX_LATENCY) != 0{
            let data = persistence_init_min_max_latency(); 
            let data = PersBaseData{ file, aux: Some(PData::Latency(data))}; 
            return PersistenceAgent{flags, data};
        } else if (flags & PERSISTENCE_M_BUCKETS) != 0 {
            let data = persistence_init_buckets(aux);
            let data = PersBaseData{file, aux: Some(PData::Bucket(data))};
            return PersistenceAgent{flags, data};
        }else{
            return PersistenceAgent{flags, data: PersBaseData{file, aux: None}};
        }
    }

    fn write_latency  (lat: & mut MinMaxLatencyData, payload: PingPongPayload){
        let latency = payload.compute_latency(); 
        if latency < lat.min {
            lat.min = latency;
            lat.min_payload = Some(payload.clone());
        }
        if latency > lat.max {
            lat.max = latency;
            lat.max_payload = Some(payload);
        }
    }

    fn write_buckets(bucket_data: &mut BucketData, payload: PingPongPayload) -> Result<(), Error>{
        bucket_data.tot_packets+=1;
        if ! payload.is_valid(){
            bucket_data.prev_payload = Some(payload.clone());
            return Err(Error::new(std::io::ErrorKind::Other, "writing to bucket failed"));
        }
        if payload.id% 1_000_000 == 0{
            println!("{}", payload.id);
        }
        let mut ts_diff: [u64;4] = [0;4];
        for i in 0..4{
            if let Some(prev_payload) = &bucket_data.prev_payload{
                if payload.ts[i] < prev_payload.ts[i] {
                    eprintln!("ERROR: Timestamps are not monotonically increasing");
                    return Err(Error::new(std::io::ErrorKind::Other, "timestamps are not monotonically increasing"));
                }

                ts_diff[i] = payload.ts[i] - prev_payload.ts[i];
            }
        }

        let (rel_min, rel_max, abs_min, abs_max) = bucket_ranges(bucket_data.send_interval);
        for i in 0..4 {
            if ts_diff[i] < bucket_data.min_values.rel_latency[i] {
                bucket_data.min_values.rel_latency[i] = ts_diff[i];
            }
            if ts_diff[i] > bucket_data.max_values.rel_latency[i] {
                bucket_data.max_values.rel_latency[i] = ts_diff[i];
            }
            let idx = bucket_idx(ts_diff[i], rel_min, rel_max);
            bucket_data.buckets[idx as usize].rel_latency[i] += 1;
        }
        let abs_latency = payload.compute_latency();
        if abs_latency< bucket_data.min_values.abs_latency{
           bucket_data.min_values.abs_latency = abs_latency; 
        }
        if abs_latency > bucket_data.max_values.abs_latency {
            bucket_data.max_values.abs_latency = abs_latency;
        }
        let idx = bucket_idx(abs_latency, abs_min, abs_max);
        bucket_data.buckets[idx as usize].abs_latency += 1;
        bucket_data.prev_payload =Some(payload);
        Ok(())
    }
        

    pub fn write(&mut self, payload: PingPongPayload){
        match &mut self.data.aux {
            Some(PData::Latency(ref mut lat)) => PersistenceAgent::write_latency(lat, payload),
            Some(PData::Bucket(buc)) => {
                 let _ = PersistenceAgent::write_buckets(buc, payload);
            }
            None => {
                let res = writeln!(self.data.file, "{} {} {} {} {}", payload.id, payload.ts[0], payload.ts[1], payload.ts[2], payload.ts[3]); 
                if let Err(e) = res {
                    panic!("{}", e)
                }
            }
        }
    }

    pub fn close(&mut self) {
        match &self.data.aux {
            Some(PData::Latency(d)) => {
                let f = & mut self.data.file;
                persistence_close_min_max(f, d)
            }, 
            Some(PData::Bucket(d)) => {
                let f = & mut self.data.file;
                persistence_close_buckets(f, d) 
            } 
            _ => () 
        }
    }
}
