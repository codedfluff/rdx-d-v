//! PCI configuration space access (legacy port mechanism) and device lookup.

use alloc::vec::Vec;

use crate::arch::{inl, outl};
use crate::sync::IrqLock;

/// Configuration access is two steps (address, then data) through ports
/// every CPU shares: one access at a time, or drivers starting on different
/// CPUs read each other's registers.
static CONFIG: IrqLock<()> = IrqLock::new(());

#[derive(Clone, Copy)]
pub struct Device {
    pub bus: u8,
    pub dev: u8,
    pub func: u8,
}

impl Device {
    fn address(&self, offset: u8) -> u32 {
        0x8000_0000
            | (self.bus as u32) << 16
            | (self.dev as u32) << 11
            | (self.func as u32) << 8
            | (offset as u32 & 0xFC)
    }

    pub fn read(&self, offset: u8) -> u32 {
        let _held = CONFIG.lock();
        outl(0xCF8, self.address(offset));
        inl(0xCFC)
    }

    pub fn write(&self, offset: u8, value: u32) {
        let _held = CONFIG.lock();
        outl(0xCF8, self.address(offset));
        outl(0xCFC, value);
    }

    /// Turn on memory-space decoding and bus mastering (DMA).
    pub fn enable(&self) {
        self.write(0x04, self.read(0x04) | 0x6);
    }

    /// Address of a memory-mapped BAR (32- or 64-bit), or None for an I/O
    /// port BAR or one the firmware left unassigned.
    pub fn bar_mmio(&self, index: u8) -> Option<u64> {
        let offset = 0x10 + index * 4;
        let low = self.read(offset);
        if low & 1 != 0 {
            return None;
        }
        let mut address = (low & 0xFFFF_FFF0) as u64;
        if (low >> 1) & 3 == 2 {
            address |= (self.read(offset + 4) as u64) << 32;
        }
        (address != 0).then_some(address)
    }
}

/// Every device whose class code (class << 16 | subclass << 8 | interface)
/// matches.
pub fn find(class_code: u32) -> Vec<Device> {
    let mut found = Vec::new();
    for bus in 0..=255u8 {
        for dev in 0..32u8 {
            let first = Device { bus, dev, func: 0 };
            if first.read(0) & 0xFFFF == 0xFFFF {
                continue;
            }
            let multifunction = first.read(0x0C) & 0x0080_0000 != 0;
            for func in 0..if multifunction { 8u8 } else { 1 } {
                let d = Device { bus, dev, func };
                if d.read(0) & 0xFFFF != 0xFFFF && d.read(0x08) >> 8 == class_code {
                    found.push(d);
                }
            }
        }
    }
    found
}
