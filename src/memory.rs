//! Physical frame allocator, kernel page tables, and the kernel heap.

use core::alloc::{GlobalAlloc, Layout};
use core::arch::asm;
use core::ptr::null_mut;
use core::sync::atomic::{AtomicU64, Ordering};

use crate::sync::IrqLock;
use crate::uefi::{BootInfo, MEM_CONVENTIONAL};

pub const PAGE: u64 = 4096;
const MIB: u64 = 1024 * 1024;
const GIB: u64 = 1024 * MIB;

// ---------------------------------------------------------------- frames

#[derive(Clone, Copy)]
pub struct Region {
    pub start: u64,
    pub end: u64,
}

const MAX_REGIONS: usize = 64;

struct FrameAllocator {
    regions: [Region; MAX_REGIONS],
    count: usize,
    current: usize,
    next: u64,
    free_head: u64,
    total: u64,
    used: u64,
}

static FRAMES: IrqLock<FrameAllocator> = IrqLock::new(FrameAllocator {
    regions: [Region { start: 0, end: 0 }; MAX_REGIONS],
    count: 0,
    current: 0,
    next: 0,
    free_head: 0,
    total: 0,
    used: 0,
});

impl FrameAllocator {
    fn alloc(&mut self) -> Option<u64> {
        if self.free_head != 0 {
            let frame = self.free_head;
            self.free_head = unsafe { *(frame as *const u64) };
            self.used += 1;
            return Some(frame);
        }
        while self.current < self.count {
            if self.next + PAGE <= self.regions[self.current].end {
                let frame = self.next;
                self.next += PAGE;
                self.used += 1;
                return Some(frame);
            }
            self.current += 1;
            if self.current < self.count {
                self.next = self.regions[self.current].start;
            }
        }
        None
    }

    /// Carve `pages` contiguous frames off the tail of the roomiest region.
    fn alloc_contiguous(&mut self, pages: u64) -> Option<u64> {
        let size = pages * PAGE;
        let mut best: Option<(usize, u64)> = None;
        for i in self.current..self.count {
            let start = if i == self.current { self.next } else { self.regions[i].start };
            let avail = self.regions[i].end.saturating_sub(start);
            if avail >= size && best.map_or(true, |(_, b)| avail > b) {
                best = Some((i, avail));
            }
        }
        let (i, _) = best?;
        self.regions[i].end -= size;
        self.used += pages;
        Some(self.regions[i].end)
    }
}

pub fn alloc_frame() -> Option<u64> {
    FRAMES.lock().alloc()
}

pub fn free_frame(frame: u64) {
    let mut f = FRAMES.lock();
    unsafe { *(frame as *mut u64) = f.free_head };
    f.free_head = frame;
    f.used -= 1;
}

fn alloc_zeroed_frame() -> u64 {
    let frame = alloc_frame().expect("out of physical memory");
    unsafe { core::ptr::write_bytes(frame as *mut u8, 0, PAGE as usize) };
    frame
}

// ---------------------------------------------------------------- paging

const PTE_PRESENT: u64 = 1;
const PTE_WRITE: u64 = 1 << 1;
const PTE_HUGE: u64 = 1 << 7;

/// Replace the firmware's page tables with our own: an identity map of
/// everything below `limit` using 2 MiB pages, except the first 2 MiB, which
/// stays unmapped so null-pointer dereferences fault instead of scribbling.
unsafe fn init_paging(limit: u64) -> u64 {
    let gibs = ((limit + GIB - 1) / GIB).clamp(4, 512);
    let pml4 = alloc_zeroed_frame() as *mut u64;
    let pdpt = alloc_zeroed_frame() as *mut u64;
    pml4.write(pdpt as u64 | PTE_PRESENT | PTE_WRITE);
    for g in 0..gibs {
        let pd = alloc_zeroed_frame() as *mut u64;
        for i in 0..512u64 {
            let addr = g * GIB + i * 2 * MIB;
            if addr != 0 {
                pd.add(i as usize).write(addr | PTE_PRESENT | PTE_WRITE | PTE_HUGE);
            }
        }
        pdpt.add(g as usize).write(pd as u64 | PTE_PRESENT | PTE_WRITE);
    }
    asm!("mov cr3, {}", in(reg) pml4 as u64, options(nostack, preserves_flags));
    KERNEL_PML4.store(pml4 as u64, Ordering::Relaxed);
    IDENTITY_LIMIT.store(gibs * GIB, Ordering::Relaxed);
    gibs
}

/// A free page of conventional memory below 640 KiB, if the firmware left
/// one. Secondary CPUs start in 16-bit real mode and can only run code there.
static LOW_PAGE: AtomicU64 = AtomicU64::new(0);

pub fn low_page() -> Option<u64> {
    let page = LOW_PAGE.load(Ordering::Relaxed);
    (page != 0).then_some(page)
}

/// Temporarily identity-map (or unmap again) the first 2 MiB, which is
/// normally left unmapped to catch null pointers. Needed while secondary
/// CPUs run their start-up code from low memory.
pub fn map_low_memory(mapped: bool) {
    unsafe {
        let pml4 = kernel_address_space() as *const u64;
        let pdpt = (*pml4 & PTE_ADDR) as *const u64;
        let pd = (*pdpt & PTE_ADDR) as *mut u64;
        *pd = if mapped { PTE_PRESENT | PTE_WRITE | PTE_HUGE } else { 0 };
    }
    flush_tlb();
}

/// Discard this CPU's cached address translations.
pub fn flush_tlb() {
    unsafe {
        asm!("mov {tmp}, cr3", "mov cr3, {tmp}", tmp = out(reg) _, options(nostack, preserves_flags));
    }
}

/// End of the identity-mapped region set up at boot.
static IDENTITY_LIMIT: AtomicU64 = AtomicU64::new(0);
/// Device registers that lie beyond it are mapped into this window of
/// otherwise unused kernel address space, wherever they physically are.
const MMIO_WINDOW_START: u64 = 384 * GIB;
const MMIO_WINDOW_END: u64 = 512 * GIB;
static MMIO_NEXT: AtomicU64 = AtomicU64::new(MMIO_WINDOW_START);

/// Make a device's memory-mapped registers reachable and return the address
/// to use for them. Devices below the identity-mapped limit are used as-is;
/// firmware often places others far higher (even above 512 GiB), and those
/// get a mapping in the kernel's MMIO window.
pub fn map_mmio(phys: u64, len: u64) -> Option<u64> {
    let end = phys.checked_add(len)?;
    if end <= IDENTITY_LIMIT.load(Ordering::Relaxed) {
        return Some(phys);
    }
    let first = phys & !(2 * MIB - 1);
    let size = ((end + 2 * MIB - 1) & !(2 * MIB - 1)) - first;
    let virt = MMIO_NEXT.fetch_add(size, Ordering::Relaxed);
    if virt.max(IDENTITY_LIMIT.load(Ordering::Relaxed)) != virt || virt + size > MMIO_WINDOW_END {
        return None; // window exhausted, or RAM reaches into it
    }
    unsafe {
        // The kernel's PDPT is shared by every process, so a mapping added
        // here is visible everywhere.
        let pml4 = kernel_address_space() as *const u64;
        let pdpt = (*pml4 & PTE_ADDR) as *mut u64;
        let mut offset = 0;
        while offset < size {
            let v = virt + offset;
            let pdpt_entry = pdpt.add((v / GIB) as usize);
            if *pdpt_entry & PTE_PRESENT == 0 {
                *pdpt_entry = try_alloc_zeroed_frame()? | PTE_PRESENT | PTE_WRITE;
            }
            let pd = (*pdpt_entry & PTE_ADDR) as *mut u64;
            *pd.add(((v / (2 * MIB)) % 512) as usize) = (first + offset) | PTE_PRESENT | PTE_WRITE | PTE_HUGE;
            offset += 2 * MIB;
        }
    }
    Some(virt + (phys - first))
}

// ---------------------------------------------------------------- user address spaces
//
// The kernel lives in the low 512 GiB (PML4 slot 0), mapped supervisor-only.
// Every process gets its own PML4 that shares slot 0 with the kernel and owns
// everything above it. User memory is mapped with 4 KiB pages.

const PTE_USER: u64 = 1 << 2;
const PTE_ADDR: u64 = 0x000F_FFFF_FFFF_F000;

/// Start of the address range processes may use (PML4 slot 2).
pub const USER_BASE: u64 = 0x100_0000_0000;
pub const USER_END: u64 = USER_BASE + (1 << 39);

static KERNEL_PML4: AtomicU64 = AtomicU64::new(0);
static VM_LOCK: IrqLock<()> = IrqLock::new(());

pub fn kernel_address_space() -> u64 {
    KERNEL_PML4.load(Ordering::Relaxed)
}

pub fn current_address_space() -> u64 {
    let cr3: u64;
    unsafe { asm!("mov {}, cr3", out(reg) cr3, options(nomem, nostack, preserves_flags)) };
    cr3 & PTE_ADDR
}

pub fn switch_address_space(pml4: u64) {
    if current_address_space() != pml4 {
        unsafe { asm!("mov cr3, {}", in(reg) pml4, options(nostack, preserves_flags)) };
    }
}

/// One zeroed 4 KiB page of physical memory, e.g. for a device driver's
/// control structures. Its address is usable as-is (identity mapped).
pub fn try_alloc_zeroed_frame() -> Option<u64> {
    let frame = alloc_frame()?;
    unsafe { core::ptr::write_bytes(frame as *mut u8, 0, PAGE as usize) };
    Some(frame)
}

pub fn new_address_space() -> Option<u64> {
    let pml4 = try_alloc_zeroed_frame()?;
    unsafe {
        let kernel = kernel_address_space() as *const u64;
        (pml4 as *mut u64).write(kernel.read());
    }
    Some(pml4)
}

fn indices(virt: u64) -> [usize; 4] {
    [
        ((virt >> 39) & 511) as usize,
        ((virt >> 30) & 511) as usize,
        ((virt >> 21) & 511) as usize,
        ((virt >> 12) & 511) as usize,
    ]
}

/// Map a fresh zeroed page at `virt` in the given address space and return
/// its physical address. If the page is already mapped, returns the existing
/// frame. None means out of memory or an address outside the user range.
pub fn map_user_page(pml4: u64, virt: u64, writable: bool) -> Option<u64> {
    if !(USER_BASE..USER_END).contains(&virt) {
        return None;
    }
    // Threads of one process share its page tables and may grow them at the
    // same moment on different CPUs.
    let _guard = VM_LOCK.lock();
    let idx = indices(virt);
    unsafe {
        let mut table = pml4 as *mut u64;
        for &i in &idx[..3] {
            let entry = table.add(i);
            if *entry & PTE_PRESENT == 0 {
                *entry = try_alloc_zeroed_frame()? | PTE_PRESENT | PTE_WRITE | PTE_USER;
            }
            table = (*entry & PTE_ADDR) as *mut u64;
        }
        let entry = table.add(idx[3]);
        if *entry & PTE_PRESENT != 0 {
            return Some(*entry & PTE_ADDR);
        }
        let frame = try_alloc_zeroed_frame()?;
        *entry = frame | PTE_PRESENT | PTE_USER | if writable { PTE_WRITE } else { 0 };
        Some(frame)
    }
}

/// Marks a page that belongs to a shared memory object rather than to the
/// process: tearing down the address space must not free it. (Bit 9 of a
/// page table entry is ignored by the CPU and free for the OS to use.)
const PTE_SHARED: u64 = 1 << 9;

/// Map an existing frame (owned by a shared memory object) at `virt`.
pub fn map_shared_page(pml4: u64, virt: u64, frame: u64) -> bool {
    if !(USER_BASE..USER_END).contains(&virt) {
        return false;
    }
    let _guard = VM_LOCK.lock();
    let idx = indices(virt);
    unsafe {
        let mut table = pml4 as *mut u64;
        for &i in &idx[..3] {
            let entry = table.add(i);
            if *entry & PTE_PRESENT == 0 {
                let Some(fresh) = try_alloc_zeroed_frame() else { return false };
                *entry = fresh | PTE_PRESENT | PTE_WRITE | PTE_USER;
            }
            table = (*entry & PTE_ADDR) as *mut u64;
        }
        let entry = table.add(idx[3]);
        if *entry & PTE_PRESENT != 0 {
            return false; // something is already mapped there
        }
        *entry = frame | PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_SHARED;
    }
    true
}

/// Remove a mapping made by `map_shared_page` (the frame is left alone: it
/// belongs to whoever shared it). Returns false if there was none.
///
/// Only this CPU's translation cache is flushed. That is enough for a
/// process that is not running on another CPU at this moment, which is
/// what the callers ensure; others reload their tables on switching to it.
pub fn unmap_shared_page(pml4: u64, virt: u64) -> bool {
    if !(USER_BASE..USER_END).contains(&virt) {
        return false;
    }
    let _guard = VM_LOCK.lock();
    let idx = indices(virt);
    unsafe {
        let mut table = pml4 as *mut u64;
        for &i in &idx[..3] {
            let entry = *table.add(i);
            if entry & PTE_PRESENT == 0 {
                return false;
            }
            table = (entry & PTE_ADDR) as *mut u64;
        }
        let entry = table.add(idx[3]);
        if *entry & PTE_PRESENT == 0 || *entry & PTE_SHARED == 0 {
            return false;
        }
        *entry = 0;
        asm!("invlpg [{}]", in(reg) virt, options(nostack, preserves_flags));
    }
    true
}

/// Physical address behind a user virtual address, if user mode may access it.
fn translate_user(pml4: u64, virt: u64, write: bool) -> Option<u64> {
    if !(USER_BASE..USER_END).contains(&virt) {
        return None;
    }
    let idx = indices(virt);
    unsafe {
        let mut table = pml4 as *const u64;
        for (level, &i) in idx.iter().enumerate() {
            let entry = *table.add(i);
            if entry & PTE_PRESENT == 0 || entry & PTE_USER == 0 {
                return None;
            }
            if level == 3 {
                if write && entry & PTE_WRITE == 0 {
                    return None;
                }
                return Some((entry & PTE_ADDR) | (virt & 0xFFF));
            }
            table = (entry & PTE_ADDR) as *const u64;
        }
    }
    None
}

/// True if user mode can read (or write) every byte of `addr..addr+len`.
/// System calls use this before touching memory a process handed them.
pub fn user_range_ok(pml4: u64, addr: u64, len: u64, write: bool) -> bool {
    let Some(end) = addr.checked_add(len) else { return false };
    let mut page = addr & !(PAGE - 1);
    while page < end {
        if translate_user(pml4, page, write).is_none() {
            return false;
        }
        page += PAGE;
    }
    true
}

unsafe fn free_table(table: u64, level: u32) {
    for i in 0..512 {
        let entry = *(table as *const u64).add(i);
        if entry & PTE_PRESENT != 0 {
            if level > 1 {
                free_table(entry & PTE_ADDR, level - 1);
            } else if entry & PTE_SHARED == 0 {
                free_frame(entry & PTE_ADDR);
            }
        }
    }
    free_frame(table);
}

/// Release every page and page table a process owned. Must not be called
/// while that address space is the active one.
pub fn free_address_space(pml4: u64) {
    unsafe {
        for i in 1..512 {
            let entry = *(pml4 as *const u64).add(i);
            if entry & PTE_PRESENT != 0 {
                free_table(entry & PTE_ADDR, 3);
            }
        }
    }
    free_frame(pml4);
}

// ---------------------------------------------------------------- heap

/// Power-of-two size-class allocator over one contiguous region. Freed blocks
/// go on a per-class intrusive free list; fresh blocks are bump-allocated.
struct Heap {
    start: usize,
    end: usize,
    next: usize,
    free: [usize; 48],
    used: usize,
}

static HEAP: IrqLock<Heap> = IrqLock::new(Heap { start: 0, end: 0, next: 0, free: [0; 48], used: 0 });

fn block_size(layout: Layout) -> usize {
    layout.size().max(layout.align()).max(16).next_power_of_two()
}

pub struct KernelAllocator;

unsafe impl GlobalAlloc for KernelAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let mut h = HEAP.lock();
        let size = block_size(layout);
        let class = size.trailing_zeros() as usize;
        if class >= h.free.len() {
            return null_mut();
        }
        let head = h.free[class];
        if head != 0 {
            h.free[class] = *(head as *const usize);
            h.used += size;
            return head as *mut u8;
        }
        let align = size.min(4096).max(layout.align());
        let ptr = (h.next + align - 1) & !(align - 1);
        if ptr + size > h.end {
            return null_mut();
        }
        h.next = ptr + size;
        h.used += size;
        ptr as *mut u8
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        let mut h = HEAP.lock();
        let size = block_size(layout);
        let class = size.trailing_zeros() as usize;
        *(ptr as *mut usize) = h.free[class];
        h.free[class] = ptr as usize;
        h.used -= size;
    }
}

// ---------------------------------------------------------------- init + stats

pub struct Stats {
    pub total_bytes: u64,
    pub used_bytes: u64,
    pub heap_size: usize,
    pub heap_used: usize,
    pub mapped_gib: u64,
}

static MAPPED_GIB: IrqLock<u64> = IrqLock::new(0);

pub fn init(boot: &BootInfo) {
    let mut ram_top = 0u64;
    {
        let mut f = FRAMES.lock();
        for d in boot.memory_map() {
            let start = d.start;
            let end = d.start + d.pages * PAGE;
            // Types 1..=10 are real RAM of one kind or another (loader, boot
            // services, runtime, conventional, ACPI); the rest is MMIO/reserved.
            if (1..=10).contains(&d.kind) && end > ram_top {
                ram_top = end;
            }
            if d.kind != MEM_CONVENTIONAL {
                continue;
            }
            // Note one free page in low memory for starting other CPUs.
            let low = start.max(0x8000);
            if LOW_PAGE.load(Ordering::Relaxed) == 0 && low + PAGE <= end.min(0x9F000) {
                LOW_PAGE.store(low, Ordering::Relaxed);
            }
            // Leave the first 2 MiB alone: it is unmapped once paging is ours.
            let start = start.max(2 * MIB);
            if end <= start || f.count == MAX_REGIONS {
                continue;
            }
            let i = f.count;
            f.regions[i] = Region { start, end };
            f.count += 1;
            f.total += (end - start) / PAGE;
        }
        f.next = f.regions[0].start;
        assert!(f.count > 0, "firmware reported no usable memory");
    }

    // Heap: a quarter of RAM, clamped to 4..64 MiB, shrinking if fragmented.
    let total_bytes = FRAMES.lock().total * PAGE;
    let mut heap_bytes = (total_bytes / 4).clamp(4 * MIB, 64 * MIB) & !(PAGE - 1);
    let heap_start = loop {
        if let Some(addr) = FRAMES.lock().alloc_contiguous(heap_bytes / PAGE) {
            break addr;
        }
        heap_bytes /= 2;
        assert!(heap_bytes >= MIB, "no contiguous memory for the kernel heap");
    };
    {
        let mut h = HEAP.lock();
        h.start = heap_start as usize;
        h.next = heap_start as usize;
        h.end = (heap_start + heap_bytes) as usize;
    }

    let fb_end = if boot.has_fb { boot.fb_base + boot.fb_size as u64 } else { 0 };
    let gibs = unsafe { init_paging(ram_top.max(fb_end)) };
    *MAPPED_GIB.lock() = gibs;
}

pub fn stats() -> Stats {
    let (total, used) = {
        let f = FRAMES.lock();
        (f.total * PAGE, f.used * PAGE)
    };
    let (heap_size, heap_used) = {
        let h = HEAP.lock();
        (h.end - h.start, h.used)
    };
    Stats { total_bytes: total, used_bytes: used, heap_size, heap_used, mapped_gib: *MAPPED_GIB.lock() }
}

/// Copy of the usable-RAM regions as they stand now, for `memmap`.
pub fn regions(out: &mut [Region; MAX_REGIONS]) -> usize {
    let f = FRAMES.lock();
    out[..f.count].copy_from_slice(&f.regions[..f.count]);
    f.count
}

pub const REGION_CAPACITY: usize = MAX_REGIONS;
