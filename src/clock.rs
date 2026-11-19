//! Wall-clock time.
//!
//! The battery-backed hardware clock (the CMOS RTC) is read once at boot;
//! after that the time is that reading plus the system tick count, so
//! asking for it is cheap and never touches the hardware again.
//!
//! Times are whole seconds since 1970-01-01 00:00:00, taking the hardware
//! clock to be UTC (RDX has no time zones).

use alloc::format;
use alloc::string::String;
use core::sync::atomic::{AtomicU64, Ordering};

use crate::arch::{self, inb, outb, TICK_HZ};

/// The time at tick zero.
static BOOT_TIME: AtomicU64 = AtomicU64::new(0);

fn cmos(reg: u8) -> u8 {
    outb(0x70, reg);
    inb(0x71)
}

/// (year, month, day, hour, minute, second) straight from the RTC.
fn read_rtc() -> (u64, u64, u64, u64, u64, u64) {
    // Wait out any in-progress update so we don't read a torn value.
    for _ in 0..100_000 {
        if cmos(0x0A) & 0x80 == 0 {
            break;
        }
    }
    let status_b = cmos(0x0B);
    let bcd = status_b & 0x04 == 0;
    let conv = |v: u8| (if bcd { (v & 0x0F) + (v >> 4) * 10 } else { v }) as u64;
    let raw_hour = cmos(0x04);
    let mut hour = conv(raw_hour & 0x7F);
    if status_b & 0x02 == 0 {
        // 12-hour mode: bit 7 of the hour means PM.
        hour %= 12;
        if raw_hour & 0x80 != 0 {
            hour += 12;
        }
    }
    (2000 + conv(cmos(0x09)), conv(cmos(0x08)), conv(cmos(0x07)), hour, conv(cmos(0x02)), conv(cmos(0x00)))
}

/// Days from 1970-01-01 to the given date (proleptic Gregorian calendar).
fn days_from_civil(year: u64, month: u64, day: u64) -> u64 {
    let y = if month <= 2 { year - 1 } else { year };
    let era = y / 400;
    let yoe = y - era * 400;
    let m = (month + 9) % 12; // March = 0
    let doy = (153 * m + 2) / 5 + day - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe - 719_468
}

/// The reverse: (year, month, day) for a day count.
fn civil_from_days(days: u64) -> (u64, u64, u64) {
    let z = days + 719_468;
    let era = z / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    (yoe + era * 400 + if month <= 2 { 1 } else { 0 }, month, day)
}

/// Read the hardware clock. Call once the tick counter is running (or
/// before: ticks are then zero, which is just as good).
pub fn init() {
    let (year, month, day, hour, minute, second) = read_rtc();
    let month = month.clamp(1, 12);
    let day = day.clamp(1, 31);
    let now = days_from_civil(year, month, day) * 86_400 + hour * 3600 + minute * 60 + second;
    BOOT_TIME.store(now.saturating_sub(arch::ticks() / TICK_HZ), Ordering::Relaxed);
}

/// Seconds since 1970-01-01 00:00:00.
pub fn now() -> u64 {
    BOOT_TIME.load(Ordering::Relaxed) + arch::ticks() / TICK_HZ
}

/// "2026-11-13 14:02:11"
pub fn format(seconds: u64) -> String {
    let (year, month, day) = civil_from_days(seconds / 86_400);
    let t = seconds % 86_400;
    format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", year, month, day, t / 3600, t / 60 % 60, t % 60)
}
