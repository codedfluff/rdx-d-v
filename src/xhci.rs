//! USB keyboards, through an xHCI (USB 3) host controller driver.
//!
//! One kernel thread owns all of this. At start-up it takes each xHCI
//! controller away from the firmware, resets it and sets up the rings the
//! controller and driver talk through. Then, forever, every 10 ms it:
//!
//!   * looks for newly plugged-in devices, gives each an address, reads its
//!     descriptors and, if it is a keyboard, switches it to the simple
//!     "boot protocol" and starts asking for key reports;
//!   * turns arriving 8-byte key reports into characters on the kernel's
//!     input queue, the same one the PS/2 keyboard feeds.
//!
//! The controller is polled rather than interrupt driven, so nothing here
//! depends on how the machine's interrupts happen to be wired.
//!
//! It also drives USB mass storage (flash drives, external disks): it reads
//! each drive's size, and one that carries the RDX disk signature is mounted
//! and has changed files written to it by this same thread.
//!
//! Limits: devices plugged in through an external hub are not seen (there
//! is no hub driver), and only xHCI controllers are supported, which covers
//! PCs from roughly 2012 onwards.

use alloc::format;
use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;
use core::sync::atomic::Ordering;

use crate::sync::IrqLock;
use crate::disk::{self, BlockDevice, Volume, SECTOR};
use crate::{arch, fs, input, keyboard, memory, pci, sched, serial};

// ---------------------------------------------------------------- status log

static STATUS: IrqLock<Vec<String>> = IrqLock::new(Vec::new());

fn log(message: String) {
    serial::write_str("[usb] ");
    serial::write_str(&message);
    serial::write_str("\n");
    let mut status = STATUS.lock();
    if status.len() < 40 {
        status.push(message);
    }
}

/// What the USB driver has found so far, for the shell's `usb` command.
pub fn status() -> Vec<String> {
    STATUS.lock().clone()
}

// ---------------------------------------------------------------- helpers

unsafe fn rd32(address: u64) -> u32 {
    (address as *const u32).read_volatile()
}

unsafe fn wr32(address: u64, value: u32) {
    (address as *mut u32).write_volatile(value)
}

unsafe fn wr64(address: u64, value: u64) {
    wr32(address, value as u32);
    wr32(address + 4, (value >> 32) as u32);
}

/// Poll `done` for up to `ms` milliseconds: a short busy spin first (most
/// things finish at once), then sleeping between checks.
fn wait_until(ms: u64, mut done: impl FnMut() -> bool) -> bool {
    for _ in 0..2000 {
        if done() {
            return true;
        }
        core::hint::spin_loop();
    }
    let deadline = arch::ticks() + sched::ms_to_ticks(ms) + 1;
    while arch::ticks() < deadline {
        sched::sleep_ms(10);
        if done() {
            return true;
        }
    }
    false
}

// Transfer request block (TRB) types.
const TRB_NORMAL: u32 = 1;
const TRB_SETUP: u32 = 2;
const TRB_DATA: u32 = 3;
const TRB_STATUS: u32 = 4;
const TRB_LINK: u32 = 6;
const TRB_ENABLE_SLOT: u32 = 9;
const TRB_DISABLE_SLOT: u32 = 10;
const TRB_ADDRESS_DEVICE: u32 = 11;
const TRB_CONFIGURE_ENDPOINT: u32 = 12;
const TRB_EVALUATE_CONTEXT: u32 = 13;
const EVENT_TRANSFER: u32 = 32;
const EVENT_COMMAND: u32 = 33;

const RING_SIZE: usize = 256; // 16-byte TRBs in one 4 KiB page

/// A ring the driver writes and the controller reads (commands, transfers).
/// The last entry is a link back to the start; the "cycle" bit tells the
/// controller which entries are new.
struct Ring {
    base: u64,
    index: usize,
    cycle: u32,
}

impl Ring {
    fn push(&mut self, param: u64, status: u32, control: u32) {
        unsafe {
            let trb = (self.base + self.index as u64 * 16) as *mut u32;
            trb.add(0).write_volatile(param as u32);
            trb.add(1).write_volatile((param >> 32) as u32);
            trb.add(2).write_volatile(status);
            trb.add(3).write_volatile(control | self.cycle);
            self.index += 1;
            if self.index == RING_SIZE - 1 {
                let link = (self.base + self.index as u64 * 16) as *mut u32;
                link.add(0).write_volatile(self.base as u32);
                link.add(1).write_volatile((self.base >> 32) as u32);
                link.add(2).write_volatile(0);
                link.add(3).write_volatile(TRB_LINK << 10 | 1 << 1 | self.cycle); // toggle cycle
                self.index = 0;
                self.cycle ^= 1;
            }
        }
    }
}

#[derive(Clone, Copy)]
struct Event {
    status: u32,
    control: u32,
}

impl Event {
    fn kind(&self) -> u32 {
        (self.control >> 10) & 0x3F
    }
    /// Completion code: 1 = success, 13 = short packet (also fine).
    fn code(&self) -> u32 {
        self.status >> 24
    }
    fn ok(&self) -> bool {
        self.code() == 1 || self.code() == 13
    }
    fn slot(&self) -> u8 {
        (self.control >> 24) as u8
    }
    fn endpoint(&self) -> u8 {
        ((self.control >> 16) & 0x1F) as u8
    }
}

// ---------------------------------------------------------------- key translation

const LOWER: &[u8; 53] = b"abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\x08\t -=[]\\\0;'`,./";
const UPPER: &[u8; 53] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\x08\t _+{}|\0:\"~<>?";

/// HID keyboard usage code -> the byte RDX programs see (0 = no character).
fn translate(usage: u8, shift: bool, ctrl: bool, caps: bool) -> u8 {
    match usage {
        4..=56 => {
            let i = (usage - 4) as usize;
            let mut ch = if shift { UPPER[i] } else { LOWER[i] };
            if caps && ch.is_ascii_alphabetic() {
                ch ^= 0x20;
            }
            if ctrl && ch.is_ascii_alphabetic() {
                ch &= 0x1F;
            }
            ch
        }
        74 => keyboard::KEY_HOME,
        75 => keyboard::KEY_PAGE_UP,
        76 => keyboard::KEY_DELETE,
        77 => keyboard::KEY_END,
        78 => keyboard::KEY_PAGE_DOWN,
        79 => keyboard::KEY_RIGHT,
        80 => keyboard::KEY_LEFT,
        81 => keyboard::KEY_DOWN,
        82 => keyboard::KEY_UP,
        // Numeric keypad
        84 => b'/',
        85 => b'*',
        86 => b'-',
        87 => b'+',
        88 => b'\n',
        89..=97 => b'1' + (usage - 89),
        98 => b'0',
        99 => b'.',
        _ => 0,
    }
}

#[derive(Clone, Copy, PartialEq)]
enum HidKind {
    Keyboard,
    Mouse,
    Tablet,
}

/// QEMU's "usb-tablet": its reports are buttons, then x and y as 16-bit
/// numbers from 0 to 0x7FFF, then the wheel. Other tablets lay their
/// reports out in their own ways (described by a report descriptor, which
/// is not parsed here), so only this one is recognised, by its IDs.
const QEMU_TABLET: (u16, u16) = (0x0627, 0x0001);

struct Keyboard {
    slot: u8,
    port: usize,
    /// Device context index of its interrupt endpoint.
    dci: u8,
    ring: Ring,
    /// Page the controller writes each 8-byte report into.
    buffer: u64,
    last: [u8; 8],
    /// What it is: a keyboard, a (boot protocol) mouse, or QEMU's virtual
    /// tablet, which reports absolute positions.
    kind: HidKind,
    // Auto-repeat of the most recently pressed key.
    repeat_usage: u8,
    repeat_char: u8,
    repeat_at: u64,
    dead: bool,
}

impl Keyboard {
    /// A boot-protocol report: modifier bits, a reserved byte, then up to
    /// six usage codes of keys currently held down.
    fn report(&mut self, report: [u8; 8]) {
        match self.kind {
            HidKind::Mouse => {
                // Boot mouse report: buttons, then x and y movement.
                crate::display::mouse(report[1] as i8 as i32, report[2] as i8 as i32, (report[0] & 7) as u32);
                return;
            }
            HidKind::Tablet => {
                let x = u16::from_le_bytes([report[1], report[2]]) as u32;
                let y = u16::from_le_bytes([report[3], report[4]]) as u32;
                crate::display::mouse_absolute(x, y, 0x7FFF, (report[0] & 7) as u32);
                return;
            }
            HidKind::Keyboard => {}
        }
        if report[2] == 1 {
            return; // too many keys held: the keyboard can't tell which
        }
        let shift = report[0] & 0x22 != 0;
        let ctrl = report[0] & 0x11 != 0;
        for &usage in &report[2..8] {
            if usage < 4 || self.last[2..8].contains(&usage) {
                continue; // nothing, or already down last time
            }
            if usage == 57 {
                keyboard::caps_lock().fetch_xor(true, Ordering::Relaxed);
                continue;
            }
            let ch = translate(usage, shift, ctrl, keyboard::caps_lock().load(Ordering::Relaxed));
            if ch != 0 {
                input::push(ch);
                self.repeat_usage = usage;
                self.repeat_char = ch;
                self.repeat_at = arch::ticks() + sched::ms_to_ticks(400);
            }
        }
        if !report[2..8].contains(&self.repeat_usage) {
            self.repeat_usage = 0;
        }
        self.last = report;
    }
}

// ---------------------------------------------------------------- drives

/// A USB mass-storage device (flash drive, external disk).
struct Drive {
    slot: u8,
    port: usize,
    dci_in: u8,
    dci_out: u8,
    ring_in: Ring,
    ring_out: Ring,
    /// Page holding the command wrapper (at 0) and status wrapper (at 64).
    control: u64,
    /// Page that data is transferred through.
    data: u64,
    sectors: u64,
    tag: u32,
    name: String,
    /// The RDX filesystem on it, if it has one and we are using it.
    volume: Option<Volume>,
}

/// Lets the generic disk code read and write a drive through its controller.
struct DriveIo<'a> {
    controller: &'a mut Controller,
    drive: usize,
}

impl BlockDevice for DriveIo<'_> {
    fn sectors(&self) -> u64 {
        self.controller.drives[self.drive].sectors
    }

    fn read(&mut self, lba: u64, buf: &mut [u8]) -> bool {
        self.controller.drive_transfer(self.drive, lba, buf.as_mut_ptr(), buf.len(), false)
    }

    fn write(&mut self, lba: u64, buf: &[u8]) -> bool {
        self.controller.drive_transfer(self.drive, lba, buf.as_ptr() as *mut u8, buf.len(), true)
    }

    fn flush(&mut self) -> bool {
        // SYNCHRONIZE CACHE(10). Many flash drives have no cache and reject
        // it, which is fine.
        self.controller.scsi(self.drive, &[0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0], 0, true);
        true
    }
}

// ---------------------------------------------------------------- controller

// Operational register offsets.
const OP_USBCMD: u64 = 0x00;
const OP_USBSTS: u64 = 0x04;
const OP_CRCR: u64 = 0x18;
const OP_DCBAAP: u64 = 0x30;
const OP_CONFIG: u64 = 0x38;
const OP_PORTS: u64 = 0x400;

// Port status bits.
const PORT_CONNECTED: u32 = 1 << 0;
const PORT_ENABLED: u32 = 1 << 1;
const PORT_RESET: u32 = 1 << 4;
const PORT_POWER: u32 = 1 << 9;
const PORT_RESET_CHANGE: u32 = 1 << 21;
/// Every write-1-to-clear "something changed" bit.
const PORT_CHANGE_BITS: u32 = 0x7F << 17;

struct Controller {
    name: String,
    op: u64,
    doorbells: u64,
    interrupter: u64,
    ports: usize,
    /// Size of one context structure: 32 or 64 bytes.
    context_size: u64,
    /// Can the controller use memory above 4 GiB?
    wide_addresses: bool,
    commands: Ring,
    events: u64,
    event_index: usize,
    event_cycle: u32,
    /// Table of per-device context pointers, indexed by slot.
    dcbaa: u64,
    /// One page for descriptor reads.
    scratch: u64,
    /// Per port: 0 = empty, otherwise something is attached and handled.
    port_state: Vec<u8>,
    port_slot: Vec<u8>,
    keyboards: Vec<Keyboard>,
    drives: Vec<Drive>,
}

impl Controller {
    fn page(&self) -> Option<u64> {
        let frame = memory::try_alloc_zeroed_frame()?;
        (self.wide_addresses || frame < 1 << 32).then_some(frame)
    }

    fn ring(&self) -> Option<Ring> {
        Some(Ring { base: self.page()?, index: 0, cycle: 1 })
    }

    /// Take over the controller at this PCI location and start it.
    fn start(device: pci::Device) -> Result<Controller, &'static str> {
        device.enable();
        let physical = device.bar_mmio(0).ok_or("no memory-mapped registers")?;
        // All register groups live in this one region (64 KiB on most
        // controllers); map generously.
        let base = memory::map_mmio(physical, 0x10_0000).ok_or("could not map its registers")?;
        unsafe {
            let cap_length = rd32(base) & 0xFF;
            let params1 = rd32(base + 0x04);
            let params2 = rd32(base + 0x08);
            let cap_params = rd32(base + 0x10);
            let op = base + cap_length as u64;
            let doorbells = base + (rd32(base + 0x14) & !3) as u64;
            let runtime = base + (rd32(base + 0x18) & !0x1F) as u64;

            // Ask the firmware to let go (it may be emulating a PS/2
            // keyboard on top of this controller), and silence its SMIs.
            let mut ext = ((cap_params >> 16) as u64) * 4;
            while ext != 0 {
                let cap = rd32(base + ext);
                if cap & 0xFF == 1 {
                    wr32(base + ext, cap | 1 << 24);
                    wait_until(1000, || rd32(base + ext) & (1 << 16) == 0);
                    let control = rd32(base + ext + 4);
                    wr32(base + ext + 4, (control & !0x0000_E011) | 0xE000_0000);
                }
                let next = ((cap >> 8) & 0xFF) as u64;
                if next == 0 {
                    break;
                }
                ext += next * 4;
            }

            // Stop, then reset.
            wr32(op + OP_USBCMD, rd32(op + OP_USBCMD) & !1);
            if !wait_until(200, || rd32(op + OP_USBSTS) & 1 != 0) {
                return Err("controller would not stop");
            }
            wr32(op + OP_USBCMD, 1 << 1);
            if !wait_until(1000, || rd32(op + OP_USBCMD) & (1 << 1) == 0 && rd32(op + OP_USBSTS) & (1 << 11) == 0) {
                return Err("controller did not come out of reset");
            }

            let max_slots = params1 & 0xFF;
            let ports = (params1 >> 24) as usize;
            let mut c = Controller {
                name: format!("{:02x}:{:02x}.{}", device.bus, device.dev, device.func),
                op,
                doorbells,
                interrupter: runtime + 0x20,
                ports,
                context_size: if cap_params & (1 << 2) != 0 { 64 } else { 32 },
                wide_addresses: cap_params & 1 != 0,
                commands: Ring { base: 0, index: 0, cycle: 1 },
                events: 0,
                event_index: 0,
                event_cycle: 1,
                dcbaa: 0,
                scratch: 0,
                port_state: vec![0; ports],
                port_slot: vec![0; ports],
                keyboards: Vec::new(),
                drives: Vec::new(),
            };
            const OOM: &str = "out of suitable memory";
            c.dcbaa = c.page().ok_or(OOM)?;
            c.scratch = c.page().ok_or(OOM)?;
            c.commands = c.ring().ok_or(OOM)?;
            c.events = c.page().ok_or(OOM)?;
            let segment_table = c.page().ok_or(OOM)?;

            // Some controllers want pages of their own to scribble in.
            let scratchpads = (((params2 >> 21) & 0x1F) << 5 | (params2 >> 27) & 0x1F) as u64;
            if scratchpads > 512 {
                return Err("controller wants too many scratchpad pages");
            }
            if scratchpads > 0 {
                let array = c.page().ok_or(OOM)?;
                for i in 0..scratchpads {
                    ((array + i * 8) as *mut u64).write_volatile(c.page().ok_or(OOM)?);
                }
                (c.dcbaa as *mut u64).write_volatile(array);
            }

            wr32(op + OP_CONFIG, max_slots);
            wr64(op + OP_DCBAAP, c.dcbaa);
            wr64(op + OP_CRCR, c.commands.base | 1);
            // One event ring segment, no interrupts: we poll it.
            (segment_table as *mut u64).write_volatile(c.events);
            ((segment_table + 8) as *mut u32).write_volatile(RING_SIZE as u32);
            wr32(c.interrupter + 0x08, 1);
            wr64(c.interrupter + 0x18, c.events);
            wr64(c.interrupter + 0x10, segment_table);

            wr32(op + OP_USBCMD, 1); // run
            if !wait_until(200, || rd32(op + OP_USBSTS) & 1 == 0) {
                return Err("controller would not start");
            }
            for port in 0..ports {
                let reg = c.port_register(port);
                if rd32(reg) & PORT_POWER == 0 {
                    wr32(reg, PORT_POWER);
                }
            }
            Ok(c)
        }
    }

    fn port_register(&self, port: usize) -> u64 {
        self.op + OP_PORTS + 0x10 * port as u64
    }

    fn ring_doorbell(&self, slot: u8, target: u32) {
        unsafe { wr32(self.doorbells + 4 * slot as u64, target) };
    }

    /// Pointer to context number `index` within a device or input context.
    fn context(&self, base: u64, index: u64) -> *mut u32 {
        (base + index * self.context_size) as *mut u32
    }

    fn next_event(&mut self) -> Option<Event> {
        unsafe {
            let trb = (self.events + self.event_index as u64 * 16) as *const u32;
            let control = trb.add(3).read_volatile();
            if control & 1 != self.event_cycle {
                return None; // nothing new
            }
            let event = Event { status: trb.add(2).read_volatile(), control };
            self.event_index += 1;
            if self.event_index == RING_SIZE {
                self.event_index = 0;
                self.event_cycle ^= 1;
            }
            // Tell the controller how far we have read.
            wr64(self.interrupter + 0x18, (self.events + self.event_index as u64 * 16) | 1 << 3);
            Some(event)
        }
    }

    /// An event nobody is waiting for: a key report, most likely.
    fn handle_unsolicited(&mut self, event: Event) {
        if event.kind() != EVENT_TRANSFER {
            return;
        }
        let Some(i) = self.keyboards.iter().position(|k| k.slot == event.slot() && k.dci == event.endpoint())
        else {
            return;
        };
        if !event.ok() {
            if !self.keyboards[i].dead {
                self.keyboards[i].dead = true;
                log(format!("keyboard on port {} stopped responding (code {})", self.keyboards[i].port + 1, event.code()));
            }
            return;
        }
        let k = &mut self.keyboards[i];
        let mut report = [0u8; 8];
        unsafe { core::ptr::copy_nonoverlapping(k.buffer as *const u8, report.as_mut_ptr(), 8) };
        k.report(report);
        // Ask for the next report.
        k.ring.push(k.buffer, 8, TRB_NORMAL << 10 | 1 << 5 | 1 << 2);
        let (slot, dci) = (k.slot, k.dci);
        self.ring_doorbell(slot, dci as u32);
    }

    /// Wait for one particular event, dealing with any others as they come.
    /// For transfers, `slot` and `endpoint` say which one; a command
    /// completion matches regardless (only one command runs at a time).
    fn wait_event(&mut self, kind: u32, slot: u8, endpoint: u8, ms: u64) -> Option<Event> {
        let mut found = None;
        wait_until(ms, || {
            while let Some(event) = self.next_event() {
                let wanted = event.kind() == kind
                    && (kind == EVENT_COMMAND || (event.slot() == slot && event.endpoint() == endpoint));
                if wanted {
                    found = Some(event);
                    return true;
                }
                self.handle_unsolicited(event);
            }
            false
        });
        found
    }

    /// Run a controller command and return its completion event if it succeeded.
    fn command(&mut self, param: u64, control: u32) -> Option<Event> {
        self.commands.push(param, 0, control);
        self.ring_doorbell(0, 0);
        self.wait_event(EVENT_COMMAND, 0, 0, 1000).filter(|e| e.ok())
    }

    /// A control transfer on a device's default endpoint: the 8-byte setup
    /// packet, an optional data stage, then the status handshake.
    #[allow(clippy::too_many_arguments)]
    fn control(
        &mut self,
        slot: u8,
        ep0: &mut Ring,
        request_type: u8,
        request: u8,
        value: u16,
        index: u16,
        data: u64,
        length: u16,
    ) -> bool {
        let to_host = request_type & 0x80 != 0;
        let setup = request_type as u64
            | (request as u64) << 8
            | (value as u64) << 16
            | (index as u64) << 32
            | (length as u64) << 48;
        let transfer_type = if length == 0 { 0 } else if to_host { 3 } else { 2 };
        ep0.push(setup, 8, TRB_SETUP << 10 | 1 << 6 | transfer_type << 16);
        if length > 0 {
            ep0.push(data, length as u32, TRB_DATA << 10 | (to_host as u32) << 16);
        }
        // The status stage runs opposite to the data stage, and reports back.
        let status_to_host = length == 0 || !to_host;
        ep0.push(0, 0, TRB_STATUS << 10 | 1 << 5 | (status_to_host as u32) << 16);
        self.ring_doorbell(slot, 1);
        self.wait_event(EVENT_TRANSFER, slot, 1, 1000).is_some_and(|e| e.ok())
    }

    /// Something was plugged into `port`: bring it up, and if it is a
    /// keyboard start reading it. Returns a description for the log.
    fn enumerate(&mut self, port: usize) -> Result<&'static str, &'static str> {
        const OOM: &str = "out of suitable memory";
        let reg = self.port_register(port);
        unsafe {
            // USB 3 ports enable themselves; USB 2 ports need a reset first.
            if rd32(reg) & PORT_ENABLED == 0 {
                wr32(reg, PORT_POWER | PORT_RESET);
                if !wait_until(500, || rd32(reg) & PORT_RESET_CHANGE != 0) {
                    return Err("port reset timed out");
                }
            }
            wr32(reg, PORT_POWER | PORT_CHANGE_BITS);
            sched::sleep_ms(20); // let the device recover from the reset
            let status = rd32(reg);
            if status & PORT_CONNECTED == 0 || status & PORT_ENABLED == 0 {
                return Err("port did not enable");
            }
            // 1 = full speed (12 Mbit), 2 = low, 3 = high (480 Mbit), 4+ = SuperSpeed
            let speed = (status >> 10) & 0xF;

            let slot = self.command(0, TRB_ENABLE_SLOT << 10).ok_or("no free device slot")?.slot();
            self.port_slot[port] = slot;
            let device_context = self.page().ok_or(OOM)?;
            let input = self.page().ok_or(OOM)?;
            let mut ep0 = self.ring().ok_or(OOM)?;
            ((self.dcbaa + slot as u64 * 8) as *mut u64).write_volatile(device_context);

            // Describe the device to the controller: where it is plugged in
            // and how to talk to its default control endpoint.
            let mut max_packet: u32 = match speed {
                1 | 2 => 8,
                3 => 64,
                _ => 512,
            };
            let slot_words = (speed << 20, ((port + 1) as u32) << 16);
            self.context(input, 0).add(1).write_volatile(0b11); // adding slot + endpoint 0
            let slot_context = self.context(input, 1);
            slot_context.add(0).write_volatile(slot_words.0 | 1 << 27);
            slot_context.add(1).write_volatile(slot_words.1);
            let ep0_context = self.context(input, 2);
            ep0_context.add(1).write_volatile(3 << 1 | 4 << 3 | max_packet << 16); // control endpoint
            ep0_context.add(2).write_volatile(ep0.base as u32 | 1);
            ep0_context.add(3).write_volatile((ep0.base >> 32) as u32);
            ep0_context.add(4).write_volatile(8);
            self.command(input, TRB_ADDRESS_DEVICE << 10 | (slot as u32) << 24).ok_or("device did not accept an address")?;
            sched::sleep_ms(10);

            // Device descriptor, first 8 bytes: enough to learn the real
            // packet size of endpoint 0 on a full-speed device.
            let buf = self.scratch;
            let bytes = |i: u64| *((buf + i) as *const u8);
            if !self.control(slot, &mut ep0, 0x80, 6, 0x0100, 0, buf, 8) {
                return Err("could not read the device descriptor");
            }
            let reported = bytes(7) as u32;
            if speed == 1 && reported != max_packet && [16, 32, 64].contains(&reported) {
                max_packet = reported;
                self.context(input, 0).add(1).write_volatile(0b10);
                ep0_context.add(1).write_volatile(3 << 1 | 4 << 3 | max_packet << 16);
                self.command(input, TRB_EVALUATE_CONTEXT << 10 | (slot as u32) << 24)
                    .ok_or("could not update the control endpoint")?;
            }

            // The whole device descriptor, for the vendor and product IDs.
            if !self.control(slot, &mut ep0, 0x80, 6, 0x0100, 0, buf, 18) {
                return Err("could not read the device descriptor");
            }
            let ids = (bytes(8) as u16 | (bytes(9) as u16) << 8, bytes(10) as u16 | (bytes(11) as u16) << 8);

            // Configuration descriptor: header first for its total length,
            // then the whole thing (interfaces and endpoints follow it).
            if !self.control(slot, &mut ep0, 0x80, 6, 0x0200, 0, buf, 9) {
                return Err("could not read the configuration descriptor");
            }
            let total = (bytes(2) as u16 | (bytes(3) as u16) << 8).min(1024);
            let configuration = bytes(5);
            if !self.control(slot, &mut ep0, 0x80, 6, 0x0200, 0, buf, total) {
                return Err("could not read the configuration descriptor");
            }

            // Walk the interfaces looking for something we can drive:
            //   * a HID boot keyboard (class 3, subclass 1, protocol 1) and
            //     its interrupt IN endpoint, or
            //   * a mass-storage device speaking SCSI over "bulk-only"
            //     transport (class 8, subclass 6, protocol 0x50) and its
            //     bulk IN and bulk OUT endpoints.
            let mut found = None;
            let (mut bulk_in, mut bulk_out) = (None, None);
            let (mut i, mut wanted, mut interface) = (0u64, 0u8, 0u8);
            while i + 2 <= total as u64 {
                let (len, kind) = (bytes(i) as u64, bytes(i + 1));
                if len < 2 {
                    break;
                }
                if kind == 4 && i + 9 <= total as u64 {
                    wanted = match (bytes(i + 5), bytes(i + 6), bytes(i + 7)) {
                        (3, 1, 1) => 1,
                        (8, 6, 0x50) => 2,
                        (3, 1, 2) => 3, // boot mouse
                        (3, 0, 0) if ids == QEMU_TABLET => 4,
                        _ => 0,
                    };
                    interface = bytes(i + 2);
                } else if kind == 5 && wanted != 0 && i + 7 <= total as u64 {
                    let address = bytes(i + 2);
                    let transfer = bytes(i + 3) & 3;
                    let size = (bytes(i + 4) as u32 | (bytes(i + 5) as u32) << 8) & 0x7FF;
                    if matches!(wanted, 1 | 3 | 4) && found.is_none() && address & 0x80 != 0 && transfer == 3 {
                        let kind = match wanted {
                            3 => HidKind::Mouse,
                            4 => HidKind::Tablet,
                            _ => HidKind::Keyboard,
                        };
                        found = Some((address, size, bytes(i + 6), interface, kind));
                    } else if wanted == 2 && transfer == 2 {
                        if address & 0x80 != 0 {
                            bulk_in.get_or_insert((address, size));
                        } else {
                            bulk_out.get_or_insert((address, size));
                        }
                    }
                }
                i += len;
            }
            if found.is_none() {
                if let (Some(bulk_in), Some(bulk_out)) = (bulk_in, bulk_out) {
                    if !self.control(slot, &mut ep0, 0x00, 9, configuration as u16, 0, 0, 0) {
                        return Err("could not select the configuration");
                    }
                    return self.attach_drive(slot, port, input, slot_words, bulk_in, bulk_out);
                }
            }
            let Some((address, packet_size, interval, interface, kind)) = found else {
                return Ok("not a keyboard, mouse or drive, ignored");
            };

            if !self.control(slot, &mut ep0, 0x00, 9, configuration as u16, 0, 0, 0) {
                return Err("could not select the configuration");
            }
            // HID class requests: boot protocol (fixed 8-byte reports), and
            // only report when something changes. Not every device answers.
            // (The tablet keeps its own report format: no boot protocol.)
            if kind != HidKind::Tablet {
                self.control(slot, &mut ep0, 0x21, 0x0B, 0, interface as u16, 0, 0);
            }
            self.control(slot, &mut ep0, 0x21, 0x0A, 0, interface as u16, 0, 0);

            // Tell the controller about the keyboard's interrupt endpoint.
            let dci = (address & 0x0F) * 2 + 1;
            // The controller wants the polling period as a power of two of
            // 125 microsecond units.
            let period = if speed >= 3 {
                (interval.clamp(1, 16) - 1) as u32
            } else {
                (3 + (interval.max(1) as u32).ilog2()).clamp(3, 10)
            };
            let ring = self.ring().ok_or(OOM)?;
            let buffer = self.page().ok_or(OOM)?;
            self.context(input, 0).add(1).write_volatile(1 | 1 << dci);
            slot_context.add(0).write_volatile(slot_words.0 | (dci as u32) << 27);
            slot_context.add(1).write_volatile(slot_words.1);
            let ep_context = self.context(input, dci as u64 + 1);
            ep_context.add(0).write_volatile(period << 16);
            ep_context.add(1).write_volatile(3 << 1 | 7 << 3 | packet_size << 16); // interrupt IN
            ep_context.add(2).write_volatile(ring.base as u32 | 1);
            ep_context.add(3).write_volatile((ring.base >> 32) as u32);
            ep_context.add(4).write_volatile(packet_size << 16 | 8);
            self.command(input, TRB_CONFIGURE_ENDPOINT << 10 | (slot as u32) << 24)
                .ok_or("could not set up the keyboard endpoint")?;

            let mut k = Keyboard {
                slot,
                port,
                dci,
                ring,
                buffer,
                last: [0; 8],
                kind,
                repeat_usage: 0,
                repeat_char: 0,
                repeat_at: 0,
                dead: false,
            };
            k.ring.push(k.buffer, 8, TRB_NORMAL << 10 | 1 << 5 | 1 << 2);
            self.keyboards.push(k);
            self.ring_doorbell(slot, dci as u32);
            Ok(match kind {
                HidKind::Keyboard => "keyboard ready",
                HidKind::Mouse => "mouse ready",
                HidKind::Tablet => "tablet (absolute pointer) ready",
            })
        }
    }

    /// Set up a mass-storage device's two bulk endpoints, find out how big
    /// it is, and mount it if it is an RDX disk.
    fn attach_drive(
        &mut self,
        slot: u8,
        port: usize,
        input: u64,
        slot_words: (u32, u32),
        bulk_in: (u8, u32),
        bulk_out: (u8, u32),
    ) -> Result<&'static str, &'static str> {
        const OOM: &str = "out of suitable memory";
        let dci_in = (bulk_in.0 & 0x0F) * 2 + 1;
        let dci_out = (bulk_out.0 & 0x0F) * 2;
        let ring_in = self.ring().ok_or(OOM)?;
        let ring_out = self.ring().ok_or(OOM)?;
        unsafe {
            self.context(input, 0).add(1).write_volatile(1 | 1 << dci_in | 1 << dci_out);
            let slot_context = self.context(input, 1);
            slot_context.add(0).write_volatile(slot_words.0 | (dci_in.max(dci_out) as u32) << 27);
            slot_context.add(1).write_volatile(slot_words.1);
            // Endpoint types: 2 = bulk OUT, 6 = bulk IN.
            for (dci, kind, size, ring) in [(dci_in, 6u32, bulk_in.1, &ring_in), (dci_out, 2, bulk_out.1, &ring_out)] {
                let ep = self.context(input, dci as u64 + 1);
                ep.add(0).write_volatile(0);
                ep.add(1).write_volatile(3 << 1 | kind << 3 | size << 16);
                ep.add(2).write_volatile(ring.base as u32 | 1);
                ep.add(3).write_volatile((ring.base >> 32) as u32);
                ep.add(4).write_volatile(1024);
            }
        }
        self.command(input, TRB_CONFIGURE_ENDPOINT << 10 | (slot as u32) << 24)
            .ok_or("could not set up the drive's endpoints")?;

        let control = self.page().ok_or(OOM)?;
        let data = self.page().ok_or(OOM)?;
        self.drives.push(Drive {
            slot,
            port,
            dci_in,
            dci_out,
            ring_in,
            ring_out,
            control,
            data,
            sectors: 0,
            tag: 1,
            name: String::from("USB drive"),
            volume: None,
        });
        let d = self.drives.len() - 1;

        // INQUIRY: who are you? (vendor at bytes 8..16, product at 16..32)
        if self.scsi(d, &[0x12, 0, 0, 0, 36, 0], 36, true) {
            let text = unsafe { core::slice::from_raw_parts((self.drives[d].data + 8) as *const u8, 24) };
            let name = String::from_utf8_lossy(text).split_whitespace().collect::<Vec<_>>().join(" ");
            if !name.is_empty() {
                self.drives[d].name = name;
            }
        }
        // TEST UNIT READY, until it says yes. A freshly plugged drive first
        // answers "not yet"; REQUEST SENSE acknowledges that so it moves on.
        let mut ready = false;
        for _ in 0..10 {
            if self.scsi(d, &[0, 0, 0, 0, 0, 0], 0, true) {
                ready = true;
                break;
            }
            self.scsi(d, &[0x03, 0, 0, 0, 18, 0], 18, true);
            sched::sleep_ms(100);
        }
        // READ CAPACITY(10): last block number and block size, big-endian.
        if !ready || !self.scsi(d, &[0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0], 8, true) {
            self.drives.pop();
            return Err("drive did not become ready");
        }
        let reply = unsafe { core::slice::from_raw_parts(self.drives[d].data as *const u8, 8) };
        let last_block = u32::from_be_bytes([reply[0], reply[1], reply[2], reply[3]]) as u64;
        let block_size = u32::from_be_bytes([reply[4], reply[5], reply[6], reply[7]]);
        if block_size != 512 {
            self.drives.pop();
            return Err("drive does not use 512-byte sectors, unsupported");
        }
        self.drives[d].sectors = last_block + 1;
        log(format!("drive \"{}\", {} MiB", self.drives[d].name, (last_block + 1) / 2048));

        // Use it for saving files if it carries the RDX signature and no
        // other disk is in use already.
        if disk::is_mounted() {
            return Ok("drive ready (not used: another RDX disk is already mounted)");
        }
        let mounted = Volume::mount(&mut DriveIo { controller: self, drive: d });
        match mounted {
            Some((volume, files)) => {
                let restored = fs::merge_saved(files);
                disk::set_info(Some(volume.info("USB", &self.drives[d].name, self.drives[d].sectors)));
                self.drives[d].volume = Some(volume);
                log(format!("RDX disk mounted, {} saved file(s) restored", restored));
                Ok("drive ready, in use as the RDX disk")
            }
            None => Ok("drive ready (no RDX signature, so it will not be written to)"),
        }
    }

    /// Run one SCSI command using the bulk-only transport: a 31-byte command
    /// wrapper out, then the data (if any) in or out, then a 13-byte status
    /// wrapper in. Data travels through the drive's `data` page.
    fn scsi(&mut self, d: usize, command: &[u8], length: u32, to_host: bool) -> bool {
        let (slot, dci_in, dci_out, control, data, tag) = {
            let drive = &mut self.drives[d];
            drive.tag = drive.tag.wrapping_add(1);
            (drive.slot, drive.dci_in, drive.dci_out, drive.control, drive.data, drive.tag)
        };
        unsafe {
            let cbw = control as *mut u8;
            core::ptr::write_bytes(cbw, 0, 31);
            (cbw as *mut u32).write_unaligned(0x4342_5355); // "USBC"
            (cbw.add(4) as *mut u32).write_unaligned(tag);
            (cbw.add(8) as *mut u32).write_unaligned(length);
            cbw.add(12).write(if to_host { 0x80 } else { 0 });
            cbw.add(14).write(command.len() as u8);
            core::ptr::copy_nonoverlapping(command.as_ptr(), cbw.add(15), command.len());
        }
        const FLAGS: u32 = TRB_NORMAL << 10 | 1 << 5 | 1 << 2; // report completion, short packets too

        self.drives[d].ring_out.push(control, 31, FLAGS);
        self.ring_doorbell(slot, dci_out as u32);
        if !self.wait_event(EVENT_TRANSFER, slot, dci_out, 5000).is_some_and(|e| e.ok()) {
            return false;
        }
        if length > 0 {
            let dci = if to_host { dci_in } else { dci_out };
            let ring = if to_host { &mut self.drives[d].ring_in } else { &mut self.drives[d].ring_out };
            ring.push(data, length, FLAGS);
            self.ring_doorbell(slot, dci as u32);
            if !self.wait_event(EVENT_TRANSFER, slot, dci, 5000).is_some_and(|e| e.ok()) {
                return false;
            }
        }
        let csw = control + 64;
        self.drives[d].ring_in.push(csw, 13, FLAGS);
        self.ring_doorbell(slot, dci_in as u32);
        if !self.wait_event(EVENT_TRANSFER, slot, dci_in, 5000).is_some_and(|e| e.ok()) {
            return false;
        }
        unsafe {
            let csw = csw as *const u8;
            (csw as *const u32).read_unaligned() == 0x5342_5355 // "USBS"
                && (csw.add(4) as *const u32).read_unaligned() == tag
                && csw.add(12).read() == 0 // command passed
        }
    }

    /// READ(10) / WRITE(10), eight sectors (one page) per command.
    fn drive_transfer(&mut self, d: usize, lba: u64, buf: *mut u8, len: usize, write: bool) -> bool {
        if len % SECTOR != 0 || lba + (len / SECTOR) as u64 > self.drives[d].sectors || lba >> 32 != 0 {
            return false;
        }
        let page = self.drives[d].data as *mut u8;
        let mut done = 0;
        while done < len {
            let n = (len - done).min(8 * SECTOR);
            let block = (lba + (done / SECTOR) as u64) as u32;
            let count = (n / SECTOR) as u8;
            let b = block.to_be_bytes();
            let command = [if write { 0x2A } else { 0x28 }, 0, b[0], b[1], b[2], b[3], 0, 0, count, 0];
            unsafe {
                if write {
                    core::ptr::copy_nonoverlapping(buf.add(done), page, n);
                }
                if !self.scsi(d, &command, n as u32, !write) {
                    return false;
                }
                if !write {
                    core::ptr::copy_nonoverlapping(page, buf.add(done), n);
                }
            }
            done += n;
        }
        true
    }

    /// If files have changed, write them to the mounted USB disk.
    fn sync_drives(&mut self) {
        let Some(d) = self.drives.iter().position(|drive| drive.volume.is_some()) else { return };
        if !fs::take_dirty() {
            return;
        }
        let mut volume = self.drives[d].volume.take().unwrap();
        let result = volume.save(&mut DriveIo { controller: self, drive: d }, fs::snapshot());
        match result {
            Ok(()) => disk::set_info(Some(volume.info("USB", &self.drives[d].name, self.drives[d].sectors))),
            Err(e) => log(format!("could not save to the USB disk: {}", e)),
        }
        self.drives[d].volume = Some(volume);
    }

    /// One pass of housekeeping: key reports, auto-repeat, saving files,
    /// plug and unplug.
    fn service(&mut self) {
        while let Some(event) = self.next_event() {
            self.handle_unsolicited(event);
        }

        let now = arch::ticks();
        for k in self.keyboards.iter_mut() {
            if k.repeat_usage != 0 && now >= k.repeat_at {
                input::push(k.repeat_char);
                k.repeat_at = now + sched::ms_to_ticks(40);
            }
        }

        self.sync_drives();

        for port in 0..self.ports {
            let connected = unsafe { rd32(self.port_register(port)) } & PORT_CONNECTED != 0;
            if connected && self.port_state[port] == 0 {
                self.port_state[port] = 1;
                sched::sleep_ms(100); // let the plug settle
                let outcome = self.enumerate(port);
                let text = match outcome {
                    Ok(text) => text,
                    Err(text) => text,
                };
                log(format!("controller {} port {}: {}", self.name, port + 1, text));
            } else if !connected && self.port_state[port] != 0 {
                self.port_state[port] = 0;
                self.keyboards.retain(|k| k.port != port);
                if self.drives.iter().any(|drive| drive.port == port && drive.volume.is_some()) {
                    disk::set_info(None);
                    log(String::from("the RDX disk was unplugged; files are no longer being saved"));
                }
                self.drives.retain(|drive| drive.port != port);
                let slot = core::mem::take(&mut self.port_slot[port]);
                if slot != 0 {
                    self.command(0, TRB_DISABLE_SLOT << 10 | (slot as u32) << 24);
                }
                unsafe { wr32(self.port_register(port), PORT_POWER | PORT_CHANGE_BITS) };
                log(format!("controller {} port {}: unplugged", self.name, port + 1));
            }
        }
    }
}

fn task() {
    let mut controllers = Vec::new();
    for device in pci::find(0x0C_03_30) {
        match Controller::start(device) {
            Ok(c) => {
                log(format!("xHCI controller {} started, {} ports", c.name, c.ports));
                controllers.push(c);
            }
            Err(e) => log(format!(
                "xHCI controller {:02x}:{:02x}.{} unusable: {}",
                device.bus, device.dev, device.func, e
            )),
        }
    }
    if controllers.is_empty() {
        log(String::from("no usable xHCI controller (USB keyboards will not work)"));
        return;
    }
    loop {
        for c in controllers.iter_mut() {
            c.service();
        }
        sched::sleep_ms(10);
    }
}

/// Start the USB driver thread.
pub fn start() {
    sched::spawn("usb", task);
}
