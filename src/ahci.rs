//! AHCI (SATA) disk driver.
//!
//! Finds the AHCI controller on the PCI bus, takes over each port that has a
//! hard disk attached, and reads/writes sectors with DMA commands. Commands
//! are issued one at a time and polled to completion, so the driver works
//! with interrupts off and needs no IRQ handler.

use alloc::vec;
use alloc::vec::Vec;

use crate::{memory, pci};

use crate::disk::{BlockDevice, SECTOR};

/// Locate the first AHCI controller (class 01, subclass 06, interface 01)
/// and return the address of its memory-mapped registers (BAR 5).
fn find_controller() -> Option<u64> {
    for device in pci::find(0x01_06_01) {
        device.enable();
        if let Some(registers) = device.bar_mmio(5).and_then(|abar| memory::map_mmio(abar, 0x2000)) {
            return Some(registers);
        }
    }
    None
}

// ---------------------------------------------------------------- registers

const HBA_GHC: u64 = 0x04;
const HBA_PORTS_IMPLEMENTED: u64 = 0x0C;

const PORT_CLB: u64 = 0x00;
const PORT_FB: u64 = 0x08;
const PORT_IS: u64 = 0x10;
const PORT_IE: u64 = 0x14;
const PORT_CMD: u64 = 0x18;
const PORT_TFD: u64 = 0x20;
const PORT_SIG: u64 = 0x24;
const PORT_SSTS: u64 = 0x28;
const PORT_SERR: u64 = 0x30;
const PORT_CI: u64 = 0x38;

const CMD_ST: u32 = 1 << 0;
const CMD_FRE: u32 = 1 << 4;
const CMD_FR: u32 = 1 << 14;
const CMD_CR: u32 = 1 << 15;

const ATA_IDENTIFY: u8 = 0xEC;
const ATA_READ_DMA_EXT: u8 = 0x25;
const ATA_WRITE_DMA_EXT: u8 = 0x35;
const ATA_FLUSH_CACHE_EXT: u8 = 0xEA;

const SPIN_LIMIT: u32 = 5_000_000;

unsafe fn rd(base: u64, offset: u64) -> u32 {
    ((base + offset) as *const u32).read_volatile()
}

unsafe fn wr(base: u64, offset: u64, value: u32) {
    ((base + offset) as *mut u32).write_volatile(value)
}

/// Spin until `done()` is true. False means it never happened.
fn wait_for(mut done: impl FnMut() -> bool) -> bool {
    for _ in 0..SPIN_LIMIT {
        if done() {
            return true;
        }
        core::hint::spin_loop();
    }
    false
}

// ---------------------------------------------------------------- disk

pub struct Disk {
    port: u64,
    /// One page of DMA memory: command list at +0, received-FIS area at
    /// +0x400, command table at +0x800.
    mem: u64,
    pub sectors: u64,
    pub model: [u8; 40],
}

impl Disk {
    /// Run one ATA command in slot 0 and wait for it to finish.
    unsafe fn command(&self, cmd: u8, lba: u64, count: u16, buf: *mut u8, bytes: usize, write: bool) -> bool {
        let header = self.mem as *mut u32;
        let table = self.mem + 0x800;
        core::ptr::write_bytes(table as *mut u8, 0, 0x90);

        // Host-to-device register FIS.
        let fis = table as *mut u8;
        fis.add(0).write(0x27);
        fis.add(1).write(0x80); // this FIS carries a command
        fis.add(2).write(cmd);
        fis.add(4).write(lba as u8);
        fis.add(5).write((lba >> 8) as u8);
        fis.add(6).write((lba >> 16) as u8);
        fis.add(7).write(0x40); // LBA addressing
        fis.add(8).write((lba >> 24) as u8);
        fis.add(9).write((lba >> 32) as u8);
        fis.add(10).write((lba >> 40) as u8);
        fis.add(12).write(count as u8);
        fis.add(13).write((count >> 8) as u8);

        // One physical-region descriptor covering the whole buffer.
        let has_data = bytes > 0;
        if has_data {
            let prd = (table + 0x80) as *mut u32;
            prd.add(0).write(buf as u64 as u32);
            prd.add(1).write((buf as u64 >> 32) as u32);
            prd.add(2).write(0);
            prd.add(3).write(bytes as u32 - 1);
        }

        // Command header: FIS length 5 dwords, direction, descriptor count.
        header.add(0).write_volatile(5 | if write { 1 << 6 } else { 0 } | (has_data as u32) << 16);
        header.add(1).write_volatile(0);
        header.add(2).write_volatile(table as u32);
        header.add(3).write_volatile((table >> 32) as u32);

        let port = self.port;
        if !wait_for(|| rd(port, PORT_TFD) & 0x88 == 0) {
            return false; // device stuck busy
        }
        wr(port, PORT_IS, 0xFFFF_FFFF);
        wr(port, PORT_CI, 1);
        let mut failed = false;
        let finished = wait_for(|| {
            if rd(port, PORT_IS) & (1 << 30) != 0 {
                failed = true; // task file error
                return true;
            }
            rd(port, PORT_CI) & 1 == 0
        });
        finished && !failed && rd(port, PORT_TFD) & 0x01 == 0
    }

    fn transfer(&self, lba: u64, buf: *mut u8, len: usize, write: bool) -> bool {
        if len % SECTOR != 0 || lba + (len / SECTOR) as u64 > self.sectors {
            return false;
        }
        // The kernel heap is physically contiguous and identity mapped, so a
        // buffer's address is also its DMA address. 512 KiB per command.
        const CHUNK: usize = 1024 * SECTOR;
        let mut done = 0;
        while done < len {
            let n = (len - done).min(CHUNK);
            let cmd = if write { ATA_WRITE_DMA_EXT } else { ATA_READ_DMA_EXT };
            let ok = unsafe {
                self.command(cmd, lba + (done / SECTOR) as u64, (n / SECTOR) as u16, buf.add(done), n, write)
            };
            if !ok {
                return false;
            }
            done += n;
        }
        true
    }

    pub fn model(&self) -> &str {
        core::str::from_utf8(&self.model).unwrap_or("?").trim()
    }
}

impl BlockDevice for Disk {
    fn sectors(&self) -> u64 {
        self.sectors
    }

    fn read(&mut self, lba: u64, buf: &mut [u8]) -> bool {
        self.transfer(lba, buf.as_mut_ptr(), buf.len(), false)
    }

    fn write(&mut self, lba: u64, buf: &[u8]) -> bool {
        self.transfer(lba, buf.as_ptr() as *mut u8, buf.len(), true)
    }

    /// Ask the drive to commit its write cache to the platter/flash.
    fn flush(&mut self) -> bool {
        unsafe { self.command(ATA_FLUSH_CACHE_EXT, 0, 0, core::ptr::null_mut(), 0, false) }
    }
}

/// Stop a port, point it at our own command structures, and restart it.
unsafe fn take_over_port(port: u64, mem: u64) -> bool {
    wr(port, PORT_CMD, rd(port, PORT_CMD) & !CMD_ST);
    if !wait_for(|| rd(port, PORT_CMD) & CMD_CR == 0) {
        return false;
    }
    wr(port, PORT_CMD, rd(port, PORT_CMD) & !CMD_FRE);
    if !wait_for(|| rd(port, PORT_CMD) & CMD_FR == 0) {
        return false;
    }
    wr(port, PORT_CLB, mem as u32);
    wr(port, PORT_CLB + 4, (mem >> 32) as u32);
    wr(port, PORT_FB, (mem + 0x400) as u32);
    wr(port, PORT_FB + 4, ((mem + 0x400) >> 32) as u32);
    wr(port, PORT_SERR, 0xFFFF_FFFF);
    wr(port, PORT_IS, 0xFFFF_FFFF);
    wr(port, PORT_IE, 0); // we poll
    wr(port, PORT_CMD, rd(port, PORT_CMD) | CMD_FRE);
    wr(port, PORT_CMD, rd(port, PORT_CMD) | CMD_ST);
    true
}

/// Find every SATA hard disk and get it ready for I/O.
pub fn probe() -> Vec<Disk> {
    let mut disks = Vec::new();
    let Some(abar) = find_controller() else { return disks };
    unsafe {
        wr(abar, HBA_GHC, rd(abar, HBA_GHC) | 1 << 31); // AHCI mode
        let implemented = rd(abar, HBA_PORTS_IMPLEMENTED);
        for i in 0..32u64 {
            if implemented & (1 << i) == 0 {
                continue;
            }
            let port = abar + 0x100 + i * 0x80;
            // Device detected with an established link, and it is a plain
            // SATA disk (not a CD drive or port multiplier).
            if rd(port, PORT_SSTS) & 0xF != 3 || rd(port, PORT_SIG) != 0x0000_0101 {
                continue;
            }
            let Some(mem) = memory::try_alloc_zeroed_frame() else { break };
            if !take_over_port(port, mem) {
                continue;
            }
            let mut disk = Disk { port, mem, sectors: 0, model: [b' '; 40] };
            let mut id = vec![0u8; SECTOR];
            if !disk.command(ATA_IDENTIFY, 0, 0, id.as_mut_ptr(), SECTOR, false) {
                continue;
            }
            let word = |n: usize| u16::from_le_bytes([id[n * 2], id[n * 2 + 1]]) as u64;
            disk.sectors = word(100) | word(101) << 16 | word(102) << 32 | word(103) << 48;
            // The model name is stored with each pair of characters swapped.
            for k in 0..20 {
                disk.model[k * 2] = id[54 + k * 2 + 1];
                disk.model[k * 2 + 1] = id[54 + k * 2];
            }
            if disk.sectors > 0 {
                disks.push(disk);
            }
        }
    }
    disks
}
