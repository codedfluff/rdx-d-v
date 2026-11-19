//! Minimal hand-rolled UEFI bindings: just enough to grab the framebuffer and
//! memory map, then leave boot services for good.

use core::ffi::c_void;
use core::ptr::{addr_of_mut, null_mut};

pub type Handle = *mut c_void;

#[repr(C)]
pub struct SystemTable {
    hdr: [u8; 24],
    firmware_vendor: *const u16,
    firmware_revision: u32,
    con_in_handle: Handle,
    con_in: *mut c_void,
    con_out_handle: Handle,
    con_out: *mut c_void,
    stderr_handle: Handle,
    stderr: *mut c_void,
    runtime_services: *mut c_void,
    boot_services: *mut c_void,
    config_entries: usize,
    config_table: *mut c_void,
}

#[repr(C)]
struct Guid(u32, u16, u16, [u8; 8]);

const GOP_GUID: Guid = Guid(0x9042a9de, 0x23dc, 0x4a38, [0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a]);

#[repr(C)]
struct Gop {
    query_mode: usize,
    set_mode: usize,
    blt: usize,
    mode: *const GopMode,
}

#[repr(C)]
struct GopMode {
    max_mode: u32,
    mode: u32,
    info: *const GopModeInfo,
    info_size: usize,
    fb_base: u64,
    fb_size: usize,
}

#[repr(C)]
struct GopModeInfo {
    version: u32,
    width: u32,
    height: u32,
    pixel_format: u32,
    pixel_masks: [u32; 4],
    pixels_per_scan_line: u32,
}

// GUIDs as they appear in memory (first three fields little-endian).
const ACPI2_GUID: [u8; 16] =
    [0x71, 0xE8, 0x68, 0x88, 0xF1, 0xE4, 0xD3, 0x11, 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81];
const ACPI1_GUID: [u8; 16] =
    [0x30, 0x2D, 0x9D, 0xEB, 0x88, 0x2D, 0xD3, 0x11, 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D];

const LOADED_IMAGE_GUID: Guid = Guid(0x5B1B31A1, 0x9562, 0x11D2, [0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B]);
const FILE_SYSTEM_GUID: Guid = Guid(0x964E5B22, 0x6459, 0x11D2, [0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B]);

#[repr(C)]
struct FileSystem {
    revision: u64,
    open_volume: extern "efiapi" fn(*mut FileSystem, *mut *mut File) -> usize,
}

#[repr(C)]
struct File {
    revision: u64,
    open: extern "efiapi" fn(*mut File, *mut *mut File, *const u16, u64, u64) -> usize,
    close: extern "efiapi" fn(*mut File) -> usize,
    delete: usize,
    read: extern "efiapi" fn(*mut File, *mut usize, *mut u8) -> usize,
    write: usize,
    get_position: usize,
    set_position: usize,
    get_info: usize,
    set_info: usize,
    flush: usize,
}

// Indices into the boot services function table (after the 24-byte header).
const BS_GET_MEMORY_MAP: usize = 4;
const BS_ALLOCATE_POOL: usize = 5;
const BS_HANDLE_PROTOCOL: usize = 16;
const BS_EXIT_BOOT_SERVICES: usize = 26;
const BS_LOCATE_PROTOCOL: usize = 37;

type GetMemoryMapFn = extern "efiapi" fn(*mut usize, *mut u8, *mut usize, *mut usize, *mut u32) -> usize;
type AllocatePoolFn = extern "efiapi" fn(u32, usize, *mut *mut u8) -> usize;
type HandleProtocolFn = extern "efiapi" fn(Handle, *const Guid, *mut *mut c_void) -> usize;
type ExitBootServicesFn = extern "efiapi" fn(Handle, usize) -> usize;
type LocateProtocolFn = extern "efiapi" fn(*const Guid, *mut c_void, *mut *mut c_void) -> usize;

pub const MAX_BOOT_FILES: usize = 512;

/// A file read from the boot disk before leaving boot services.
#[derive(Clone, Copy)]
pub struct BootFile {
    name: [u8; 48],
    name_len: usize,
    data: *const u8,
    size: usize,
}

impl BootFile {
    const EMPTY: BootFile = BootFile { name: [0; 48], name_len: 0, data: core::ptr::null(), size: 0 };

    pub fn name(&self) -> &[u8] {
        &self.name[..self.name_len]
    }

    pub fn data(&self) -> &[u8] {
        unsafe { core::slice::from_raw_parts(self.data, self.size) }
    }
}

/// Read every regular file in \bin on the volume we booted from into memory.
/// Failures are non-fatal: the kernel just boots with fewer (or no) programs.
unsafe fn load_boot_files(image: Handle, bs: *mut c_void, info: &mut BootInfo) {
    let handle_protocol: HandleProtocolFn = core::mem::transmute(bs_fn(bs, BS_HANDLE_PROTOCOL));
    let allocate_pool: AllocatePoolFn = core::mem::transmute(bs_fn(bs, BS_ALLOCATE_POOL));

    let mut loaded_image: *mut c_void = null_mut();
    if handle_protocol(image, &LOADED_IMAGE_GUID, &mut loaded_image) != 0 || loaded_image.is_null() {
        return;
    }
    // EFI_LOADED_IMAGE_PROTOCOL.DeviceHandle sits at offset 24.
    let device = *((loaded_image as *const u8).add(24) as *const Handle);
    let mut fs: *mut c_void = null_mut();
    if handle_protocol(device, &FILE_SYSTEM_GUID, &mut fs) != 0 || fs.is_null() {
        return;
    }
    let fs = fs as *mut FileSystem;
    let mut root: *mut File = null_mut();
    if ((*fs).open_volume)(fs, &mut root) != 0 || root.is_null() {
        return;
    }
    let dir_name: [u16; 5] = [b'\\' as u16, b'b' as u16, b'i' as u16, b'n' as u16, 0];
    let mut dir: *mut File = null_mut();
    if ((*root).open)(root, &mut dir, dir_name.as_ptr(), 1, 0) != 0 || dir.is_null() {
        return;
    }

    // Each read of a directory handle yields one EFI_FILE_INFO record:
    // FileSize at +8, Attribute at +72, NUL-terminated UTF-16 name at +80.
    let mut entry = [0u64; 64];
    let entry_ptr = entry.as_mut_ptr() as *mut u8;
    while info.file_count < MAX_BOOT_FILES {
        let mut len = core::mem::size_of_val(&entry);
        if ((*dir).read)(dir, &mut len, entry_ptr) != 0 || len == 0 {
            break;
        }
        let size = (entry_ptr.add(8) as *const u64).read() as usize;
        let attributes = (entry_ptr.add(72) as *const u64).read();
        let name16 = entry_ptr.add(80) as *const u16;
        if attributes & 0x10 != 0 || size == 0 {
            continue; // directory or empty file
        }

        let mut bf = BootFile::EMPTY;
        while bf.name_len < bf.name.len() {
            let c = name16.add(bf.name_len).read();
            if c == 0 {
                break;
            }
            bf.name[bf.name_len] = if c < 128 { c as u8 } else { b'?' };
            bf.name_len += 1;
        }

        let mut file: *mut File = null_mut();
        if ((*dir).open)(dir, &mut file, name16, 1, 0) != 0 || file.is_null() {
            continue;
        }
        // Pool type 2 = EfiLoaderData: stays reserved after ExitBootServices.
        let mut data: *mut u8 = null_mut();
        if allocate_pool(2, size, &mut data) == 0 && !data.is_null() {
            let mut read_len = size;
            if ((*file).read)(file, &mut read_len, data) == 0 {
                bf.data = data;
                bf.size = read_len;
                info.files[info.file_count] = bf;
                info.file_count += 1;
            }
        }
        ((*file).close)(file);
    }
    if info.file_count == MAX_BOOT_FILES {
        // Full: is there anything left that did not fit?
        let mut len = core::mem::size_of_val(&entry);
        info.files_skipped = ((*dir).read)(dir, &mut len, entry_ptr) == 0 && len != 0;
    }
    ((*dir).close)(dir);
    ((*root).close)(root);
}

unsafe fn bs_fn(bs: *mut c_void, index: usize) -> usize {
    *((bs as *const u8).add(24 + index * 8) as *const usize)
}

pub const MEM_CONVENTIONAL: u32 = 7;

#[derive(Clone, Copy)]
pub struct MemDescriptor {
    pub kind: u32,
    pub start: u64,
    pub pages: u64,
}

#[derive(Clone, Copy)]
pub struct BootInfo {
    pub has_fb: bool,
    pub fb_base: u64,
    pub fb_size: usize,
    pub width: usize,
    pub height: usize,
    pub stride: usize,
    pub bgr: bool,
    mmap: *const u8,
    mmap_len: usize,
    desc_size: usize,
    files: [BootFile; MAX_BOOT_FILES],
    file_count: usize,
    /// \bin held more files than fit (MAX_BOOT_FILES): the rest were not loaded.
    pub files_skipped: bool,
    /// Physical address of the ACPI root pointer, or 0 if the firmware has none.
    pub rsdp: u64,
}

impl BootInfo {
    pub fn files(&self) -> &[BootFile] {
        &self.files[..self.file_count]
    }

    pub fn memory_map(&self) -> impl Iterator<Item = MemDescriptor> + '_ {
        let count = if self.desc_size == 0 { 0 } else { self.mmap_len / self.desc_size };
        (0..count).map(move |i| unsafe {
            let p = self.mmap.add(i * self.desc_size);
            MemDescriptor {
                kind: (p as *const u32).read_unaligned(),
                start: (p.add(8) as *const u64).read_unaligned(),
                pages: (p.add(24) as *const u64).read_unaligned(),
            }
        })
    }
}

#[repr(C, align(16))]
struct MmapBuf([u8; 64 * 1024]);
static mut MMAP_BUF: MmapBuf = MmapBuf([0; 64 * 1024]);

/// Collect what the kernel needs from the firmware and call ExitBootServices.
/// After this returns the firmware is gone and the machine is ours.
pub unsafe fn exit_to_kernel(image: Handle, st: *const SystemTable) -> BootInfo {
    let bs = (*st).boot_services;

    let mut info = BootInfo {
        has_fb: false,
        fb_base: 0,
        fb_size: 0,
        width: 0,
        height: 0,
        stride: 0,
        bgr: true,
        mmap: null_mut(),
        mmap_len: 0,
        desc_size: 0,
        files: [BootFile::EMPTY; MAX_BOOT_FILES],
        file_count: 0,
        files_skipped: false,
        rsdp: 0,
    };

    load_boot_files(image, bs, &mut info);

    // ACPI root pointer, from the firmware's configuration table: an array
    // of (GUID, pointer) pairs. Prefer the ACPI 2.0 entry over the 1.0 one.
    let entries = (*st).config_table as *const u8;
    if !entries.is_null() {
        for i in 0..(*st).config_entries {
            let entry = entries.add(i * 24);
            let guid = core::slice::from_raw_parts(entry, 16);
            let pointer = (entry.add(16) as *const u64).read_unaligned();
            if guid == ACPI2_GUID {
                info.rsdp = pointer;
                break;
            }
            if guid == ACPI1_GUID {
                info.rsdp = pointer;
            }
        }
    }

    // Framebuffer via the Graphics Output Protocol.
    let locate: LocateProtocolFn = core::mem::transmute(bs_fn(bs, BS_LOCATE_PROTOCOL));
    let mut gop: *mut c_void = null_mut();
    if locate(&GOP_GUID, null_mut(), &mut gop) == 0 && !gop.is_null() {
        let mode = (*(gop as *const Gop)).mode;
        if !mode.is_null() && !(*mode).info.is_null() {
            let mi = &*(*mode).info;
            // 0 = RGBX, 1 = BGRX. Anything else has no simple linear framebuffer.
            if mi.pixel_format <= 1 && (*mode).fb_base != 0 {
                info.has_fb = true;
                info.fb_base = (*mode).fb_base;
                info.fb_size = (*mode).fb_size;
                info.width = mi.width as usize;
                info.height = mi.height as usize;
                info.stride = mi.pixels_per_scan_line as usize;
                info.bgr = mi.pixel_format == 1;
            }
        }
    }

    // Memory map + exit. The map key goes stale if the firmware allocates in
    // between, so retry a few times.
    let get_map: GetMemoryMapFn = core::mem::transmute(bs_fn(bs, BS_GET_MEMORY_MAP));
    let exit: ExitBootServicesFn = core::mem::transmute(bs_fn(bs, BS_EXIT_BOOT_SERVICES));
    let buf = addr_of_mut!(MMAP_BUF) as *mut u8;
    for _ in 0..8 {
        let mut size = core::mem::size_of::<MmapBuf>();
        let mut key = 0usize;
        let mut desc_size = 0usize;
        let mut desc_ver = 0u32;
        if get_map(&mut size, buf, &mut key, &mut desc_size, &mut desc_ver) != 0 {
            continue;
        }
        if exit(image, key) == 0 {
            info.mmap = buf;
            info.mmap_len = size;
            info.desc_size = desc_size;
            return info;
        }
    }
    crate::serial::write_str("FATAL: ExitBootServices failed\n");
    loop {
        core::arch::asm!("hlt");
    }
}
