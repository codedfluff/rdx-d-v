//! /proc: a folder of files that are not stored anywhere. Reading one asks
//! the kernel a question and gets the answer as text - memory in use, the
//! network's state, what the USB driver has found. It is how programs (and
//! `cat`) see inside the kernel without a system call for every fact.
//!
//! The filesystem consults this module for any path under `proc/`.

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::Write;

use crate::arch::{self, TICK_HZ};
use crate::{console, disk, memory, net, smp, xhci};

/// The directory's key in the filesystem.
pub const DIR: &str = "proc";

/// The files in /proc.
pub const ENTRIES: &[&str] =
    &["about", "cpu", "disk", "display", "mem", "memmap", "net", "uptime", "usb", "version"];

/// If `key` is a path inside /proc, the name of the file it refers to.
pub fn entry(key: &str) -> Option<&str> {
    key.strip_prefix("proc/")
}

/// The current contents of a /proc file.
pub fn read(name: &str) -> Option<String> {
    Some(match name {
        "about" => about(),
        "cpu" => cpu(),
        "disk" => disk_info(),
        "display" => crate::display::status(),
        "mem" => mem(),
        "memmap" => memmap(),
        "net" => lines(net::status()),
        "uptime" => uptime(),
        "usb" => {
            let status = xhci::status();
            if status.is_empty() { String::from("The USB driver is still starting up.\n") } else { lines(status) }
        }
        "version" => format!("RDX {}\n", env!("CARGO_PKG_VERSION")),
        _ => return None,
    })
}

fn lines(items: Vec<String>) -> String {
    let mut text = String::new();
    for item in items {
        text.push_str(&item);
        text.push('\n');
    }
    text
}

fn about() -> String {
    let (cols, rows) = console::size();
    format!(
        "RDX {} - a from-scratch x86_64 kernel written in Rust.\n\
         Boots directly from UEFI, no bootloader in between.\n\
         Console: {}x{} cells. Timer: {} Hz. Scheduler: preemptive round-robin on {} CPU core(s).\n",
        env!("CARGO_PKG_VERSION"),
        cols,
        rows,
        TICK_HZ,
        smp::online()
    )
}

fn mem() -> String {
    let s = memory::stats();
    let mib = |b: u64| b as f64 / (1024.0 * 1024.0);
    format!(
        "physical: {:.1} MiB used of {:.1} MiB usable\n\
         heap:     {:.1} KiB used of {:.1} MiB\n\
         paging:   {} GiB identity-mapped (2 MiB pages), first 2 MiB unmapped\n",
        mib(s.used_bytes),
        mib(s.total_bytes),
        s.heap_used as f64 / 1024.0,
        mib(s.heap_size as u64),
        s.mapped_gib
    )
}

fn memmap() -> String {
    let mut regions = [memory::Region { start: 0, end: 0 }; memory::REGION_CAPACITY];
    let n = memory::regions(&mut regions);
    let mut text = String::new();
    for r in &regions[..n] {
        if r.end > r.start {
            let _ = writeln!(text, "  {:#012x} - {:#012x}  {:>8} KiB", r.start, r.end, (r.end - r.start) / 1024);
        }
    }
    text
}

fn uptime() -> String {
    let t = arch::ticks();
    let secs = t / TICK_HZ;
    format!(
        "up {:02}:{:02}:{:02}.{:02} ({} ticks)\n",
        secs / 3600,
        secs / 60 % 60,
        secs % 60,
        t % TICK_HZ * 100 / TICK_HZ,
        t
    )
}

fn cpu() -> String {
    #[allow(unused_unsafe)]
    let cpuid = |leaf: u32| unsafe { core::arch::x86_64::__cpuid(leaf) };
    let mut text = String::new();

    let v = cpuid(0);
    let mut vendor = [0u8; 12];
    vendor[0..4].copy_from_slice(&v.ebx.to_le_bytes());
    vendor[4..8].copy_from_slice(&v.edx.to_le_bytes());
    vendor[8..12].copy_from_slice(&v.ecx.to_le_bytes());
    let _ = writeln!(text, "vendor: {}", core::str::from_utf8(&vendor).unwrap_or("?"));

    if cpuid(0x8000_0000).eax >= 0x8000_0004 {
        let mut brand = [0u8; 48];
        for (i, leaf) in (0x8000_0002u32..=0x8000_0004).enumerate() {
            let r = cpuid(leaf);
            for (j, reg) in [r.eax, r.ebx, r.ecx, r.edx].iter().enumerate() {
                brand[i * 16 + j * 4..][..4].copy_from_slice(&reg.to_le_bytes());
            }
        }
        let end = brand.iter().position(|&b| b == 0).unwrap_or(48);
        let _ = writeln!(text, "model:  {}", core::str::from_utf8(&brand[..end]).unwrap_or("?").trim());
    }

    let f = cpuid(1);
    let mut features: Vec<&str> = Vec::new();
    for (bit, name) in [(0, "fpu"), (4, "tsc"), (9, "apic"), (25, "sse"), (26, "sse2")] {
        if f.edx & (1 << bit) != 0 {
            features.push(name);
        }
    }
    for (bit, name) in [(0, "sse3"), (9, "ssse3"), (19, "sse4.1"), (20, "sse4.2"), (28, "avx"), (30, "rdrand")] {
        if f.ecx & (1 << bit) != 0 {
            features.push(name);
        }
    }
    let _ = writeln!(text, "flags:  {}", features.join(" "));
    let _ = writeln!(text, "cores:  {}", smp::online());
    text
}

fn disk_info() -> String {
    match disk::info() {
        Some(d) => format!(
            "{} disk \"{}\", {} MiB\n\
             your files use {} bytes of {} KiB available ({} saves so far)\n\
             Files you create or change are saved to it and survive a reboot.\n",
            d.kind,
            d.model,
            d.size_bytes / (1024 * 1024),
            d.used_bytes,
            d.capacity_bytes / 1024,
            d.saves
        ),
        None => String::from(
            "No RDX disk is attached, so files only live in memory.\n\
             RDX only uses a disk that carries its own signature; run.cmd creates one.\n",
        ),
    }
}
