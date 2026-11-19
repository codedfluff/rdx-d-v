//! COM1 (16550 UART) — debug log output and an alternate input source.

use crate::arch::{inb, outb};

const COM1: u16 = 0x3F8;

pub fn init() {
    outb(COM1 + 1, 0x00); // no interrupts, we poll
    outb(COM1 + 3, 0x80); // DLAB on
    outb(COM1, 0x01); // divisor 1 = 115200 baud
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); // 8N1
    outb(COM1 + 2, 0xC7); // FIFO on, cleared
    outb(COM1 + 4, 0x03); // DTR + RTS
}

pub fn write_byte(b: u8) {
    // Bounded wait so a missing UART on real hardware can't hang the kernel.
    for _ in 0..100_000 {
        if inb(COM1 + 5) & 0x20 != 0 {
            break;
        }
    }
    outb(COM1, b);
}

pub fn write_str(s: &str) {
    for b in s.bytes() {
        if b == b'\n' {
            write_byte(b'\r');
        }
        write_byte(b);
    }
}

pub fn try_read() -> Option<u8> {
    let status = inb(COM1 + 5);
    // 0xFF = floating bus, i.e. no UART present.
    if status != 0xFF && status & 0x01 != 0 {
        Some(inb(COM1))
    } else {
        None
    }
}
