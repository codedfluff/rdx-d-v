//! Program loader. RDX executables are 64-bit PE files — the same container
//! format as a Windows .exe — linked at a fixed address with no imports.
//! They are not Windows programs: they talk to the kernel through RDX system
//! calls, not the Windows API.

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

use crate::memory::{self, PAGE, USER_BASE};
use crate::pipe::FdTable;
use crate::{fs, sched};

/// Where programs must be linked (lld-link /base).
pub const IMAGE_BASE: u64 = USER_BASE + 0x40_0000;
const IMAGE_MAX: u64 = 64 * 1024 * 1024;
pub const HEAP_BASE: u64 = USER_BASE + 0x4000_0000;
pub const HEAP_MAX: u64 = 500 * 1024 * 1024; // address space (up to the threads' stacks); pages are mapped as the heap grows
pub const STACK_TOP: u64 = USER_BASE + 0x8000_0000;
const STACK_PAGES: u64 = 256; // 1 MiB, mapped at the start
/// How far the main stack may grow: pages below the first 1 MiB are mapped
/// when first touched (see arch::handle_exception), so deep recursion works
/// without every program paying for a big stack.
pub const STACK_MAX: u64 = 8 * 1024 * 1024;
const ARG_AREA: u64 = 256;

fn u16_at(data: &[u8], off: usize) -> Result<u16, &'static str> {
    let b = data.get(off..off + 2).ok_or("truncated executable")?;
    Ok(u16::from_le_bytes([b[0], b[1]]))
}

fn u32_at(data: &[u8], off: usize) -> Result<u32, &'static str> {
    let b = data.get(off..off + 4).ok_or("truncated executable")?;
    Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

fn u64_at(data: &[u8], off: usize) -> Result<u64, &'static str> {
    Ok(u32_at(data, off)? as u64 | (u32_at(data, off + 4)? as u64) << 32)
}

/// The search path used when the environment has no PATH: the root
/// directory, where the shipped programs live.
pub const DEFAULT_PATH: &str = "/";

/// Environment variables as (name, value) pairs.
pub type Env = Vec<(String, String)>;

/// The value of a variable in an environment.
pub fn env_get<'a>(env: &'a Env, name: &str) -> Option<&'a str> {
    env.iter().find(|(n, _)| n == name).map(|(_, value)| value.as_str())
}

/// Find the program a command name refers to: a file of exactly that name
/// (or that name with ".exe" added), looked for first relative to `cwd` and
/// then in each directory of `path` (a colon-separated list, like the PATH
/// variable it comes from). A name containing a slash is a path in its own
/// right and is not searched for. Returns the file's key and its contents.
pub fn resolve(name: &str, cwd: &str, path: &str) -> Option<(String, Vec<u8>)> {
    let mut bases: Vec<String> = alloc::vec![String::from(cwd)];
    if !name.contains('/') {
        for dir in path.split(':').filter(|d| !d.is_empty()) {
            let key = fs::resolve("", dir);
            if !bases.contains(&key) {
                bases.push(key);
            }
        }
    }
    for base in &bases {
        for candidate in [fs::resolve(base, name), fs::resolve(base, &format!("{}.exe", name))] {
            if let Some(image) = fs::read(&candidate) {
                return Some((candidate, image));
            }
        }
    }
    None
}

/// Load `image` into a fresh address space and start it as a process with
/// the given open files, working directory and environment. `cmdline` is
/// handed to the program's entry point.
pub fn spawn(
    name: &str,
    image: &[u8],
    cmdline: &str,
    fds: FdTable,
    cwd: String,
    env: Env,
) -> Result<u32, &'static str> {
    let space = memory::new_address_space().ok_or("out of memory")?;
    match load(space, image, cmdline) {
        Ok((entry, stack)) => {
            Ok(sched::spawn_process(name, space, entry, stack, HEAP_BASE, STACK_TOP - ARG_AREA, fds, cwd, env))
        }
        Err(e) => {
            memory::free_address_space(space);
            Err(e)
        }
    }
}

fn load(space: u64, image: &[u8], cmdline: &str) -> Result<(u64, u64), &'static str> {
    // DOS header -> PE signature -> COFF header -> optional header.
    if image.get(0..2) != Some(b"MZ") {
        return Err("not an executable (no MZ header)");
    }
    let pe = u32_at(image, 0x3C)? as usize;
    if image.get(pe..pe + 4) != Some(b"PE\0\0") {
        return Err("not an executable (no PE signature)");
    }
    if u16_at(image, pe + 4)? != 0x8664 {
        return Err("executable is not x86-64");
    }
    let section_count = u16_at(image, pe + 6)? as usize;
    let opt_size = u16_at(image, pe + 20)? as usize;
    let opt = pe + 24;
    if u16_at(image, opt)? != 0x20B {
        return Err("executable is not PE32+");
    }
    let entry_rva = u32_at(image, opt + 16)? as u64;
    let base = u64_at(image, opt + 24)?;
    let image_size = u32_at(image, opt + 56)? as u64;
    if base != IMAGE_BASE {
        return Err("executable is linked at the wrong base address (rebuild it with build.cmd)");
    }
    if image_size > IMAGE_MAX || entry_rva >= image_size {
        return Err("executable is malformed or too large");
    }
    // Data directory 1 is the import table: we have no DLLs to offer.
    if u32_at(image, opt + 108)? > 1 && u32_at(image, opt + 112 + 8 + 4)? != 0 {
        return Err("executable imports DLLs (Windows programs cannot run on RDX)");
    }

    // Map each section and copy its bytes in through the physical frames.
    let sections = opt + opt_size;
    for i in 0..section_count {
        let s = sections + i * 40;
        let virt_size = u32_at(image, s + 8)? as u64;
        let rva = u32_at(image, s + 12)? as u64;
        let raw_size = u32_at(image, s + 16)? as u64;
        let raw_off = u32_at(image, s + 20)? as u64;
        let writable = u32_at(image, s + 36)? & 0x8000_0000 != 0;

        let mem_size = virt_size.max(raw_size);
        if rva % PAGE != 0 || rva + mem_size > image_size {
            return Err("executable has a malformed section");
        }
        let file_size = raw_size.min(mem_size);
        let file = image
            .get(raw_off as usize..(raw_off + file_size) as usize)
            .ok_or("executable section runs past the end of the file")?;

        let mut off = 0;
        while off < mem_size {
            let frame = memory::map_user_page(space, base + rva + off, writable).ok_or("out of memory")?;
            if off < file_size {
                let n = (file_size - off).min(PAGE) as usize;
                unsafe { core::ptr::copy_nonoverlapping(file.as_ptr().add(off as usize), frame as *mut u8, n) };
            }
            off += PAGE;
        }
    }

    // One read-only page of kernel-supplied code: where signal handlers
    // return to. It makes the `sigreturn` system call (number 44).
    let page = memory::map_user_page(space, sched::SIGNAL_RETURN, false).ok_or("out of memory")?;
    const SIGRETURN: [u8; 9] = [0xB8, 44, 0, 0, 0, 0xCD, 0x80, 0x0F, 0x0B]; // mov eax, 44; int 0x80; ud2
    unsafe { core::ptr::copy_nonoverlapping(SIGRETURN.as_ptr(), page as *mut u8, SIGRETURN.len()) };

    // Stack, with the command line parked in its top 256 bytes.
    let mut top_frame = 0;
    for i in 0..STACK_PAGES {
        top_frame = memory::map_user_page(space, STACK_TOP - (STACK_PAGES - i) * PAGE, true).ok_or("out of memory")?;
    }
    let args = cmdline.as_bytes();
    let n = args.len().min(ARG_AREA as usize - 1);
    unsafe {
        // The frame was zeroed, so the string is already NUL-terminated.
        core::ptr::copy_nonoverlapping(args.as_ptr(), (top_frame + PAGE - ARG_AREA) as *mut u8, n);
    }

    // The entry point is entered like a called function: 16-byte aligned
    // stack minus a return slot, with room above for its shadow space.
    let stack = STACK_TOP - ARG_AREA - 64 - 8;
    Ok((base + entry_rva, stack))
}
