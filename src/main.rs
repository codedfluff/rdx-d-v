//! RDX — a from-scratch x86_64 kernel that boots straight from UEFI.
//!
//! The firmware loads this binary as an EFI application. `efi_main` takes the
//! framebuffer and memory map, calls ExitBootServices, and from then on the
//! kernel owns the machine: its own GDT, IDT, page tables, heap and scheduler.

#![no_std]
#![no_main]

extern crate alloc;

#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => { $crate::console::_print(format_args!($($arg)*)) };
}

#[macro_export]
macro_rules! println {
    () => { $crate::print!("\n") };
    ($($arg:tt)*) => { $crate::console::_print(format_args!("{}\n", format_args!($($arg)*))) };
}

mod acpi;
mod ahci;
mod arch;
mod clock;
mod console;
mod display;
mod disk;
mod e1000;
mod exec;
mod fs;
mod input;
mod keyboard;
mod local;
mod memory;
mod mouse;
mod net;
mod pci;
mod pipe;
mod procfs;
mod pty;
mod random;
mod sched;
mod serial;
mod shell;
mod shm;
mod smp;
mod sync;
mod syscall;
mod timer;
mod uefi;
mod xhci;

use core::panic::PanicInfo;

#[global_allocator]
static ALLOCATOR: memory::KernelAllocator = memory::KernelAllocator;

fn ok(what: &str) {
    console::set_fg(console::GREEN);
    print!("[ ok ] ");
    console::reset_fg();
    println!("{}", what);
}

#[no_mangle]
pub extern "efiapi" fn efi_main(image: uefi::Handle, system_table: *const uefi::SystemTable) -> usize {
    serial::init();
    let boot = unsafe { uefi::exit_to_kernel(image, system_table) };
    sync::cli();
    kmain(&boot)
}

fn kmain(boot: &uefi::BootInfo) -> ! {
    console::init(boot);
    display::init(boot);
    shell::banner();
    if boot.has_fb {
        ok("left UEFI boot services, framebuffer console up");
        println!("       {}x{} {}", boot.width, boot.height, if boot.bgr { "BGRX" } else { "RGBX" });
    } else {
        ok("left UEFI boot services (no framebuffer, serial only)");
    }

    arch::init_gdt();
    arch::init_idt();
    ok("GDT, TSS and IDT loaded");

    memory::init(boot);
    let m = memory::stats();
    ok("frame allocator, page tables and heap");
    println!(
        "       {} MiB usable RAM, {} MiB heap, {} GiB mapped",
        m.total_bytes / (1024 * 1024),
        m.heap_size / (1024 * 1024),
        m.mapped_gib
    );

    clock::init();
    random::init(clock::now());
    let skipped = boot.files_skipped;
    let (from_boot, from_disk) = fs::init(boot);
    ok("filesystem");
    println!("       {} file(s) loaded from \\bin on the boot disk", from_boot);
    if skipped {
        println!("       WARNING: \\bin has more than {} files; the rest were NOT loaded", uefi::MAX_BOOT_FILES);
    }
    match (from_disk, disk::info()) {
        (Some(count), Some(d)) => println!(
            "       {} saved file(s) restored from SATA disk \"{}\" ({} MiB)",
            count,
            d.model,
            d.size_bytes / (1024 * 1024)
        ),
        _ => println!("       no RDX disk attached: new files will be lost on reboot"),
    }

    acpi::init(boot.rsdp);
    arch::init_pic();
    let clock = timer::init();
    ok("system timer");
    println!("       {} Hz from the CPU's APIC timer, calibrated against {}", arch::TICK_HZ, clock);
    if keyboard::init() {
        ok("PS/2 keyboard controller");
        if mouse::init() {
            ok("PS/2 mouse");
        }
    } else {
        println!("       no PS/2 controller on this machine");
    }

    sched::init();
    let cores = smp::start_secondaries();
    ok("processors");
    println!("       {} CPU core(s) running", cores);
    xhci::start();
    match net::start() {
        Some(adapter) => {
            ok("network adapter");
            println!("       {}", adapter);
        }
        None => println!("       no supported network adapter"),
    }
    sched::spawn("init", shell::init);
    ok("scheduler started, USB driver running in the background (see: usb)");
    println!();

    // From here on this context is the idle task: it only runs when nothing
    // else can, and tidies up after dead tasks.
    sync::sti();
    loop {
        sched::reap();
        arch::hlt();
    }
}

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    sync::cli();
    arch::set_panicking(); // the other CPUs stop at their next tick
    console::panic_print(format_args!("\n*** KERNEL PANIC ***\n{}\nSystem halted.\n", info));
    arch::halt_forever()
}
