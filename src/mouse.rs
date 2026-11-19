//! PS/2 mouse, on the second ("auxiliary") port of the same controller the
//! PS/2 keyboard uses.
//!
//! Once enabled, the mouse sends a 3-byte packet for every movement or
//! button change: flags (buttons, signs, overflow), then x and y movement.
//! Its bytes arrive through port 0x60 like the keyboard's, flagged as the
//! mouse's by bit 5 of the status register; keyboard::poll sorts them out
//! and hands ours to `byte`. (USB mice are handled by xhci.rs.)

use core::sync::atomic::{AtomicBool, Ordering};

use crate::arch::{inb, outb};
use crate::display;
use crate::sync::IrqLock;

static PRESENT: AtomicBool = AtomicBool::new(false);
static PACKET: IrqLock<([u8; 3], usize)> = IrqLock::new(([0; 3], 0));

fn wait_writable() -> bool {
    for _ in 0..200_000 {
        if inb(0x64) & 0x02 == 0 {
            return true;
        }
        core::hint::spin_loop();
    }
    false
}

fn wait_readable() -> bool {
    for _ in 0..200_000 {
        if inb(0x64) & 0x01 != 0 {
            return true;
        }
        core::hint::spin_loop();
    }
    false
}

fn controller(command: u8) {
    wait_writable();
    outb(0x64, command);
}

/// Send a byte to the mouse and wait for its acknowledgement (0xFA).
fn send(byte: u8) -> bool {
    controller(0xD4); // the next data byte is for the second port
    if !wait_writable() {
        return false;
    }
    outb(0x60, byte);
    for _ in 0..4 {
        if !wait_readable() {
            return false;
        }
        if inb(0x60) == 0xFA {
            return true;
        }
    }
    false
}

/// Switch the mouse on. Call with interrupts off, before the keyboard is
/// being polled (we read the controller's answers ourselves). Returns true
/// if a mouse answered.
pub fn init() -> bool {
    controller(0xA8); // enable the second port
    controller(0x20); // read the configuration byte
    if !wait_readable() {
        return false;
    }
    let config = inb(0x60);
    // Interrupts from the second port on (bit 1), its clock on (bit 5 off).
    controller(0x60);
    if !wait_writable() {
        return false;
    }
    outb(0x60, (config | 0x02) & !0x20);
    // Default settings, then start reporting.
    if !send(0xF6) || !send(0xF4) {
        return false;
    }
    PRESENT.store(true, Ordering::Relaxed);
    true
}

pub fn present() -> bool {
    PRESENT.load(Ordering::Relaxed)
}

/// One byte from the mouse.
pub fn byte(b: u8) {
    let packet = {
        let mut p = PACKET.lock();
        let (bytes, count) = &mut *p;
        // The first byte of every packet has bit 3 set; if this one does
        // not, we are out of step: drop bytes until we are back in it.
        if *count == 0 && b & 0x08 == 0 {
            return;
        }
        bytes[*count] = b;
        *count += 1;
        if *count < 3 {
            return;
        }
        *count = 0;
        *bytes
    };
    let flags = packet[0];
    if flags & 0xC0 != 0 {
        return; // the movement overflowed: the numbers are meaningless
    }
    let dx = packet[1] as i32 - if flags & 0x10 != 0 { 256 } else { 0 };
    let dy = packet[2] as i32 - if flags & 0x20 != 0 { 256 } else { 0 };
    // The mouse counts y upwards; the screen counts it downwards.
    display::mouse(dx, -dy, (flags & 0x07) as u32);
}
