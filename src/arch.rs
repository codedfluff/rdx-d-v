//! x86_64 CPU plumbing: port I/O, GDT/TSS, IDT, interrupt entry stubs,
//! the legacy PIC and PIT, and the central interrupt dispatcher.

use core::arch::{asm, global_asm};
use core::ptr::{addr_of, addr_of_mut};
use core::sync::atomic::{AtomicBool, AtomicU64, Ordering};

use crate::{input, keyboard, sched, serial, syscall, timer};

// ---------------------------------------------------------------- port I/O

pub fn outb(port: u16, value: u8) {
    unsafe { asm!("out dx, al", in("dx") port, in("al") value, options(nomem, nostack, preserves_flags)) };
}

pub fn outw(port: u16, value: u16) {
    unsafe { asm!("out dx, ax", in("dx") port, in("ax") value, options(nomem, nostack, preserves_flags)) };
}

pub fn inb(port: u16) -> u8 {
    let value: u8;
    unsafe { asm!("in al, dx", in("dx") port, out("al") value, options(nomem, nostack, preserves_flags)) };
    value
}

pub fn outl(port: u16, value: u32) {
    unsafe { asm!("out dx, eax", in("dx") port, in("eax") value, options(nomem, nostack, preserves_flags)) };
}

pub fn inl(port: u16) -> u32 {
    let value: u32;
    unsafe { asm!("in eax, dx", in("dx") port, out("eax") value, options(nomem, nostack, preserves_flags)) };
    value
}

fn io_wait() {
    outb(0x80, 0);
}

pub fn hlt() {
    unsafe { asm!("hlt", options(nomem, nostack)) };
}

pub fn halt_forever() -> ! {
    loop {
        unsafe { asm!("cli", "hlt", options(nomem, nostack)) };
    }
}

pub fn read_cr2() -> u64 {
    let v: u64;
    unsafe { asm!("mov {}, cr2", out(reg) v, options(nomem, nostack, preserves_flags)) };
    v
}

// ---------------------------------------------------------------- GDT + TSS

pub const KERNEL_CS: u64 = 0x08;
pub const KERNEL_DS: u64 = 0x10;
pub const USER_DS: u64 = 0x18 | 3;
pub const USER_CS: u64 = 0x20 | 3;
/// Selector of CPU 0's TSS. Each CPU has its own, 16 bytes further on.
const TSS_SEL: u16 = 0x28;

/// The most CPU cores the kernel will use.
pub const MAX_CPUS: usize = 16;

static SMP_READY: AtomicBool = AtomicBool::new(false);
static PANICKING: AtomicBool = AtomicBool::new(false);

/// Which CPU is running this code: 0 for the one that booted, 1.. for the
/// others. Read from the task register, which holds a different TSS selector
/// on every CPU and cannot be changed from user mode.
pub fn cpu_id() -> usize {
    if !SMP_READY.load(Ordering::Relaxed) {
        return 0;
    }
    let tr: u16;
    unsafe { asm!("str {0:x}", out(reg) tr, options(nomem, nostack, preserves_flags)) };
    (tr.saturating_sub(TSS_SEL) / 16) as usize
}

pub fn panicking() -> bool {
    PANICKING.load(Ordering::Relaxed)
}

/// Called first thing by the panic handler; makes every other CPU stop at
/// its next timer tick or lock attempt.
pub fn set_panicking() {
    PANICKING.store(true, Ordering::Relaxed);
}

/// Tell the CPU which stack to switch to when an interrupt arrives while a
/// process is running in ring 3. Updated on every context switch.
pub fn set_kernel_stack(top: u64) {
    unsafe { (*(addr_of_mut!(TSS) as *mut Tss).add(cpu_id())).rsp[0] = top };
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct Tss {
    _reserved0: u32,
    rsp: [u64; 3],
    _reserved1: u64,
    ist: [u64; 7],
    _reserved2: u64,
    _reserved3: u16,
    iomap_base: u16,
}

#[repr(C, packed)]
struct TablePtr {
    limit: u16,
    base: u64,
}

// One GDT shared by every CPU: five flat segments, then a TSS descriptor
// (two slots each) per CPU.
const GDT_ENTRIES: usize = 5 + 2 * MAX_CPUS;
static mut GDT: [u64; GDT_ENTRIES] = [0; GDT_ENTRIES];
const EMPTY_TSS: Tss = Tss {
    _reserved0: 0,
    rsp: [0; 3],
    _reserved1: 0,
    ist: [0; 7],
    _reserved2: 0,
    _reserved3: 0,
    iomap_base: 104,
};
static mut TSS: [Tss; MAX_CPUS] = [EMPTY_TSS; MAX_CPUS];

const DF_STACK_SIZE: usize = 16 * 1024;
#[repr(C, align(16))]
struct DfStack([u8; DF_STACK_SIZE]);
static mut DF_STACK: DfStack = DfStack([0; DF_STACK_SIZE]);

/// Load the shared GDT and this CPU's own TSS.
unsafe fn load_gdt(cpu: usize) {
    let ptr = TablePtr { limit: (GDT_ENTRIES * 8 - 1) as u16, base: addr_of!(GDT) as u64 };
    asm!(
        "lgdt [{p}]",
        "push 0x08",
        "lea {tmp}, [rip + 2f]",
        "push {tmp}",
        "retfq",
        "2:",
        "mov ax, 0x10",
        "mov ds, ax",
        "mov es, ax",
        "mov ss, ax",
        "mov fs, ax",
        "mov gs, ax",
        "ltr {tss:x}",
        p = in(reg) &ptr,
        tmp = out(reg) _,
        tss = in(reg) TSS_SEL + 16 * cpu as u16,
        out("rax") _,
    );
}

/// Give CPU `cpu` the stack it uses for double faults (IST1), so a blown
/// kernel stack still produces a report instead of a triple fault.
pub fn set_fault_stack(cpu: usize, top: u64) {
    unsafe { (*(addr_of_mut!(TSS) as *mut Tss).add(cpu)).ist[0] = top };
}

pub fn init_gdt() {
    unsafe {
        // Syscalls read and write process memory directly, so make sure the
        // firmware didn't leave SMEP/SMAP enabled (CR4 bits 20 and 21).
        asm!(
            "mov {tmp}, cr4",
            "and {tmp}, {mask}",
            "mov cr4, {tmp}",
            tmp = out(reg) _,
            mask = in(reg) !((1u64 << 20) | (1u64 << 21)),
            options(nostack),
        );

        set_fault_stack(0, addr_of_mut!(DF_STACK) as u64 + DF_STACK_SIZE as u64);

        let gdt = addr_of_mut!(GDT) as *mut u64;
        gdt.add(0).write(0);
        gdt.add(1).write(0x00AF_9A00_0000_FFFF); // 0x08 kernel code (64-bit)
        gdt.add(2).write(0x00CF_9200_0000_FFFF); // 0x10 kernel data
        gdt.add(3).write(0x00CF_F200_0000_FFFF); // 0x18 user data (ring 3)
        gdt.add(4).write(0x00AF_FA00_0000_FFFF); // 0x20 user code (ring 3)
        let tss_limit = (core::mem::size_of::<Tss>() - 1) as u64;
        for cpu in 0..MAX_CPUS {
            let tss_base = (addr_of!(TSS) as *const Tss).add(cpu) as u64;
            gdt.add(5 + 2 * cpu).write(
                (tss_limit & 0xFFFF)
                    | ((tss_base & 0xFF_FFFF) << 16)
                    | (0x89u64 << 40)
                    | (((tss_limit >> 16) & 0xF) << 48)
                    | (((tss_base >> 24) & 0xFF) << 56),
            );
            gdt.add(6 + 2 * cpu).write(tss_base >> 32);
        }
        load_gdt(0);
        SMP_READY.store(true, Ordering::Relaxed);
    }
}

/// CPU set-up for a secondary core: the same GDT and IDT as CPU 0, its own
/// TSS. Must not take any lock before this has run.
pub fn init_secondary(cpu: usize) {
    unsafe {
        load_gdt(cpu);
        let ptr = TablePtr { limit: (256 * 16 - 1) as u16, base: addr_of!(IDT) as u64 };
        asm!("lidt [{}]", in(reg) &ptr, options(readonly, nostack, preserves_flags));
    }
}

// ---------------------------------------------------------------- IDT

/// Register state saved by the interrupt entry stubs, lowest address first.
#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct Frame {
    pub r15: u64,
    pub r14: u64,
    pub r13: u64,
    pub r12: u64,
    pub r11: u64,
    pub r10: u64,
    pub r9: u64,
    pub r8: u64,
    pub rbp: u64,
    pub rdi: u64,
    pub rsi: u64,
    pub rdx: u64,
    pub rcx: u64,
    pub rbx: u64,
    pub rax: u64,
    pub vector: u64,
    pub error: u64,
    pub rip: u64,
    pub cs: u64,
    pub rflags: u64,
    pub rsp: u64,
    pub ss: u64,
}

/// Size of the FXSAVE area the stubs keep directly below every `Frame`.
pub const FX_AREA: usize = 512;

pub const VEC_KEYBOARD: u64 = 33;
pub const VEC_MOUSE: u64 = 44;
pub const VEC_SYSCALL: u64 = 0x80;
pub const VEC_YIELD: u64 = 0x81;

// Entry stubs. Each is padded to 16 bytes so stub N lives at isr_stubs + 16*N.
// Slots 0..47 are vectors 0..47; slot 48 is the system call vector (0x80),
// slot 49 the kernel-internal yield vector (0x81), slot 50 the APIC timer
// (vector 48), slot 51 the APIC spurious vector (255) and slot 52 the
// "reschedule now" interrupt one CPU sends another (vector 49).
//
// isr_common saves all GPRs plus the FPU/SSE state, hands the frame to
// `interrupt_dispatch`, and resumes whichever frame it returns — that return
// value is how context switches happen. Once it is running on the stack of
// the frame it is about to resume, it calls `interrupt_exit`, which is where
// the scheduler lets go of its lock: only then is the previous task's stack
// safe for another CPU to use. That call comes before the FPU/SSE state is
// restored, so `interrupt_exit` may use those registers freely (and may
// rewrite the frame and the saved state, which is how signals are delivered).
global_asm!(
    r#"
.macro ISR_NOERR num
.balign 16
    push 0
    push \num
    jmp isr_common
.endm

.macro ISR_ERR num
.balign 16
    push \num
    jmp isr_common
.endm

.text
.balign 16
.global isr_stubs
isr_stubs:
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR 8
ISR_NOERR 9
ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR 29
ISR_ERR 30
ISR_NOERR 31
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47
ISR_NOERR 128
ISR_NOERR 129
ISR_NOERR 48
ISR_NOERR 255
ISR_NOERR 49

.balign 16
isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld
    mov rbp, rsp
    lea rsp, [rbp - 512]
    and rsp, -16
    fxsave64 [rsp]
    mov rdi, rbp
    call interrupt_dispatch
    mov rbp, rax
    lea rsp, [rbp - 512]
    and rsp, -16
    mov rdi, rbp
    call interrupt_exit
    fxrstor64 [rsp]
    mov rsp, rbp
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq
"#
);

extern "C" {
    static isr_stubs: u8;
}

#[repr(C)]
#[derive(Clone, Copy)]
struct IdtEntry {
    offset_low: u16,
    selector: u16,
    ist: u8,
    type_attr: u8,
    offset_mid: u16,
    offset_high: u32,
    zero: u32,
}

const IDT_EMPTY: IdtEntry =
    IdtEntry { offset_low: 0, selector: 0, ist: 0, type_attr: 0, offset_mid: 0, offset_high: 0, zero: 0 };
static mut IDT: [IdtEntry; 256] = [IDT_EMPTY; 256];

/// `user` makes the gate reachable with `int` from ring 3.
unsafe fn set_gate(vector: usize, handler: u64, ist: u8, user: bool) {
    let idt = addr_of_mut!(IDT) as *mut IdtEntry;
    idt.add(vector).write(IdtEntry {
        offset_low: handler as u16,
        selector: KERNEL_CS as u16,
        ist,
        type_attr: if user { 0xEE } else { 0x8E }, // present interrupt gate, DPL 3 or 0
        offset_mid: (handler >> 16) as u16,
        offset_high: (handler >> 32) as u32,
        zero: 0,
    });
}

pub fn init_idt() {
    unsafe {
        let base = addr_of!(isr_stubs) as u64;
        for v in 0..48usize {
            set_gate(v, base + 16 * v as u64, if v == 8 { 1 } else { 0 }, false);
        }
        set_gate(VEC_SYSCALL as usize, base + 16 * 48, 0, true);
        set_gate(VEC_YIELD as usize, base + 16 * 49, 0, false);
        set_gate(timer::VEC_TIMER as usize, base + 16 * 50, 0, false);
        set_gate(timer::VEC_SPURIOUS as usize, base + 16 * 51, 0, false);
        set_gate(timer::VEC_RESCHEDULE as usize, base + 16 * 52, 0, false);
        let ptr = TablePtr { limit: (256 * 16 - 1) as u16, base: addr_of!(IDT) as u64 };
        asm!("lidt [{}]", in(reg) &ptr, options(readonly, nostack, preserves_flags));
    }
}

// ---------------------------------------------------------------- PIC + PIT

const PIC1: u16 = 0x20;
const PIC2: u16 = 0xA0;
pub const TICK_HZ: u64 = 100;

static TICKS: AtomicU64 = AtomicU64::new(0);

pub fn ticks() -> u64 {
    TICKS.load(Ordering::Relaxed)
}

/// Count one tick of the system clock. Called from the timer interrupt.
fn tick() {
    if panicking() {
        halt_forever();
    }
    // Every CPU gets timer interrupts (they drive its scheduling), but the
    // clock and input polling belong to CPU 0 alone.
    if cpu_id() != 0 {
        return;
    }
    TICKS.fetch_add(1, Ordering::Relaxed);
    // Input is polled here as well as taken from interrupts, so a machine
    // whose legacy interrupt wiring is missing still has a keyboard.
    keyboard::poll();
    while let Some(b) = serial::try_read() {
        input::push(match b {
            b'\r' => b'\n',
            0x7F => 0x08,
            other => other,
        });
    }
}

pub fn init_pic() {
    // Remap the 8259s so IRQ 0..15 land on vectors 32..47 instead of on top
    // of the CPU exceptions. Only the keyboard line is left enabled: the
    // system tick comes from the local APIC timer (see timer.rs), and the
    // kernel works even if these chips do not exist.
    outb(PIC1, 0x11);
    io_wait();
    outb(PIC2, 0x11);
    io_wait();
    outb(PIC1 + 1, 32);
    io_wait();
    outb(PIC2 + 1, 40);
    io_wait();
    outb(PIC1 + 1, 4);
    io_wait();
    outb(PIC2 + 1, 2);
    io_wait();
    outb(PIC1 + 1, 1);
    io_wait();
    outb(PIC2 + 1, 1);
    io_wait();
    outb(PIC1 + 1, 0xF9); // unmask keyboard and cascade only...
    outb(PIC2 + 1, 0xEF); // ...and the PS/2 mouse (IRQ 12)
}

fn pic_eoi(irq: u64) {
    if irq >= 8 {
        outb(PIC2, 0x20);
    }
    outb(PIC1, 0x20);
}

// ---------------------------------------------------------------- dispatch

const EXCEPTION_NAMES: [&str; 32] = [
    "divide error",
    "debug",
    "non-maskable interrupt",
    "breakpoint",
    "overflow",
    "bound range exceeded",
    "invalid opcode",
    "device not available",
    "double fault",
    "coprocessor segment overrun",
    "invalid TSS",
    "segment not present",
    "stack-segment fault",
    "general protection fault",
    "page fault",
    "reserved",
    "x87 floating-point error",
    "alignment check",
    "machine check",
    "SIMD floating-point error",
    "virtualization exception",
    "control protection exception",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "hypervisor injection",
    "VMM communication",
    "security exception",
    "reserved",
];

/// Last thing every interrupt does, on the stack of the task being resumed
/// and with that task's saved registers in `frame`.
#[no_mangle]
extern "sysv64" fn interrupt_exit(frame: *mut Frame) {
    sched::finish_switch();
    // About to drop back into a program: give it any signal it is owed.
    if unsafe { (*frame).cs } & 3 == 3 {
        sched::deliver_signals(frame);
    }
}

#[no_mangle]
extern "sysv64" fn interrupt_dispatch(frame: *mut Frame) -> *mut Frame {
    let vector = unsafe { (*frame).vector };
    crate::random::stir(); // when interrupts happen is a source of randomness
    match vector {
        0..=31 => handle_exception(frame),
        timer::VEC_TIMER => {
            tick();
            timer::eoi();
            sched::schedule(frame, true)
        }
        timer::VEC_SPURIOUS => frame,
        // Another CPU made a task runnable and saw this one idle.
        timer::VEC_RESCHEDULE => {
            timer::eoi();
            sched::schedule(frame, false)
        }
        VEC_KEYBOARD => {
            keyboard::poll();
            pic_eoi(1);
            // A key may have woken a waiting task: run it now, not at the
            // next tick.
            sched::schedule(frame, false)
        }
        VEC_MOUSE => {
            keyboard::poll(); // the mouse's bytes come through the same port
            pic_eoi(12);
            sched::schedule(frame, false)
        }
        VEC_SYSCALL => syscall::handle(frame),
        VEC_YIELD => sched::schedule(frame, false),
        39 => frame, // spurious IRQ7: no EOI
        47 => {
            outb(PIC1, 0x20); // spurious IRQ15: EOI the master only
            frame
        }
        32..=47 => {
            pic_eoi(vector - 32);
            frame
        }
        _ => frame,
    }
}

fn handle_exception(frame: *mut Frame) -> *mut Frame {
    let f = unsafe { &*frame };
    let name = EXCEPTION_NAMES[f.vector as usize];

    // Breakpoints are harmless: report and carry on.
    if f.vector == 3 {
        crate::println!("[breakpoint at {:#x}]", f.rip);
        return frame;
    }

    // A program's main stack grows on demand: a user-mode fault on a page
    // not mapped yet, just below what is, within its limit, gets a page.
    if f.vector == 14 && f.cs & 3 == 3 && f.error & 1 == 0 {
        let address = read_cr2();
        let lowest = crate::exec::STACK_TOP - crate::exec::STACK_MAX;
        if (lowest..crate::exec::STACK_TOP).contains(&address) {
            if let Some(process) = sched::current_process() {
                let page = address & !(crate::memory::PAGE - 1);
                if crate::memory::map_user_page(process.space, page, true).is_some() {
                    return frame; // run the instruction again
                }
            }
        }
    }

    // A fault inside an ordinary task kills that task, not the kernel.
    if f.vector != 8 && f.vector != 18 {
        if let Some((id, next)) = sched::kill_current(frame) {
            crate::console::set_fg(crate::console::RED);
            let ring = if f.cs & 3 == 3 { "user" } else { "kernel" };
            crate::print!("task {} crashed in {} mode: {} (error {:#x}) at rip {:#x}", id, ring, name, f.error, f.rip);
            if f.vector == 14 {
                crate::print!(", address {:#x}", read_cr2());
            }
            crate::println!();
            crate::console::reset_fg();
            return next;
        }
    }

    panic!(
        "CPU exception {}: {}\n  error={:#x} rip={:#x} cs={:#x} rflags={:#x}\n  rsp={:#x} ss={:#x} cr2={:#x}\n  rax={:#018x} rbx={:#018x} rcx={:#018x}\n  rdx={:#018x} rsi={:#018x} rdi={:#018x}\n  rbp={:#018x} r8 ={:#018x} r9 ={:#018x}",
        f.vector, name, f.error, f.rip, f.cs, f.rflags, f.rsp, f.ss, read_cr2(),
        f.rax, f.rbx, f.rcx, f.rdx, f.rsi, f.rdi, f.rbp, f.r8, f.r9
    );
}

// ---------------------------------------------------------------- power

pub fn reboot() -> ! {
    crate::sync::cli();
    // The proper way: the reset register the firmware describes in ACPI.
    if let Some((is_port, address, value)) = crate::acpi::reset_info() {
        crate::println!("ACPI: restarting through the reset register at {:#x}", address);
        if is_port {
            outb(address as u16, value);
        } else if let Some(mapped) = crate::memory::map_mmio(address, 1) {
            unsafe { (mapped as *mut u8).write_volatile(value) };
        }
        for _ in 0..10_000_000 {
            core::hint::spin_loop();
        }
    }
    // Failing that, pulse the CPU reset line via the keyboard controller...
    for _ in 0..10_000 {
        if inb(0x64) & 0x02 == 0 {
            break;
        }
    }
    outb(0x64, 0xFE);
    for _ in 0..1_000_000 {
        core::hint::spin_loop();
    }
    // ...and if that did nothing, triple-fault on purpose.
    unsafe {
        let null = TablePtr { limit: 0, base: 0 };
        asm!("lidt [{}]", "int3", in(reg) &null);
    }
    halt_forever()
}

pub fn shutdown() -> ! {
    crate::sync::cli();
    // The proper way, which works on real hardware: ACPI sleep state S5.
    match crate::acpi::power_off_info() {
        Some(how) => {
            crate::println!("ACPI: powering off (S5 type {} through port {:#x})", how.type_a, how.port_a);
            crate::acpi::power_off(&how);
        }
        None => crate::println!("ACPI: this firmware does not describe how to power off"),
    }
    // Last resorts, for emulators whose tables we could not read.
    outw(0x604, 0x2000); // QEMU
    outw(0xB004, 0x2000); // Bochs / old QEMU
    outw(0x4004, 0x3400); // VirtualBox
    crate::println!("It is now safe to turn off your computer.");
    halt_forever()
}
