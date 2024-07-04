use ibverbs::{Device, DeviceList};




pub fn ib_device_find_by_name<'a>(device_list: &'a DeviceList, name: &str) -> Option<Device<'a>> {
    let mut dev = None;
    for d in device_list{
        let dev_name = d.name();
        match dev_name {
            Some(d_name) => if name == d_name.to_str().unwrap() {
                dev = Some(Device::from(d));
            } ,
            None => ()
        };
    }
    return dev;
}
