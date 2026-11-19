//! Starting the other CPU cores.
//!
//! The firmware hands us one running CPU. The rest sit halted until they are
//! sent an INIT signal followed by a "start-up IPI" naming a page of low
//! memory to begin executing — in 16-bit real mode, exactly as a PC powers
//! on. So we copy a small trampoline there that walks the core up through
//! 32-bit protected mode into 64-bit long mode, using the same page tables
//! and control-register settings as CPU 0, and jumps into the kernel.
//!
//! Cores are started one at a time. Each ends up in `secondary_main`, where
//! it loads its own TSS, starts its own timer and becomes an idle task the
//! scheduler can hand work to.

use alloc::vec;
use alloc::vec::Vec;
use core::arch::asm;
use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

use crate::arch::{self, MAX_CPUS};
use crate::{acpi, memory, sched, sync, timer};

// The trampoline, hand-assembled. Fields marked ____ are patched in below.
// It is loaded at a page-aligned address B below 1 MiB and entered with
// CS:IP = (B >> 4):0.
#[rustfmt::skip]
const TRAMPOLINE: [u8; 0xA8] = [
    // ---- 16-bit real mode ----
    0xFA,                               // 00 cli
    0xFC,                               // 01 cld
    0x8C, 0xC8,                         // 02 mov ax, cs
    0x8E, 0xD8,                         // 04 mov ds, ax             (data segment = this page)
    0x0F, 0x01, 0x16, 0x90, 0x00,       // 06 lgdt [0x90]            (the pointer at offset 90)
    0x0F, 0x20, 0xC0,                   // 0B mov eax, cr0
    0x0C, 0x01,                         // 0E or al, 1               (protected mode on)
    0x0F, 0x22, 0xC0,                   // 10 mov cr0, eax
    0x66, 0xEA, 0, 0, 0, 0, 0x08, 0x00, // 13 jmp 0x08:____          (B + 0x1B)
    // ---- 32-bit protected mode ----
    0x66, 0xB8, 0x10, 0x00,             // 1B mov ax, 0x10
    0x8E, 0xD8,                         // 1F mov ds, ax
    0x8E, 0xC0,                         // 21 mov es, ax
    0x8E, 0xD0,                         // 23 mov ss, ax
    0xB8, 0, 0, 0, 0,                   // 25 mov eax, ____          (CR4)
    0x0F, 0x22, 0xE0,                   // 2A mov cr4, eax
    0xB8, 0, 0, 0, 0,                   // 2D mov eax, ____          (CR3)
    0x0F, 0x22, 0xD8,                   // 32 mov cr3, eax
    0xB9, 0x80, 0x00, 0x00, 0xC0,       // 35 mov ecx, 0xC0000080    (EFER)
    0xB8, 0, 0, 0, 0,                   // 3A mov eax, ____          (EFER value)
    0x31, 0xD2,                         // 3F xor edx, edx
    0x0F, 0x30,                         // 41 wrmsr
    0xB8, 0, 0, 0, 0,                   // 43 mov eax, ____          (CR0: paging on -> long mode)
    0x0F, 0x22, 0xC0,                   // 48 mov cr0, eax
    0xEA, 0, 0, 0, 0, 0x18, 0x00,       // 4B jmp 0x18:____          (B + 0x52)
    // ---- 64-bit long mode ----
    0x48, 0x8B, 0x24, 0x25, 0, 0, 0, 0, // 52 mov rsp, [____]        (B + 0x98)
    0x48, 0x8B, 0x04, 0x25, 0, 0, 0, 0, // 5A mov rax, [____]        (B + 0xA0)
    0xFF, 0xD0,                         // 62 call rax
    0xF4,                               // 64 hlt
    0xEB, 0xFD,                         // 65 jmp to the hlt
    0, 0, 0, 0, 0, 0, 0, 0, 0,          // 67 padding to 0x70
    // 70: a tiny GDT: null, 32-bit code, 32-bit data, 64-bit code
    0, 0, 0, 0, 0, 0, 0, 0,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x9A, 0xCF, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x92, 0xCF, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0x9A, 0xAF, 0x00,
    // 90: GDT pointer: limit, then base ____ (B + 0x70)
    0x1F, 0x00, 0, 0, 0, 0, 0, 0,
    // 98: stack pointer for this core ____
    0, 0, 0, 0, 0, 0, 0, 0,
    // A0: address of secondary_entry ____
    0, 0, 0, 0, 0, 0, 0, 0,
];

/// Number of the core being started, and whether it has checked in.
static STARTING: AtomicUsize = AtomicUsize::new(0);
static CHECKED_IN: AtomicBool = AtomicBool::new(false);
/// Set once low memory is unmapped again and the new cores may proceed.
static RELEASED: AtomicBool = AtomicBool::new(false);
static ONLINE: AtomicUsize = AtomicUsize::new(1);

/// How many CPU cores are running kernel code.
pub fn online() -> usize {
    ONLINE.load(Ordering::Relaxed)
}

/// APIC IDs of every enabled processor the firmware lists (ACPI MADT).
fn processors() -> Vec<u32> {
    let mut ids = Vec::new();
    let Some(madt) = acpi::find_table(b"APIC") else { return ids };
    unsafe {
        let length = ((madt + 4) as *const u32).read_unaligned() as u64;
        let mut entry = madt + 44;
        while entry + 2 <= madt + length {
            let kind = *(entry as *const u8);
            let size = *((entry + 1) as *const u8) as u64;
            if size < 2 {
                break;
            }
            let u32_at = |offset: u64| ((entry + offset) as *const u32).read_unaligned();
            match kind {
                // Local APIC: 8-bit ID, flags bit 0 = enabled
                0 if size >= 8 && u32_at(4) & 1 != 0 => ids.push(*((entry + 3) as *const u8) as u32),
                // Local x2APIC: 32-bit ID
                9 if size >= 16 && u32_at(8) & 1 != 0 => ids.push(u32_at(4)),
                _ => {}
            }
            entry += size;
        }
    }
    ids
}

fn read_cr(number: u8) -> u64 {
    let value: u64;
    unsafe {
        match number {
            0 => asm!("mov {}, cr0", out(reg) value, options(nomem, nostack, preserves_flags)),
            3 => asm!("mov {}, cr3", out(reg) value, options(nomem, nostack, preserves_flags)),
            _ => asm!("mov {}, cr4", out(reg) value, options(nomem, nostack, preserves_flags)),
        }
    }
    value
}

/// Start every other core. Returns the total number now running.
pub fn start_secondaries() -> usize {
    let me = timer::local_id();
    let others: Vec<u32> = processors().into_iter().filter(|&id| id != me).collect();
    let Some(base) = memory::low_page() else { return 1 };
    let cr3 = read_cr(3);
    if others.is_empty() || cr3 >= 1 << 32 {
        return 1; // nothing to start, or page tables a 32-bit core can't load
    }

    let efer: u64;
    unsafe {
        let (low, high): (u32, u32);
        asm!("rdmsr", in("ecx") 0xC000_0080u32, out("eax") low, out("edx") high, options(nomem, nostack));
        efer = (high as u64) << 32 | low as u64;
    }

    memory::map_low_memory(true);
    let page = base as *mut u8;
    let patch32 = |offset: usize, value: u32| unsafe { (page.add(offset) as *mut u32).write_unaligned(value) };
    let patch64 = |offset: usize, value: u64| unsafe { (page.add(offset) as *mut u64).write_unaligned(value) };
    unsafe { core::ptr::copy_nonoverlapping(TRAMPOLINE.as_ptr(), page, TRAMPOLINE.len()) };
    patch32(0x15, base as u32 + 0x1B);
    patch32(0x26, read_cr(4) as u32 & !(1 << 17)); // PCID can't be on before long mode
    patch32(0x2E, cr3 as u32);
    patch32(0x3B, (efer as u32 & !(1 << 10)) | 1 << 8); // long mode enable
    patch32(0x44, read_cr(0) as u32);
    patch32(0x4C, base as u32 + 0x52);
    patch32(0x56, base as u32 + 0x98);
    patch32(0x5E, base as u32 + 0xA0);
    patch32(0x92, base as u32 + 0x70);
    patch64(0xA0, secondary_entry as *const () as u64);

    let mut started = 1;
    for &apic_id in others.iter().take(MAX_CPUS - 1) {
        // Each core needs a boot stack (which becomes its idle task's stack)
        // and a stack for double faults. Both live for ever.
        let stack = vec![0u8; 64 * 1024].leak();
        let fault_stack = vec![0u8; 16 * 1024].leak();
        arch::set_fault_stack(started, (fault_stack.as_ptr() as u64 + fault_stack.len() as u64) & !0xF);
        patch64(0x98, (stack.as_ptr() as u64 + stack.len() as u64) & !0xF);
        STARTING.store(started, Ordering::SeqCst);
        CHECKED_IN.store(false, Ordering::SeqCst);

        timer::send_ipi(apic_id, 0x4500); // INIT
        timer::spin_ms(10);
        let vector = (base >> 12) as u32;
        for _ in 0..2 {
            timer::send_ipi(apic_id, 0x4600 | vector); // start-up, twice as the spec asks
            timer::spin_ms(1);
            if CHECKED_IN.load(Ordering::SeqCst) {
                break;
            }
        }
        for _ in 0..200 {
            if CHECKED_IN.load(Ordering::SeqCst) {
                break;
            }
            timer::spin_ms(1);
        }
        if CHECKED_IN.load(Ordering::SeqCst) {
            started += 1;
        }
    }

    memory::map_low_memory(false);
    ONLINE.store(started, Ordering::SeqCst);
    RELEASED.store(true, Ordering::SeqCst);
    started
}

/// Where the trampoline lands: 64-bit mode, interrupts off, on the stack
/// allocated above, still on the trampoline's temporary GDT.
extern "sysv64" fn secondary_entry() -> ! {
    let cpu = STARTING.load(Ordering::SeqCst);
    arch::init_secondary(cpu); // no locks may be taken before this
    CHECKED_IN.store(true, Ordering::SeqCst);
    while !RELEASED.load(Ordering::SeqCst) {
        core::hint::spin_loop();
    }
    memory::flush_tlb(); // forget the low-memory mapping
    timer::init_secondary();
    sched::adopt_idle();
    sync::sti();
    loop {
        sched::reap();
        arch::hlt();
    }
}
