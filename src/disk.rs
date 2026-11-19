//! Persistent storage for the filesystem, on any block device.
//!
//! Safety first: RDX only ever writes to a disk whose first sector starts
//! with the signature "RDXDISK1" (build.cmd creates such an image). Any
//! other disk — including every disk in a real PC — is left untouched.
//!
//! The on-disk format is deliberately simple: the whole set of files is
//! written as one snapshot. There are two snapshot slots, used alternately,
//! each with a header holding a generation number and checksum. A save
//! writes the *older* slot and only then its header, so if power is lost
//! mid-save the previous snapshot is still intact.
//!
//!   sector 0        "RDXDISK1"
//!   sector 1, 2     headers for slot 0 and slot 1
//!   sector 16...    slot 0 data, then slot 1 data (half the disk each)
//!
//! Two kinds of device can hold it. A SATA disk is found at boot and written
//! synchronously, as part of the operation that changed a file. A USB drive
//! is owned by the USB driver's thread, which may be plugged in at any time;
//! changes are flagged and that thread writes them out moments later.

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;

use crate::ahci;
use crate::sync::IrqLock;

pub const SECTOR: usize = 512;
const DISK_MAGIC: &[u8; 8] = b"RDXDISK1";
const SNAPSHOT_MAGIC: &[u8; 8] = b"RDXSNAP1";
const DATA_START: u64 = 16;

/// Anything that stores numbered 512-byte sectors.
pub trait BlockDevice {
    fn sectors(&self) -> u64;
    /// Read whole sectors starting at `lba`; `buf.len()` is a multiple of 512.
    fn read(&mut self, lba: u64, buf: &mut [u8]) -> bool;
    fn write(&mut self, lba: u64, buf: &[u8]) -> bool;
    /// Make the device commit anything it is still holding in a cache.
    fn flush(&mut self) -> bool;
}

/// The RDX snapshot format on some device.
pub struct Volume {
    slot_sectors: u64,
    generation: u64,
    /// Slot holding the newest snapshot; the next save goes to the other.
    active: u64,
    used_bytes: u64,
}

fn checksum(data: &[u8]) -> u64 {
    // FNV-1a
    let mut hash = 0xCBF2_9CE4_8422_2325u64;
    for &b in data {
        hash = (hash ^ b as u64).wrapping_mul(0x0000_0100_0000_01B3);
    }
    hash
}

fn u64_at(buf: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(buf[offset..offset + 8].try_into().unwrap())
}

/// Read and verify one snapshot slot. Returns (generation, data).
fn read_slot(device: &mut dyn BlockDevice, slot: u64, slot_sectors: u64) -> Option<(u64, Vec<u8>)> {
    let mut header = vec![0u8; SECTOR];
    if !device.read(1 + slot, &mut header) || &header[..8] != SNAPSHOT_MAGIC {
        return None;
    }
    let generation = u64_at(&header, 8);
    let length = u64_at(&header, 16);
    if length > slot_sectors * SECTOR as u64 {
        return None;
    }
    let padded = (length as usize).div_ceil(SECTOR) * SECTOR;
    let mut data = vec![0u8; padded];
    if padded > 0 && !device.read(DATA_START + slot * slot_sectors, &mut data) {
        return None;
    }
    data.truncate(length as usize);
    (checksum(&data) == u64_at(&header, 24)).then_some((generation, data))
}

fn parse(data: &[u8]) -> Vec<(String, Vec<u8>)> {
    let mut files = Vec::new();
    let mut pos = 0;
    while pos + 2 <= data.len() {
        let name_len = u16::from_le_bytes([data[pos], data[pos + 1]]) as usize;
        pos += 2;
        let Some(name) = data.get(pos..pos + name_len) else { break };
        pos += name_len;
        let Some(size) = data.get(pos..pos + 4) else { break };
        let size = u32::from_le_bytes(size.try_into().unwrap()) as usize;
        pos += 4;
        let Some(contents) = data.get(pos..pos + size) else { break };
        pos += size;
        files.push((String::from_utf8_lossy(name).into_owned(), contents.to_vec()));
    }
    files
}

/// Turn a set of files into the bytes of a snapshot.
pub fn serialize<'a>(files: impl Iterator<Item = (&'a String, &'a Vec<u8>)>) -> Vec<u8> {
    let mut data = Vec::new();
    for (name, contents) in files {
        data.extend_from_slice(&(name.len() as u16).to_le_bytes());
        data.extend_from_slice(name.as_bytes());
        data.extend_from_slice(&(contents.len() as u32).to_le_bytes());
        data.extend_from_slice(contents);
    }
    data
}

impl Volume {
    /// If the device carries the RDX signature, load its newest valid
    /// snapshot. Returns the volume and the files it held.
    pub fn mount(device: &mut dyn BlockDevice) -> Option<(Volume, Vec<(String, Vec<u8>)>)> {
        let mut first = vec![0u8; SECTOR];
        if device.sectors() < DATA_START + 16 || !device.read(0, &mut first) || &first[..8] != DISK_MAGIC {
            return None;
        }
        let slot_sectors = (device.sectors() - DATA_START) / 2;
        let mut best: Option<(u64, u64, Vec<u8>)> = None;
        for slot in 0..2 {
            if let Some((generation, data)) = read_slot(device, slot, slot_sectors) {
                if best.as_ref().map_or(true, |b| generation > b.0) {
                    best = Some((generation, slot, data));
                }
            }
        }
        let (generation, active, data) = best.unwrap_or((0, 1, Vec::new()));
        let volume = Volume { slot_sectors, generation, active, used_bytes: data.len() as u64 };
        Some((volume, parse(&data)))
    }

    /// Write a new snapshot (the output of `serialize`).
    pub fn save(&mut self, device: &mut dyn BlockDevice, mut data: Vec<u8>) -> Result<(), &'static str> {
        let length = data.len();
        if length as u64 > self.slot_sectors * SECTOR as u64 {
            return Err("disk full");
        }
        let sum = checksum(&data);
        data.resize(length.div_ceil(SECTOR) * SECTOR, 0);

        let slot = 1 - self.active;
        if !data.is_empty() && !device.write(DATA_START + slot * self.slot_sectors, &data) {
            return Err("disk write failed");
        }
        let mut header = vec![0u8; SECTOR];
        header[..8].copy_from_slice(SNAPSHOT_MAGIC);
        header[8..16].copy_from_slice(&(self.generation + 1).to_le_bytes());
        header[16..24].copy_from_slice(&(length as u64).to_le_bytes());
        header[24..32].copy_from_slice(&sum.to_le_bytes());
        if !device.write(1 + slot, &header) || !device.flush() {
            return Err("disk write failed");
        }
        self.generation += 1;
        self.active = slot;
        self.used_bytes = length as u64;
        Ok(())
    }

    /// Describe this volume for the `disk` command.
    pub fn info(&self, kind: &'static str, model: &str, sectors: u64) -> Info {
        Info {
            kind,
            model: String::from(model),
            size_bytes: sectors * SECTOR as u64,
            capacity_bytes: self.slot_sectors * SECTOR as u64,
            used_bytes: self.used_bytes,
            saves: self.generation,
        }
    }
}

#[derive(Clone)]
pub struct Info {
    /// "SATA" or "USB".
    pub kind: &'static str,
    pub model: String,
    pub size_bytes: u64,
    pub capacity_bytes: u64,
    pub used_bytes: u64,
    pub saves: u64,
}

/// The SATA disk in use, if there is one.
static SATA: IrqLock<Option<(ahci::Disk, Volume)>> = IrqLock::new(None);
/// Description of whichever disk is in use (SATA or USB).
static INFO: IrqLock<Option<Info>> = IrqLock::new(None);

pub fn set_info(info: Option<Info>) {
    *INFO.lock() = info;
}

pub fn info() -> Option<Info> {
    INFO.lock().clone()
}

/// Is any RDX disk in use (so that files are being saved)?
pub fn is_mounted() -> bool {
    INFO.lock().is_some()
}

/// Is the disk in use a SATA one, written synchronously by `save`?
pub fn sata_mounted() -> bool {
    SATA.lock().is_some()
}

pub enum Mount {
    /// No disk carrying the RDX signature is attached.
    NoDisk,
    /// Mounted; these are the files it held.
    Ready(Vec<(String, Vec<u8>)>),
}

/// Look for a SATA RDX disk and load its newest valid snapshot.
pub fn init() -> Mount {
    for mut disk in ahci::probe() {
        if let Some((volume, files)) = Volume::mount(&mut disk) {
            set_info(Some(volume.info("SATA", disk.model(), disk.sectors)));
            *SATA.lock() = Some((disk, volume));
            return Mount::Ready(files);
        }
    }
    Mount::NoDisk
}

/// Write a new snapshot containing exactly `files` to the SATA disk.
pub fn save<'a>(files: impl Iterator<Item = (&'a String, &'a Vec<u8>)>) -> Result<(), &'static str> {
    let mut guard = SATA.lock();
    let Some((disk, volume)) = guard.as_mut() else { return Ok(()) };
    volume.save(disk, serialize(files))?;
    set_info(Some(volume.info("SATA", disk.model(), disk.sectors)));
    Ok(())
}

/// The in-memory shape of the filesystem: file name -> contents.
pub type FileMap = BTreeMap<String, Vec<u8>>;
