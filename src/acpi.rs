//! Just enough ACPI to look up a firmware table by its four-letter name,
//! and to turn the machine off or restart it.

use core::sync::atomic::{AtomicU64, Ordering};

static RSDP: AtomicU64 = AtomicU64::new(0);

/// Remember where the firmware's root pointer is (0 if it gave us none).
pub fn init(rsdp: u64) {
    if rsdp != 0 && unsafe { core::slice::from_raw_parts(rsdp as *const u8, 8) } == b"RSD PTR " {
        RSDP.store(rsdp, Ordering::Relaxed);
    }
}

unsafe fn u32_at(address: u64) -> u32 {
    (address as *const u32).read_unaligned()
}

unsafe fn u64_at(address: u64) -> u64 {
    (address as *const u64).read_unaligned()
}

/// Physical address of the table with this signature, e.g. b"HPET".
pub fn find_table(signature: &[u8; 4]) -> Option<u64> {
    let rsdp = RSDP.load(Ordering::Relaxed);
    if rsdp == 0 {
        return None;
    }
    unsafe {
        let revision = *((rsdp + 15) as *const u8);
        let xsdt = if revision >= 2 { u64_at(rsdp + 24) } else { 0 };
        // The root table is a 36-byte header followed by pointers to the
        // other tables: 64-bit ones in the XSDT, 32-bit in the older RSDT.
        let (root, entry_size) = if xsdt != 0 { (xsdt, 8) } else { (u32_at(rsdp + 16) as u64, 4) };
        if root == 0 {
            return None;
        }
        let length = u32_at(root + 4) as u64;
        if !(36..=64 * 1024).contains(&length) {
            return None;
        }
        for i in 0..(length - 36) / entry_size {
            let entry = root + 36 + i * entry_size;
            let table = if entry_size == 8 { u64_at(entry) } else { u32_at(entry) as u64 };
            if table != 0 && core::slice::from_raw_parts(table as *const u8, 4) == signature {
                return Some(table);
            }
        }
    }
    None
}

// ---------------------------------------------------------------- power
//
// Turning the machine off and restarting it are ACPI jobs. The fixed
// description table (FADT) says where the power-management control
// register is; the value that means "soft off" (sleep state S5) has to be
// dug out of the firmware's bytecode table (DSDT), where it is stored as a
// small package named _S5_.

/// The PM1 control ports and the S5 sleep-type values for each.
pub struct PowerOff {
    pub port_a: u16,
    pub port_b: u16,
    pub type_a: u16,
    pub type_b: u16,
}

/// Find the `_S5_` package in the DSDT and return its first two values.
unsafe fn find_s5(dsdt: u64) -> Option<(u16, u16)> {
    if dsdt == 0 || core::slice::from_raw_parts(dsdt as *const u8, 4) != b"DSDT" {
        return None;
    }
    let length = u32_at(dsdt + 4) as usize;
    if !(36..=4 * 1024 * 1024).contains(&length) {
        return None;
    }
    let code = core::slice::from_raw_parts(dsdt as *const u8, length);
    let at = code.windows(4).position(|w| w == b"_S5_")?;
    // NameOp(_S5_) PackageOp PkgLength NumElements element element ...
    let mut p = at + 4;
    if *code.get(p)? != 0x12 {
        return None;
    }
    p += 1;
    p += 1 + (*code.get(p)? >> 6) as usize; // PkgLength is 1 to 4 bytes
    p += 1; // NumElements
    let mut values = [0u16; 2];
    for value in values.iter_mut() {
        match *code.get(p)? {
            0x0A => {
                *value = *code.get(p + 1)? as u16; // BytePrefix
                p += 2;
            }
            byte => {
                *value = byte as u16; // ZeroOp, OneOp or a bare small value
                p += 1;
            }
        }
    }
    Some((values[0] & 7, values[1] & 7))
}

/// How to switch this machine off, if the firmware tells us.
pub fn power_off_info() -> Option<PowerOff> {
    let fadt = find_table(b"FACP")?;
    unsafe {
        let length = u32_at(fadt + 4) as u64;
        let port_a = u32_at(fadt + 64) as u16;
        let port_b = u32_at(fadt + 68) as u16;
        if port_a == 0 {
            return None; // "hardware-reduced" ACPI: no PM1 register
        }
        let dsdt = if length >= 148 && u64_at(fadt + 140) != 0 { u64_at(fadt + 140) } else { u32_at(fadt + 40) as u64 };
        let (type_a, type_b) = find_s5(dsdt)?;
        Some(PowerOff { port_a, port_b, type_a, type_b })
    }
}

/// Make sure the firmware has handed power management over to the OS
/// (the SCI_EN bit), asking it to if not. UEFI machines normally have.
unsafe fn enable(fadt: u64, port_a: u16) {
    let inw = |port: u16| -> u16 {
        let value: u16;
        core::arch::asm!("in ax, dx", in("dx") port, out("ax") value, options(nomem, nostack, preserves_flags));
        value
    };
    if inw(port_a) & 1 != 0 {
        return;
    }
    let smi_port = u32_at(fadt + 48) as u16;
    let enable_value = *((fadt + 52) as *const u8);
    if smi_port == 0 || enable_value == 0 {
        return;
    }
    crate::arch::outb(smi_port, enable_value);
    for _ in 0..3_000_000 {
        if inw(port_a) & 1 != 0 {
            break;
        }
        core::hint::spin_loop();
    }
}

/// Enter sleep state S5 ("soft off"). Returns only if that did not work.
pub fn power_off(how: &PowerOff) {
    const SLEEP_ENABLE: u16 = 1 << 13;
    unsafe {
        if let Some(fadt) = find_table(b"FACP") {
            enable(fadt, how.port_a);
        }
    }
    crate::arch::outw(how.port_a, how.type_a << 10 | SLEEP_ENABLE);
    if how.port_b != 0 {
        crate::arch::outw(how.port_b, how.type_b << 10 | SLEEP_ENABLE);
    }
    for _ in 0..50_000_000 {
        core::hint::spin_loop();
    }
}

/// The FADT's reset register: (is it an I/O port, address, value to write).
pub fn reset_info() -> Option<(bool, u64, u8)> {
    let fadt = find_table(b"FACP")?;
    unsafe {
        let length = u32_at(fadt + 4);
        let flags = u32_at(fadt + 112);
        if length < 129 || flags & (1 << 10) == 0 {
            return None; // no reset register on this machine
        }
        let space = *((fadt + 116) as *const u8);
        let address = u64_at(fadt + 120);
        let value = *((fadt + 128) as *const u8);
        match space {
            0 if address != 0 => Some((false, address, value)),
            1 if address != 0 && address <= 0xFFFF => Some((true, address, value)),
            _ => None,
        }
    }
}
