//! Shared input queue, fed by the PS/2 keyboard IRQ and by the serial port.

use crate::arch;
use crate::sched::{self, Wait};
use crate::sync::IrqLock;

const SIZE: usize = 256;

struct Ring {
    buf: [u8; SIZE],
    head: usize,
    tail: usize,
}

static QUEUE: IrqLock<Ring> = IrqLock::new(Ring { buf: [0; SIZE], head: 0, tail: 0 });

pub fn push(b: u8) {
    // A graphical program that has taken over the display gets every key.
    if crate::display::key(b) {
        return;
    }
    // Ctrl+C goes to the kernel, not the queue, when a program has the
    // keyboard: it interrupts that program.
    if b == 0x03 && sched::interrupt_foreground() {
        return;
    }
    {
        let mut q = QUEUE.lock();
        let next = (q.head + 1) % SIZE;
        if next != q.tail {
            let head = q.head;
            q.buf[head] = b;
            q.head = next;
        }
    }
    sched::wake(Wait::Input);
}

/// Is there a key waiting?
pub fn available() -> bool {
    let q = QUEUE.lock();
    q.head != q.tail
}

pub fn pop() -> Option<u8> {
    let mut q = QUEUE.lock();
    if q.head == q.tail {
        return None;
    }
    let b = q.buf[q.tail];
    q.tail = (q.tail + 1) % SIZE;
    Some(b)
}

/// Put a byte back at the front of the queue, to be read again next.
pub fn unpop(b: u8) {
    let mut q = QUEUE.lock();
    let tail = (q.tail + SIZE - 1) % SIZE;
    if tail != q.head {
        q.buf[tail] = b;
        q.tail = tail;
    }
}

/// Block until a byte of input arrives. A task is taken off the CPU until
/// a key is pushed; outside the scheduler we just halt until the next
/// interrupt.
pub fn read() -> u8 {
    loop {
        let seen = sched::events();
        if let Some(b) = pop() {
            return b;
        }
        if sched::current_is_idle() {
            arch::hlt();
        } else {
            sched::wait_for(Wait::Input, seen);
        }
    }
}
