//! The system tick, driven by the CPU's local APIC timer.
//!
//! Every x86-64 processor has this timer built in, so the kernel does not
//! depend on the legacy PIT/8259 chips being present or wired up. What the
//! APIC timer lacks is a known frequency, so at boot we measure it against
//! whichever reference clock the machine really has:
//!
//!   1. the HPET (found through ACPI), else
//!   2. the ACPI power-management timer, else
//!   3. channel 2 of the legacy PIT, polled (no interrupt needed), else
//!   4. the frequency the CPU reports through CPUID, else
//!   5. one full second of the battery-backed real-time clock, else
//!   6. a guess (timing will be off, but the system still runs).

use core::arch::asm;
use core::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

use crate::arch::{inb, inl, outb, TICK_HZ};
use crate::{acpi, memory};

pub const VEC_TIMER: u64 = 48;
pub const VEC_SPURIOUS: u64 = 255;
/// Sent from one CPU to another to make it reschedule straight away.
pub const VEC_RESCHEDULE: u64 = 49;

const MSR_APIC_BASE: u32 = 0x1B;
const REG_TPR: u32 = 0x80;
const REG_EOI: u32 = 0xB0;
const REG_SPURIOUS: u32 = 0xF0;
const REG_LVT_TIMER: u32 = 0x320;
const REG_INITIAL_COUNT: u32 = 0x380;
const REG_CURRENT_COUNT: u32 = 0x390;
const REG_DIVIDE: u32 = 0x3E0;

static X2APIC: AtomicBool = AtomicBool::new(false);
static BASE: AtomicU64 = AtomicU64::new(0xFEE0_0000);

fn rdmsr(msr: u32) -> u64 {
    let (low, high): (u32, u32);
    unsafe { asm!("rdmsr", in("ecx") msr, out("eax") low, out("edx") high, options(nomem, nostack, preserves_flags)) };
    (high as u64) << 32 | low as u64
}

fn wrmsr(msr: u32, value: u64) {
    unsafe {
        asm!("wrmsr", in("ecx") msr, in("eax") value as u32, in("edx") (value >> 32) as u32,
             options(nomem, nostack, preserves_flags))
    };
}

// In x2APIC mode the registers are MSRs; otherwise they are memory mapped.
fn read(reg: u32) -> u32 {
    if X2APIC.load(Ordering::Relaxed) {
        rdmsr(0x800 + (reg >> 4)) as u32
    } else {
        unsafe { ((BASE.load(Ordering::Relaxed) + reg as u64) as *const u32).read_volatile() }
    }
}

fn write(reg: u32, value: u32) {
    if X2APIC.load(Ordering::Relaxed) {
        wrmsr(0x800 + (reg >> 4), value as u64);
    } else {
        unsafe { ((BASE.load(Ordering::Relaxed) + reg as u64) as *mut u32).write_volatile(value) }
    }
}

/// Acknowledge the timer interrupt.
pub fn eoi() {
    write(REG_EOI, 0);
}

// ---------------------------------------------------------------- reference clocks

/// Wait 10 ms by the HPET. False if there is no usable HPET.
fn hpet_wait() -> bool {
    let Some(table) = acpi::find_table(b"HPET") else { return false };
    let physical = unsafe { ((table + 44) as *const u64).read_unaligned() };
    if physical == 0 {
        return false;
    }
    let Some(base) = memory::map_mmio(physical, 0x400) else { return false };
    let reg = |offset: u64| (base + offset) as *mut u64;
    unsafe {
        let capabilities = reg(0x00).read_volatile();
        let period_fs = capabilities >> 32; // femtoseconds per count
        if period_fs == 0 || period_fs > 100_000_000 {
            return false;
        }
        reg(0x10).write_volatile(reg(0x10).read_volatile() | 1); // enable the counter
        let mask = if capabilities & (1 << 13) != 0 { u64::MAX } else { 0xFFFF_FFFF };
        let target = 10_000_000_000_000 / period_fs;
        let start = reg(0xF0).read_volatile();
        for i in 0..50_000_000u32 {
            let elapsed = reg(0xF0).read_volatile().wrapping_sub(start) & mask;
            if elapsed >= target {
                return true;
            }
            if i == 1_000_000 && elapsed == 0 {
                return false; // the counter is not running
            }
        }
    }
    false
}

/// Wait 10 ms using PIT channel 2 (the old speaker timer), which can be
/// read back through port 0x61 without any interrupt.
fn pit_wait() -> bool {
    let count = (1_193_182u32 / 100) as u16;
    outb(0x61, (inb(0x61) & !0x02) | 0x01); // gate on, speaker off
    outb(0x43, 0xB0); // channel 2, one-shot
    outb(0x42, count as u8);
    outb(0x42, (count >> 8) as u8);
    if inb(0x61) & 0x20 != 0 {
        return false; // output already high: nothing is counting
    }
    for _ in 0..3_000_000 {
        if inb(0x61) & 0x20 != 0 {
            return true;
        }
    }
    false
}

/// Wait 10 ms by the ACPI power-management timer, a 3.579545 MHz counter
/// read through an I/O port named in the FADT.
fn pm_timer_wait() -> bool {
    let Some(fadt) = acpi::find_table(b"FACP") else { return false };
    let port = unsafe { ((fadt + 76) as *const u32).read_unaligned() };
    if port == 0 || port > 0xFFFF {
        return false;
    }
    // The counter is at least 24 bits wide; only use those.
    let now = || inl(port as u16) & 0x00FF_FFFF;
    let start = now();
    for i in 0..3_000_000u32 {
        let elapsed = now().wrapping_sub(start) & 0x00FF_FFFF;
        if elapsed >= 35_795 {
            return true;
        }
        if i == 100_000 && elapsed == 0 {
            return false; // the counter is not running
        }
    }
    false
}

/// Seconds field of the battery-backed clock, read between updates.
fn rtc_seconds() -> u8 {
    for _ in 0..100_000 {
        outb(0x70, 0x0A);
        if inb(0x71) & 0x80 == 0 {
            break;
        }
    }
    outb(0x70, 0x00);
    inb(0x71)
}

/// Last resort with a real clock behind it: time one whole second of the
/// real-time clock. Slow (up to two seconds of boot) but nearly universal.
fn rtc_measure() -> Option<u32> {
    let wait_for_change = || {
        let before = rtc_seconds();
        for _ in 0..4_000_000u32 {
            if rtc_seconds() != before {
                return true;
            }
        }
        false
    };
    if !wait_for_change() {
        return None;
    }
    write(REG_INITIAL_COUNT, u32::MAX);
    let ok = wait_for_change();
    let current = read(REG_CURRENT_COUNT);
    write(REG_INITIAL_COUNT, 0);
    let per_second = u32::MAX - current;
    (ok && current != 0 && per_second > 500_000).then_some(per_second / 100)
}

/// Count how far the APIC timer gets while `wait` blocks for 10 ms.
fn measure(wait: fn() -> bool) -> Option<u32> {
    write(REG_INITIAL_COUNT, u32::MAX);
    let ok = wait();
    let current = read(REG_CURRENT_COUNT);
    write(REG_INITIAL_COUNT, 0);
    let elapsed = u32::MAX - current;
    // Reject results no real machine could produce.
    (ok && current != 0 && elapsed > 5_000).then_some(elapsed)
}

fn calibrate() -> (u32, &'static str) {
    // Test hook: building with the environment variable RDX_TEST_RTC_ONLY set
    // skips the better clocks, so the slow fallback can be exercised in QEMU.
    if option_env!("RDX_TEST_RTC_ONLY").is_some() {
        return match rtc_measure() {
            Some(ticks) => (ticks, "the real-time clock"),
            None => (62_500, "a guess (no reference clock found; timing will be off)"),
        };
    }
    if let Some(ticks) = measure(hpet_wait) {
        return (ticks, "the HPET");
    }
    if let Some(ticks) = measure(pm_timer_wait) {
        return (ticks, "the ACPI timer");
    }
    if let Some(ticks) = measure(pit_wait) {
        return (ticks, "the PIT");
    }
    #[allow(unused_unsafe)]
    let cpuid = |leaf: u32| unsafe { core::arch::x86_64::__cpuid(leaf) };
    if cpuid(0).eax >= 0x15 {
        let crystal_hz = cpuid(0x15).ecx;
        if crystal_hz != 0 {
            return (crystal_hz / 16 / 100, "the CPU's reported clock rate");
        }
    }
    if let Some(ticks) = rtc_measure() {
        return (ticks, "the real-time clock");
    }
    (62_500, "a guess (no reference clock found; timing will be off)") // assumes a 100 MHz bus
}

/// Start the periodic tick. Returns the name of the clock used to calibrate.
pub fn init() -> &'static str {
    let mut base = rdmsr(MSR_APIC_BASE);
    if base & (1 << 11) == 0 {
        base |= 1 << 11; // APIC globally enabled
        wrmsr(MSR_APIC_BASE, base);
    }
    X2APIC.store(base & (1 << 10) != 0, Ordering::Relaxed);
    if !X2APIC.load(Ordering::Relaxed) {
        let registers = memory::map_mmio(base & 0x000F_FFFF_FFFF_F000, 0x1000);
        BASE.store(registers.expect("cannot map the local APIC"), Ordering::Relaxed);
    }

    write(REG_TPR, 0);
    write(REG_SPURIOUS, 0x100 | VEC_SPURIOUS as u32); // software-enable the APIC
    write(REG_DIVIDE, 0x3); // count at bus clock / 16
    write(REG_LVT_TIMER, 1 << 16); // masked while we calibrate

    let (ticks_per_10ms, source) = calibrate();
    let per_tick = (ticks_per_10ms as u64 * 100 / TICK_HZ).max(1) as u32;
    PER_TICK.store(per_tick, Ordering::Relaxed);
    write(REG_LVT_TIMER, VEC_TIMER as u32 | 1 << 17); // periodic
    write(REG_INITIAL_COUNT, per_tick);
    source
}

/// Timer counts per system tick, as measured on CPU 0. Every core's APIC
/// timer runs from the same clock, so the others reuse it.
static PER_TICK: AtomicU32 = AtomicU32::new(0);

/// Start the tick on a secondary CPU.
pub fn init_secondary() {
    let mut base = rdmsr(MSR_APIC_BASE) | 1 << 11;
    if X2APIC.load(Ordering::Relaxed) {
        base |= 1 << 10;
    }
    wrmsr(MSR_APIC_BASE, base);
    write(REG_TPR, 0);
    write(REG_SPURIOUS, 0x100 | VEC_SPURIOUS as u32);
    write(REG_DIVIDE, 0x3);
    write(REG_LVT_TIMER, VEC_TIMER as u32 | 1 << 17);
    write(REG_INITIAL_COUNT, PER_TICK.load(Ordering::Relaxed));
}

/// This CPU's APIC ID: the address other CPUs use to signal it.
pub fn local_id() -> u32 {
    let id = read(0x20);
    if X2APIC.load(Ordering::Relaxed) { id } else { id >> 24 }
}

/// Send an inter-processor interrupt. `command` is the low half of the
/// interrupt command register (delivery mode, vector, ...).
pub fn send_ipi(destination: u32, command: u32) {
    if X2APIC.load(Ordering::Relaxed) {
        wrmsr(0x830, (destination as u64) << 32 | command as u64);
    } else {
        write(0x310, destination << 24);
        write(0x300, command);
        for _ in 0..1_000_000 {
            if read(0x300) & (1 << 12) == 0 {
                break; // delivered
            }
        }
    }
}

/// Busy-wait for `ms` milliseconds by watching this CPU's timer count down.
/// Works with interrupts off, so it is usable during early boot.
pub fn spin_ms(ms: u64) {
    let per_tick = PER_TICK.load(Ordering::Relaxed) as u64;
    let mut remaining = ms * per_tick * TICK_HZ / 1000;
    let mut last = read(REG_CURRENT_COUNT) as u64;
    while remaining > 0 {
        let now = read(REG_CURRENT_COUNT) as u64;
        // The counter runs down and reloads from per_tick.
        let elapsed = if now <= last { last - now } else { last + per_tick - now };
        remaining = remaining.saturating_sub(elapsed);
        last = now;
        core::hint::spin_loop();
    }
}
