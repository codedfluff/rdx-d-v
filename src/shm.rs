//! Shared memory: named blocks of memory that several processes can map
//! into their own address spaces and then read and write directly, with no
//! system call per access.
//!
//! An object lives until it is unlinked by name *and* the last process using
//! it has gone. Its contents are not saved across reboots.

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::sync::Arc;
use alloc::vec::Vec;

use crate::memory::{self, PAGE, USER_BASE};
use crate::sync::IrqLock;

/// Where in a process shared mappings start.
pub const MAP_BASE: u64 = USER_BASE + 0x9000_0000;
const MAP_END: u64 = USER_BASE + 0xF000_0000;
const MAX_SIZE: u64 = 16 * 1024 * 1024;

pub struct Shm {
    frames: Vec<u64>,
}

impl Shm {
    pub fn size(&self) -> u64 {
        self.frames.len() as u64 * PAGE
    }
}

impl Drop for Shm {
    fn drop(&mut self) {
        for &frame in &self.frames {
            memory::free_frame(frame);
        }
    }
}

static OBJECTS: IrqLock<BTreeMap<String, Arc<Shm>>> = IrqLock::new(BTreeMap::new());

/// Find the object called `name`, creating it (zero-filled, `size` bytes
/// rounded up to whole pages) if it does not exist yet.
pub fn open(name: &str, size: u64) -> Option<Arc<Shm>> {
    let mut objects = OBJECTS.lock();
    if let Some(existing) = objects.get(name) {
        return Some(existing.clone());
    }
    if size == 0 || size > MAX_SIZE {
        return None;
    }
    let mut frames = Vec::new();
    for _ in 0..size.div_ceil(PAGE) {
        match memory::try_alloc_zeroed_frame() {
            Some(frame) => frames.push(frame),
            None => {
                frames.into_iter().for_each(memory::free_frame);
                return None;
            }
        }
    }
    let object = Arc::new(Shm { frames });
    objects.insert(String::from(name), object.clone());
    Some(object)
}

/// Forget the name. Processes that already have the object keep it.
pub fn unlink(name: &str) -> bool {
    OBJECTS.lock().remove(name).is_some()
}

/// Undo `map`.
pub fn unmap(object: &Shm, space: u64, address: u64) {
    for i in 0..object.frames.len() as u64 {
        memory::unmap_shared_page(space, address + i * PAGE);
    }
}

/// Map an object into the address space `space` at `address`.
pub fn map(object: &Shm, space: u64, address: u64) -> bool {
    if address < MAP_BASE || address + object.size() > MAP_END {
        return false;
    }
    object.frames.iter().enumerate().all(|(i, &frame)| memory::map_shared_page(space, address + i as u64 * PAGE, frame))
}
