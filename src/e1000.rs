//! Driver for Intel gigabit Ethernet adapters of the e1000 family.
//!
//! The adapter and driver share two rings of descriptors in memory, each
//! descriptor pointing at a packet buffer. To send, the driver fills a
//! buffer and moves the transmit ring's tail; to receive, it looks for
//! descriptors the adapter has marked done. Nothing here uses interrupts:
//! the network thread polls.
//!
//! Tested against QEMU's `e1000` (82540EM) and `e1000e` (82574L) models.
//! Other adapters in the family are attempted with the same code but are
//! unverified.

use alloc::vec::Vec;

use crate::{memory, pci};

const REG_CTRL: u64 = 0x0000;
const REG_STATUS: u64 = 0x0008;
const REG_IMC: u64 = 0x00D8;
const REG_RCTL: u64 = 0x0100;
const REG_TCTL: u64 = 0x0400;
const REG_TIPG: u64 = 0x0410;
const REG_RDBAL: u64 = 0x2800;
const REG_RDLEN: u64 = 0x2808;
const REG_RDH: u64 = 0x2810;
const REG_RDT: u64 = 0x2818;
const REG_TDBAL: u64 = 0x3800;
const REG_TDLEN: u64 = 0x3808;
const REG_TDH: u64 = 0x3810;
const REG_TDT: u64 = 0x3818;
const REG_MTA: u64 = 0x5200;
const REG_RAL: u64 = 0x5400;

/// Descriptors per ring (16 bytes each: 256 fill the one page a ring has).
/// The receive ring must hold a whole TCP window's worth of packets
/// (64 KiB is about 45), since it is emptied only every few milliseconds:
/// anything that arrives while it is full is dropped, and TCP then stalls
/// waiting to send it again.
const RING: usize = 256;
const BUFFER: usize = 2048;

/// Intel device IDs this driver will try.
const DEVICES: &[u16] = &[
    0x100E, 0x100F, 0x1004, 0x1010, 0x1011, 0x1026, 0x1027, 0x1076, 0x107C, // 8254x
    0x10D3, 0x10EA, 0x10EF, 0x1502, 0x1503, // 8257x / 8258x
    0x153A, 0x153B, 0x155A, 0x1559, 0x15A0, 0x15A1, 0x15A2, 0x15A3, // I217 / I218
    0x156F, 0x1570, 0x15B7, 0x15B8, 0x15D7, 0x15D8, 0x15E3, 0x15BB, 0x15BC, 0x15BD, 0x15BE, // I219
];

pub struct Nic {
    regs: u64,
    pub mac: [u8; 6],
    pub model: u16,
    rx_ring: u64,
    tx_ring: u64,
    rx_buffers: [u64; RING],
    tx_buffers: [u64; RING],
    rx_next: usize,
    tx_next: usize,
}

unsafe impl Send for Nic {}

impl Nic {
    fn read(&self, reg: u64) -> u32 {
        unsafe { ((self.regs + reg) as *const u32).read_volatile() }
    }

    fn write(&self, reg: u64, value: u32) {
        unsafe { ((self.regs + reg) as *mut u32).write_volatile(value) }
    }

    /// Find the first supported adapter and bring it up.
    pub fn probe() -> Option<Nic> {
        let device = pci::find(0x02_00_00).into_iter().find(|d| {
            let id = d.read(0);
            id & 0xFFFF == 0x8086 && DEVICES.contains(&((id >> 16) as u16))
        })?;
        device.enable();
        let regs = memory::map_mmio(device.bar_mmio(0)?, 0x20000)?;
        let mut nic = Nic {
            regs,
            mac: [0; 6],
            model: (device.read(0) >> 16) as u16,
            rx_ring: memory::try_alloc_zeroed_frame()?,
            tx_ring: memory::try_alloc_zeroed_frame()?,
            rx_buffers: [0; RING],
            tx_buffers: [0; RING],
            rx_next: 0,
            tx_next: 0,
        };
        for i in 0..RING {
            nic.rx_buffers[i] = memory::try_alloc_zeroed_frame()?;
            nic.tx_buffers[i] = memory::try_alloc_zeroed_frame()?;
        }

        // Quiet, reset, quiet again (reset re-enables interrupts).
        nic.write(REG_IMC, 0xFFFF_FFFF);
        nic.write(REG_CTRL, nic.read(REG_CTRL) | 1 << 26);
        for _ in 0..100_000 {
            if nic.read(REG_CTRL) & (1 << 26) == 0 {
                break;
            }
        }
        nic.write(REG_IMC, 0xFFFF_FFFF);
        // Set link up, auto-detect speed.
        nic.write(REG_CTRL, nic.read(REG_CTRL) | 1 << 6 | 1 << 5);

        // The adapter loads its address from its EEPROM into the first
        // receive-address register pair.
        let (low, high) = (nic.read(REG_RAL), nic.read(REG_RAL + 4));
        nic.mac = [low as u8, (low >> 8) as u8, (low >> 16) as u8, (low >> 24) as u8, high as u8, (high >> 8) as u8];
        if nic.mac == [0; 6] || nic.mac == [0xFF; 6] {
            return None;
        }
        for i in 0..128 {
            nic.write(REG_MTA + i * 4, 0); // no multicast groups
        }

        // Receive ring: every descriptor gets a buffer; the adapter owns all
        // but one of them (head == tail would mean "empty").
        unsafe {
            for i in 0..RING {
                ((nic.rx_ring + i as u64 * 16) as *mut u64).write_volatile(nic.rx_buffers[i]);
            }
        }
        nic.write(REG_RDBAL, nic.rx_ring as u32);
        nic.write(REG_RDBAL + 4, (nic.rx_ring >> 32) as u32);
        nic.write(REG_RDLEN, (RING * 16) as u32);
        nic.write(REG_RDH, 0);
        nic.write(REG_RDT, RING as u32 - 1);
        // Enable; accept broadcasts; strip the checksum; 2048-byte buffers.
        nic.write(REG_RCTL, 1 << 1 | 1 << 15 | 1 << 26);

        nic.write(REG_TDBAL, nic.tx_ring as u32);
        nic.write(REG_TDBAL + 4, (nic.tx_ring >> 32) as u32);
        nic.write(REG_TDLEN, (RING * 16) as u32);
        nic.write(REG_TDH, 0);
        nic.write(REG_TDT, 0);
        // Enable; pad short packets; standard collision settings.
        nic.write(REG_TCTL, 1 << 1 | 1 << 3 | 0x10 << 4 | 0x40 << 12);
        nic.write(REG_TIPG, 0x0060_200A);
        Some(nic)
    }

    pub fn link_up(&self) -> bool {
        self.read(REG_STATUS) & 2 != 0
    }

    /// Queue one Ethernet frame (without checksum) for sending. False if the
    /// frame is too big or the adapter has not finished with the slot yet.
    pub fn send(&mut self, frame: &[u8]) -> bool {
        if frame.len() > 1514 {
            return false;
        }
        let i = self.tx_next;
        let descriptor = (self.tx_ring + i as u64 * 16) as *mut u8;
        unsafe {
            // A slot that has been used before must be marked done (bit 0
            // of its status byte) before it can be reused.
            let used = (descriptor.add(8) as *const u16).read_volatile() != 0;
            if used && descriptor.add(12).read_volatile() & 1 == 0 {
                return false;
            }
            core::ptr::copy_nonoverlapping(frame.as_ptr(), self.tx_buffers[i] as *mut u8, frame.len());
            (descriptor as *mut u64).write_volatile(self.tx_buffers[i]);
            (descriptor.add(8) as *mut u16).write_volatile(frame.len() as u16);
            descriptor.add(10).write_volatile(0);
            descriptor.add(11).write_volatile(0b1011); // end of packet, add checksum, report status
            descriptor.add(12).write_volatile(0);
        }
        self.tx_next = (i + 1) % RING;
        self.write(REG_TDT, self.tx_next as u32);
        true
    }

    /// The next received frame, if one is waiting.
    pub fn receive(&mut self) -> Option<Vec<u8>> {
        let i = self.rx_next;
        let descriptor = (self.rx_ring + i as u64 * 16) as *mut u8;
        unsafe {
            let status = descriptor.add(12).read_volatile();
            if status & 1 == 0 {
                return None; // not filled in yet
            }
            let length = ((descriptor.add(8) as *const u16).read_volatile() as usize).min(BUFFER);
            let errors = descriptor.add(13).read_volatile();
            let frame = core::slice::from_raw_parts(self.rx_buffers[i] as *const u8, length).to_vec();
            // Hand the descriptor back to the adapter.
            descriptor.add(12).write_volatile(0);
            self.write(REG_RDT, i as u32);
            self.rx_next = (i + 1) % RING;
            // Whole frames only (bit 1 = end of packet), and no errors.
            if status & 2 != 0 && errors == 0 { Some(frame) } else { Some(Vec::new()) }
        }
    }
}
