//! Interrupt-safe spinlock.
//!
//! Taking the lock disables interrupts on this CPU, so the holder is never
//! preempted and an interrupt handler on the same CPU can never find the
//! lock already held by the code it interrupted. Other CPUs that want the
//! lock spin until it is free. Finding it held by *this* CPU means a genuine
//! bug (re-entrancy), and we panic rather than hang.

use core::arch::asm;
use core::cell::UnsafeCell;
use core::ops::{Deref, DerefMut};
use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

pub fn interrupts_enabled() -> bool {
    let flags: u64;
    unsafe { asm!("pushfq", "pop {}", out(reg) flags, options(nomem, preserves_flags)) };
    flags & 0x200 != 0
}

pub fn cli() {
    unsafe { asm!("cli", options(nomem, nostack)) };
}

pub fn sti() {
    unsafe { asm!("sti", options(nomem, nostack)) };
}

pub struct IrqLock<T> {
    locked: AtomicBool,
    /// CPU number + 1 of the holder, or 0.
    owner: AtomicUsize,
    data: UnsafeCell<T>,
}

unsafe impl<T: Send> Sync for IrqLock<T> {}

impl<T> IrqLock<T> {
    pub const fn new(value: T) -> Self {
        Self { locked: AtomicBool::new(false), owner: AtomicUsize::new(0), data: UnsafeCell::new(value) }
    }

    pub fn lock(&self) -> IrqGuard<'_, T> {
        let was_enabled = interrupts_enabled();
        cli();
        let me = crate::arch::cpu_id() + 1;
        if self.owner.load(Ordering::Relaxed) == me {
            panic!("IrqLock re-entered on CPU {} (kernel deadlock)", me - 1);
        }
        while self.locked.swap(true, Ordering::Acquire) {
            // Another CPU has it. A panicking system never unlocks, so stop
            // quietly instead of spinning forever at full power.
            if crate::arch::panicking() {
                crate::arch::halt_forever();
            }
            core::hint::spin_loop();
        }
        self.owner.store(me, Ordering::Relaxed);
        IrqGuard { lock: self, was_enabled }
    }

    /// Release a lock whose guard was deliberately forgotten. The scheduler
    /// uses this to keep its lock held across a context switch and let go
    /// only once the CPU is safely on the next task's stack.
    pub unsafe fn force_unlock(&self) {
        self.owner.store(0, Ordering::Relaxed);
        self.locked.store(false, Ordering::Release);
    }

    /// Bypass the lock entirely. Only for the panic path, where nothing else
    /// will ever run again.
    #[allow(clippy::mut_from_ref)]
    pub unsafe fn force(&self) -> &mut T {
        &mut *self.data.get()
    }
}

pub struct IrqGuard<'a, T> {
    lock: &'a IrqLock<T>,
    was_enabled: bool,
}

impl<T> Deref for IrqGuard<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        unsafe { &*self.lock.data.get() }
    }
}

impl<T> DerefMut for IrqGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        unsafe { &mut *self.lock.data.get() }
    }
}

impl<T> Drop for IrqGuard<'_, T> {
    fn drop(&mut self) {
        self.lock.owner.store(0, Ordering::Relaxed);
        self.lock.locked.store(false, Ordering::Release);
        if self.was_enabled {
            sti();
        }
    }
}
