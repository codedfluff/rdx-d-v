//! Framebuffer text console, mirrored to the serial port.
//!
//! The console keeps a grid of character cells so scrolling only has to
//! redraw cells whose contents actually changed, and never reads back from
//! video memory (which is painfully slow on real hardware).

use core::fmt::{self, Write};

use crate::serial;
use crate::sync::IrqLock;
use crate::uefi::BootInfo;

static FONT: &[u8; 96 * 16] = include_bytes!("font.bin");
const GLYPH_W: usize = 8;
const GLYPH_H: usize = 16;

const MAX_COLS: usize = 256;
const MAX_ROWS: usize = 96;

pub const BG: u32 = 0x0B0E14;
pub const FG: u32 = 0xD0D4DC;
pub const RED: u32 = 0xFF5C57;
pub const GREEN: u32 = 0x5AF78E;
pub const YELLOW: u32 = 0xF3F99D;
pub const BLUE: u32 = 0x57C7FF;
pub const MAGENTA: u32 = 0xFF6AC1;
pub const CYAN: u32 = 0x9AEDFE;
pub const DIM: u32 = 0x6C7380;

#[derive(Clone, Copy, PartialEq)]
struct Cell {
    ch: u8,
    fg: u32,
}

const BLANK: Cell = Cell { ch: b' ', fg: FG };

pub struct Console {
    ready: bool,
    /// A graphical program has the screen: keep track of the text, but
    /// draw nothing until it is handed back.
    suspended: bool,
    mirror: bool,
    fb: *mut u32,
    stride: usize,
    bgr: bool,
    scale: usize,
    cols: usize,
    rows: usize,
    origin_x: usize,
    origin_y: usize,
    width: usize,
    height: usize,
    cx: usize,
    cy: usize,
    fg: u32,
    /// Escape sequence being received: 0 none, 1 after ESC, 2 inside
    /// "ESC [" collecting numbers, 3 inside "ESC ]" (skipped), 4 at its end.
    esc: u8,
    params: [u16; 8],
    nparams: usize,
    cells: [Cell; MAX_COLS * MAX_ROWS],
}

unsafe impl Send for Console {}

static CONSOLE: IrqLock<Console> = IrqLock::new(Console {
    ready: false,
    suspended: false,
    mirror: true,
    fb: core::ptr::null_mut(),
    stride: 0,
    bgr: true,
    scale: 1,
    cols: 0,
    rows: 0,
    origin_x: 0,
    origin_y: 0,
    width: 0,
    height: 0,
    cx: 0,
    cy: 0,
    fg: FG,
    esc: 0,
    params: [0; 8],
    nparams: 0,
    cells: [BLANK; MAX_COLS * MAX_ROWS],
});

/// The 16 standard terminal colours, for "ESC [ 30..37 m" and "90..97 m".
const ANSI_COLORS: [u32; 16] = [
    0x2E3440, RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN, FG, DIM, 0xFF8A85, 0x8CFFB0, 0xFAFFC8, 0x8FDBFF, 0xFF9CD8,
    0xC6F7FF, 0xFFFFFF,
];

impl Console {
    fn native(&self, rgb: u32) -> u32 {
        if self.bgr {
            rgb
        } else {
            ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF)
        }
    }

    fn draw_cell(&mut self, col: usize, row: usize) {
        if self.suspended {
            return;
        }
        let cell = self.cells[row * MAX_COLS + col];
        let ch = if (32..128).contains(&cell.ch) { cell.ch } else { b'?' };
        let glyph = &FONT[(ch - 32) as usize * GLYPH_H..][..GLYPH_H];
        let fg = self.native(cell.fg);
        let bg = self.native(BG);
        let s = self.scale;
        let x0 = self.origin_x + col * GLYPH_W * s;
        let y0 = self.origin_y + row * GLYPH_H * s;
        for gy in 0..GLYPH_H {
            let bits = glyph[gy];
            for sy in 0..s {
                let line = unsafe { self.fb.add((y0 + gy * s + sy) * self.stride + x0) };
                for gx in 0..GLYPH_W {
                    let color = if bits & (0x80 >> gx) != 0 { fg } else { bg };
                    for sx in 0..s {
                        unsafe { line.add(gx * s + sx).write_volatile(color) };
                    }
                }
            }
        }
    }

    fn draw_cursor(&mut self) {
        if self.suspended {
            return;
        }
        let s = self.scale;
        let x0 = self.origin_x + self.cx * GLYPH_W * s;
        let y0 = self.origin_y + self.cy * GLYPH_H * s + (GLYPH_H - 2) * s;
        let color = self.native(self.fg);
        for y in 0..2 * s {
            for x in 0..GLYPH_W * s {
                unsafe { self.fb.add((y0 + y) * self.stride + x0 + x).write_volatile(color) };
            }
        }
    }

    /// Paint the whole screen from the cells: after a graphical program
    /// hands the screen back.
    fn redraw(&mut self) {
        let bg = self.native(BG);
        for y in 0..self.height {
            for x in 0..self.width {
                unsafe { self.fb.add(y * self.stride + x).write_volatile(bg) };
            }
        }
        for row in 0..self.rows {
            for col in 0..self.cols {
                if self.cells[row * MAX_COLS + col] != BLANK {
                    self.draw_cell(col, row);
                }
            }
        }
        if self.cx < self.cols {
            self.draw_cursor();
        }
    }

    fn clear(&mut self) {
        let bg = self.native(BG);
        for y in 0..if self.suspended { 0 } else { self.height } {
            for x in 0..self.width {
                unsafe { self.fb.add(y * self.stride + x).write_volatile(bg) };
            }
        }
        self.cells.fill(BLANK);
        self.cx = 0;
        self.cy = 0;
    }

    fn scroll(&mut self) {
        for row in 0..self.rows {
            for col in 0..self.cols {
                let new = if row + 1 < self.rows { self.cells[(row + 1) * MAX_COLS + col] } else { BLANK };
                let idx = row * MAX_COLS + col;
                if self.cells[idx] != new {
                    self.cells[idx] = new;
                    self.draw_cell(col, row);
                }
            }
        }
    }

    fn newline(&mut self) {
        self.cx = 0;
        if self.cy + 1 >= self.rows {
            self.scroll();
        } else {
            self.cy += 1;
        }
    }

    /// Carry out "ESC [ params final": the ANSI escape sequences that
    /// programs running in a pseudo-terminal send, and that any program may
    /// print. Covered: cursor movement (A B C D H), clearing (J K) and
    /// colours (m: 0, 30-37, 39, 90-97 and 38;2;r;g;b).
    fn csi(&mut self, last: u8) {
        let p = |i: usize| if i < self.nparams { self.params[i] as usize } else { 0 };
        let count = p(0).max(1);
        match last {
            b'A' => self.cy = self.cy.saturating_sub(count),
            b'B' => self.cy = (self.cy + count).min(self.rows - 1),
            b'C' => self.cx = (self.cx + count).min(self.cols - 1),
            b'D' => {
                for _ in 0..count {
                    if self.cx > 0 {
                        self.cx -= 1;
                    } else if self.cy > 0 {
                        self.cy -= 1;
                        self.cx = self.cols - 1;
                    }
                }
            }
            b'H' | b'f' => {
                self.cy = (p(0).max(1) - 1).min(self.rows - 1);
                self.cx = (p(1).max(1) - 1).min(self.cols - 1);
            }
            b'J' => {
                if p(0) == 2 {
                    self.clear();
                }
            }
            b'K' => {
                let row = self.cy;
                for col in self.cx..self.cols {
                    if self.cells[row * MAX_COLS + col] != BLANK {
                        self.cells[row * MAX_COLS + col] = BLANK;
                        self.draw_cell(col, row);
                    }
                }
            }
            b'm' => {
                let mut i = 0;
                if self.nparams == 0 {
                    self.fg = FG;
                }
                while i < self.nparams {
                    match p(i) {
                        0 | 39 => self.fg = FG,
                        n @ 30..=37 => self.fg = ANSI_COLORS[n - 30],
                        n @ 90..=97 => self.fg = ANSI_COLORS[n - 90 + 8],
                        38 if p(i + 1) == 2 => {
                            self.fg = ((p(i + 2) as u32 & 0xFF) << 16) | ((p(i + 3) as u32 & 0xFF) << 8) | (p(i + 4) as u32 & 0xFF);
                            i += 4;
                        }
                        _ => {}
                    }
                    i += 1;
                }
            }
            _ => {} // anything else is quietly ignored
        }
    }

    fn put(&mut self, b: u8) {
        match self.esc {
            1 => {
                self.esc = match b {
                    b'[' => 2,
                    b']' => 3, // "operating system command", e.g. a window title
                    _ => 0,
                };
                self.params = [0; 8];
                self.nparams = 0;
                return;
            }
            2 => {
                match b {
                    b'0'..=b'9' => {
                        if self.nparams == 0 {
                            self.nparams = 1;
                        }
                        let i = self.nparams - 1;
                        if i < 8 {
                            self.params[i] = self.params[i].saturating_mul(10).saturating_add((b - b'0') as u16);
                        }
                    }
                    b';' => self.nparams = (self.nparams.max(1) + 1).min(8),
                    0x20..=0x3F => {} // private markers such as '?'
                    _ => {
                        self.esc = 0;
                        self.csi(b);
                    }
                }
                return;
            }
            // A window title or the like: the console has no use for it.
            // It ends with BEL, or with ESC \.
            3 => {
                if b == 0x07 {
                    self.esc = 0;
                } else if b == 0x1B {
                    self.esc = 4;
                }
                return;
            }
            4 => {
                self.esc = 0;
                return;
            }
            _ => {}
        }
        if b == 0x1B {
            self.esc = 1;
            return;
        }
        match b {
            b'\n' => self.newline(),
            b'\r' => self.cx = 0,
            0x08 => {
                if self.cx > 0 {
                    self.cx -= 1;
                    self.cells[self.cy * MAX_COLS + self.cx] = BLANK;
                    self.draw_cell(self.cx, self.cy);
                }
            }
            b'\t' => {
                for _ in 0..(4 - self.cx % 4) {
                    self.put(b' ');
                }
            }
            _ => {
                if self.cx >= self.cols {
                    self.newline();
                }
                // Skip the (slow) pixel work when the cell already shows
                // this character: full-screen programs redraw a lot.
                let cell = Cell { ch: b, fg: self.fg };
                let idx = self.cy * MAX_COLS + self.cx;
                if self.cells[idx] != cell {
                    self.cells[idx] = cell;
                    self.draw_cell(self.cx, self.cy);
                }
                self.cx += 1;
            }
        }
    }

    fn hide_cursor(&mut self) {
        if self.cx < self.cols {
            self.draw_cell(self.cx, self.cy);
        }
    }

    fn show_cursor(&mut self) {
        if self.cx < self.cols {
            self.draw_cursor();
        }
    }
}

impl Write for Console {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        if self.mirror {
            serial::write_str(s);
        }
        if !self.ready {
            return Ok(());
        }
        // Erase the cursor, draw the text, put the cursor back.
        if self.cx < self.cols {
            self.draw_cell(self.cx, self.cy);
        }
        for b in s.bytes() {
            self.put(b);
        }
        if self.cx < self.cols {
            self.draw_cursor();
        }
        Ok(())
    }
}

pub fn init(boot: &BootInfo) {
    if !boot.has_fb {
        return;
    }
    let mut c = CONSOLE.lock();
    c.fb = boot.fb_base as *mut u32;
    c.stride = boot.stride;
    c.bgr = boot.bgr;
    c.width = boot.width;
    c.height = boot.height;
    c.scale = if boot.height >= 1440 { 2 } else { 1 };
    let (cw, ch) = (GLYPH_W * c.scale, GLYPH_H * c.scale);
    let margin = 8;
    c.cols = ((boot.width - 2 * margin) / cw).min(MAX_COLS);
    c.rows = ((boot.height - 2 * margin) / ch).min(MAX_ROWS);
    c.origin_x = (boot.width - c.cols * cw) / 2;
    c.origin_y = (boot.height - c.rows * ch) / 2;
    c.clear();
    c.ready = true;
}

pub fn size() -> (usize, usize) {
    let c = CONSOLE.lock();
    (c.cols, c.rows)
}

pub fn clear() {
    let mut c = CONSOLE.lock();
    if c.ready {
        c.clear();
        c.draw_cursor();
    }
}

pub fn set_fg(color: u32) {
    CONSOLE.lock().fg = color;
}

/// Move the cursor (clamped to the screen).
pub fn goto(col: usize, row: usize) {
    let mut c = CONSOLE.lock();
    if !c.ready {
        return;
    }
    c.hide_cursor();
    c.cx = col.min(c.cols - 1);
    c.cy = row.min(c.rows - 1);
    c.show_cursor();
}

/// Stop drawing (a graphical program has the screen), or start again and
/// put the text back.
pub fn suspend(on: bool) {
    let mut c = CONSOLE.lock();
    let was = c.suspended;
    c.suspended = on;
    if was && !on && c.ready {
        c.redraw();
    }
}

/// Move the cursor back `count` cells without erasing anything, going up
/// to the end of the previous row at the left edge. Lets a line editor
/// step back into text it has already printed.
pub fn left(count: usize) {
    let mut c = CONSOLE.lock();
    if c.mirror {
        for _ in 0..count {
            serial::write_str("");
        }
    }
    if !c.ready {
        return;
    }
    c.hide_cursor();
    for _ in 0..count {
        if c.cx > 0 {
            c.cx -= 1;
        } else if c.cy > 0 {
            c.cy -= 1;
            c.cx = c.cols - 1;
        }
    }
    c.show_cursor();
}

/// Blank everything from the cursor to the end of its line.
pub fn clear_to_eol() {
    let mut c = CONSOLE.lock();
    if !c.ready {
        return;
    }
    let row = c.cy;
    for col in c.cx..c.cols {
        let idx = row * MAX_COLS + col;
        if c.cells[idx] != BLANK {
            c.cells[idx] = BLANK;
            c.draw_cell(col, row);
        }
    }
    c.show_cursor();
}

/// Turn copying of console output to the serial port on or off. Full-screen
/// programs switch it off so their redraws don't flood the log.
pub fn set_mirror(on: bool) {
    CONSOLE.lock().mirror = on;
}

pub fn reset_fg() {
    set_fg(FG);
}

#[doc(hidden)]
pub fn _print(args: fmt::Arguments) {
    let _ = CONSOLE.lock().write_fmt(args);
}

/// Print without taking the lock. Only for the panic handler.
pub fn panic_print(args: fmt::Arguments) {
    let c = unsafe { CONSOLE.force() };
    // Whatever had the screen, the panic message must be seen.
    if c.suspended {
        c.suspended = false;
        if c.ready {
            c.redraw();
        }
    }
    c.fg = RED;
    c.mirror = true;
    let _ = c.write_fmt(args);
}
