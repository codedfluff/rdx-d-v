//! PS/2 keyboard: scancode set 1 -> ASCII, US layout.

use core::sync::atomic::{AtomicBool, Ordering};

use crate::arch::inb;
use crate::input;

const LOWER: &[u8; 58] = b"\0\x1b1234567890-=\x08\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
const UPPER: &[u8; 58] = b"\0\x1b!@#$%^&*()_+\x08\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

pub const KEY_UP: u8 = 0x80;
pub const KEY_DOWN: u8 = 0x81;
pub const KEY_LEFT: u8 = 0x82;
pub const KEY_RIGHT: u8 = 0x83;
pub const KEY_HOME: u8 = 0x84;
pub const KEY_END: u8 = 0x85;
pub const KEY_PAGE_UP: u8 = 0x86;
pub const KEY_PAGE_DOWN: u8 = 0x87;
pub const KEY_DELETE: u8 = 0x88;

static SHIFT: AtomicBool = AtomicBool::new(false);
static CAPS: AtomicBool = AtomicBool::new(false);
static CTRL: AtomicBool = AtomicBool::new(false);
static EXTENDED: AtomicBool = AtomicBool::new(false);

static PRESENT: AtomicBool = AtomicBool::new(false);

/// Returns true if the machine has a PS/2 controller.
pub fn init() -> bool {
    // With no controller the status port floats and reads as all ones.
    if inb(0x64) == 0xFF {
        return false;
    }
    // Throw away anything the firmware left in the controller's buffer.
    for _ in 0..16 {
        if inb(0x64) & 1 == 0 {
            break;
        }
        inb(0x60);
    }
    PRESENT.store(true, Ordering::Relaxed);
    true
}

/// Take whatever the controller has for us. Called from the keyboard
/// interrupt and from every timer tick, so it works with or without IRQ 1.
pub fn poll() {
    if !PRESENT.load(Ordering::Relaxed) {
        return;
    }
    for _ in 0..8 {
        let status = inb(0x64);
        if status & 0x01 == 0 {
            break;
        }
        let data = inb(0x60);
        if status & 0x20 == 0 {
            handle_scancode(data);
        } else {
            crate::mouse::byte(data); // bit 5 set: the byte is from the mouse
        }
    }
}

/// Shared with the USB keyboard driver: is Caps Lock on?
pub fn caps_lock() -> &'static AtomicBool {
    &CAPS
}

fn handle_scancode(code: u8) {
    if code == 0xE0 {
        EXTENDED.store(true, Ordering::Relaxed);
        return;
    }
    if EXTENDED.swap(false, Ordering::Relaxed) {
        // Extended keys: right ctrl, and the navigation cluster, which is
        // delivered to programs as single bytes 0x80..0x88.
        if code & 0x7F == 0x1D {
            CTRL.store(code & 0x80 == 0, Ordering::Relaxed);
        } else if code & 0x80 == 0 {
            let key = match code {
                0x48 => KEY_UP,
                0x50 => KEY_DOWN,
                0x4B => KEY_LEFT,
                0x4D => KEY_RIGHT,
                0x47 => KEY_HOME,
                0x4F => KEY_END,
                0x49 => KEY_PAGE_UP,
                0x51 => KEY_PAGE_DOWN,
                0x53 => KEY_DELETE,
                _ => 0,
            };
            if key != 0 {
                input::push(key);
            }
        }
        return;
    }

    let released = code & 0x80 != 0;
    let key = code & 0x7F;
    match key {
        0x2A | 0x36 => SHIFT.store(!released, Ordering::Relaxed),
        0x1D => CTRL.store(!released, Ordering::Relaxed),
        0x3A if !released => {
            CAPS.fetch_xor(true, Ordering::Relaxed);
        }
        _ if released => {}
        _ if (key as usize) < LOWER.len() => {
            let shift = SHIFT.load(Ordering::Relaxed);
            let mut ch = if shift { UPPER[key as usize] } else { LOWER[key as usize] };
            if CAPS.load(Ordering::Relaxed) && ch.is_ascii_alphabetic() {
                ch ^= 0x20;
            }
            if CTRL.load(Ordering::Relaxed) && ch.is_ascii_alphabetic() {
                ch &= 0x1F; // Ctrl+C -> 0x03, etc.
            }
            if ch != 0 {
                input::push(ch);
            }
        }
        _ => {}
    }
}
