//! The screen and raw input, for one graphical program at a time.
//!
//! Normally the kernel's text console owns the framebuffer and every key
//! goes to the console's input queue. A program (the window server) can
//! take the display over with the display_open system call. From then on:
//!
//!   * the framebuffer is mapped into that program, which draws on it
//!     directly, and the console stops drawing (it keeps track of what it
//!     would have shown, and puts it back afterwards);
//!   * keys and mouse movement go to that program as events, read from the
//!     descriptor display_open returned. Ctrl+C is just a key: the program
//!     decides what it means.
//!
//! Closing the descriptor - which happens by itself when the program exits
//! or crashes - hands everything back to the console. So a broken
//! graphical program can never leave the machine with a dead screen.
//!
//! Each event is 16 bytes: kind (u32: 1 key, 2 mouse), code (u32: the key's
//! byte, or the mouse buttons held: bit 0 left, 1 right, 2 middle), then
//! four i16: movement since the last event (dx, dy) and the pointer's
//! position on the screen (x, y), which the kernel keeps within the screen.

use alloc::collections::VecDeque;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use crate::memory::{self, PAGE, USER_BASE};
use crate::sched::{self, Wait};
use crate::sync::IrqLock;
use crate::uefi::BootInfo;
use crate::console;

/// Where the framebuffer appears in the owning program.
pub const MAP_BASE: u64 = USER_BASE + 0x1_0000_0000;

pub const EVENT_KEY: u32 = 1;
pub const EVENT_MOUSE: u32 = 2;
const MAX_EVENTS: usize = 512;

#[derive(Clone, Copy)]
struct Mode {
    fb: u64,
    width: usize,
    height: usize,
    stride: usize,
    bgr: bool,
}

struct State {
    mode: Option<Mode>,
    events: VecDeque<[u8; 16]>,
    x: i32,
    y: i32,
    buttons: u32,
}

static STATE: IrqLock<State> = IrqLock::new(State { mode: None, events: VecDeque::new(), x: 0, y: 0, buttons: 0 });
static OWNED: AtomicBool = AtomicBool::new(false);
static OWNER: AtomicU32 = AtomicU32::new(0);
/// Has any mouse ever reported? (For `/proc` and the mouse program.)
static MOUSE_SEEN: AtomicBool = AtomicBool::new(false);

pub fn init(boot: &BootInfo) {
    if !boot.has_fb {
        return;
    }
    let mut s = STATE.lock();
    s.mode = Some(Mode { fb: boot.fb_base, width: boot.width, height: boot.height, stride: boot.stride, bgr: boot.bgr });
    s.x = boot.width as i32 / 2;
    s.y = boot.height as i32 / 2;
}

/// Does a graphical program have the screen and the input?
pub fn owned() -> bool {
    OWNED.load(Ordering::Relaxed)
}

/// Which process has the screen (0: the console).
pub fn owner() -> u32 {
    if owned() { OWNER.load(Ordering::Relaxed) } else { 0 }
}

pub fn mouse_seen() -> bool {
    MOUSE_SEEN.load(Ordering::Relaxed)
}

/// What display_open tells the program (32 bytes): framebuffer address in
/// its memory, width, height, stride (pixels per row), pixel format (0:
/// blue, green, red, unused - 1: red, green, blue, unused), and where the
/// pointer is.
pub struct Info {
    pub address: u64,
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub format: u32,
    pub x: i32,
    pub y: i32,
}

/// Give the screen to process `pid`, mapping the framebuffer into its
/// address space `space`.
pub fn acquire(pid: u32, space: u64) -> Result<Info, &'static str> {
    let mode = STATE.lock().mode.ok_or("this machine has no framebuffer (started with no display?)")?;
    if OWNED.swap(true, Ordering::SeqCst) {
        return Err("another program already has the display");
    }
    let bytes = (mode.stride * mode.height * 4) as u64;
    let pages = bytes.div_ceil(PAGE);
    for i in 0..pages {
        let frame = (mode.fb & !(PAGE - 1)) + i * PAGE;
        if !memory::map_shared_page(space, MAP_BASE + i * PAGE, frame) {
            // Already mapped from an earlier time this program had the
            // display: that mapping is the same framebuffer, so it is fine.
            if i == 0 && memory::user_range_ok(space, MAP_BASE, 1, true) {
                break;
            }
            OWNED.store(false, Ordering::SeqCst);
            return Err("could not map the framebuffer");
        }
    }
    OWNER.store(pid, Ordering::SeqCst);
    let (x, y) = {
        let mut s = STATE.lock();
        s.events.clear();
        (s.x, s.y)
    };
    console::suspend(true);
    Ok(Info {
        address: MAP_BASE + (mode.fb & (PAGE - 1)),
        width: mode.width as u32,
        height: mode.height as u32,
        stride: mode.stride as u32,
        format: if mode.bgr { 0 } else { 1 },
        x,
        y,
    })
}

/// Hand the screen back to the console.
pub fn release() {
    if OWNED.swap(false, Ordering::SeqCst) {
        OWNER.store(0, Ordering::SeqCst);
        STATE.lock().events.clear();
        console::suspend(false);
    }
}

fn push(s: &mut State, kind: u32, code: u32, dx: i32, dy: i32) {
    let mut event = [0u8; 16];
    event[0..4].copy_from_slice(&kind.to_le_bytes());
    event[4..8].copy_from_slice(&code.to_le_bytes());
    let clamp = |v: i32| v.clamp(i16::MIN as i32, i16::MAX as i32) as i16;
    event[8..10].copy_from_slice(&clamp(dx).to_le_bytes());
    event[10..12].copy_from_slice(&clamp(dy).to_le_bytes());
    event[12..14].copy_from_slice(&clamp(s.x).to_le_bytes());
    event[14..16].copy_from_slice(&clamp(s.y).to_le_bytes());
    if s.events.len() >= MAX_EVENTS {
        s.events.pop_front(); // nobody is reading: forget the oldest
    }
    s.events.push_back(event);
}

/// A key, while a graphical program has the input. Returns false if the
/// console has it, in which case the caller queues it as usual.
pub fn key(b: u8) -> bool {
    if !owned() {
        return false;
    }
    push(&mut STATE.lock(), EVENT_KEY, b as u32, 0, 0);
    sched::wake(Wait::Input);
    true
}

/// The mouse moved (dx, dy in pixels, y growing downwards) and/or its
/// buttons changed. From the PS/2 and USB mouse drivers.
pub fn mouse(dx: i32, dy: i32, buttons: u32) {
    MOUSE_SEEN.store(true, Ordering::Relaxed);
    let wake = {
        let mut s = STATE.lock();
        let Some(mode) = s.mode else { return };
        s.x = (s.x + dx).clamp(0, mode.width as i32 - 1);
        s.y = (s.y + dy).clamp(0, mode.height as i32 - 1);
        let changed = buttons != s.buttons;
        s.buttons = buttons;
        if !owned() {
            return;
        }
        // Several moves in a row with nothing else in between become one,
        // so a program that is busy for a moment gets one jump, not a pile.
        let (x, y) = (s.x, s.y);
        if let Some(last) = s.events.back_mut() {
            let kind = u32::from_le_bytes([last[0], last[1], last[2], last[3]]);
            let code = u32::from_le_bytes([last[4], last[5], last[6], last[7]]);
            if !changed && kind == EVENT_MOUSE && code == buttons {
                let old_dx = i16::from_le_bytes([last[8], last[9]]) as i32;
                let old_dy = i16::from_le_bytes([last[10], last[11]]) as i32;
                let clamp = |v: i32| v.clamp(i16::MIN as i32, i16::MAX as i32) as i16;
                last[8..10].copy_from_slice(&clamp(old_dx + dx).to_le_bytes());
                last[10..12].copy_from_slice(&clamp(old_dy + dy).to_le_bytes());
                last[12..14].copy_from_slice(&clamp(x).to_le_bytes());
                last[14..16].copy_from_slice(&clamp(y).to_le_bytes());
                return;
            }
        }
        push(&mut s, EVENT_MOUSE, buttons, dx, dy);
        true
    };
    if wake {
        sched::wake(Wait::Input);
    }
}

/// A pointing device that reports where it is rather than how far it
/// moved (a tablet, or QEMU's virtual one): x and y are 0..=range across
/// the whole screen.
pub fn mouse_absolute(x: u32, y: u32, range: u32, buttons: u32) {
    let (dx, dy) = {
        let s = STATE.lock();
        let Some(mode) = s.mode else { return };
        let tx = (x as u64 * mode.width as u64 / (range as u64 + 1)) as i32;
        let ty = (y as u64 * mode.height as u64 / (range as u64 + 1)) as i32;
        (tx - s.x, ty - s.y)
    };
    mouse(dx, dy, buttons);
}

/// Take waiting events, as many whole ones as fit.
pub fn read(out: &mut [u8]) -> usize {
    let mut s = STATE.lock();
    let mut n = 0;
    while n + 16 <= out.len() {
        let Some(event) = s.events.pop_front() else { break };
        out[n..n + 16].copy_from_slice(&event);
        n += 16;
    }
    n
}

pub fn has_events() -> bool {
    !STATE.lock().events.is_empty()
}

/// Kept in a descriptor: dropping the last copy gives the display back.
pub struct Handle;

impl Drop for Handle {
    fn drop(&mut self) {
        release();
    }
}

/// A line for /proc: who has the screen, and where the mouse is.
pub fn status() -> alloc::string::String {
    let s = STATE.lock();
    let screen = match s.mode {
        Some(m) => alloc::format!("{}x{} framebuffer", m.width, m.height),
        None => alloc::string::String::from("no framebuffer"),
    };
    let user = match owner() {
        0 => alloc::string::String::from("the text console"),
        pid => alloc::format!("process {}", pid),
    };
    alloc::format!(
        "screen:  {}, in use by {}\nmouse:   {} (pointer at {},{}, buttons {:03b})\n",
        screen,
        user,
        if mouse_seen() {
            "working"
        } else if crate::mouse::present() {
            "PS/2 mouse found, not moved yet"
        } else {
            "not heard from yet"
        },
        s.x,
        s.y,
        s.buttons
    )
}
