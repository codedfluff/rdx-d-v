//! Pseudo-terminals: a stand-in for the keyboard and screen, so that a
//! program can be the "terminal" for another one.
//!
//! A pty has two ends. The *terminal end* (the "master") belongs to whatever
//! plays the part of the terminal - a terminal window, or `ptyrun`. The
//! *program end* (the "slave") is handed to a shell or any other program as
//! its standard input and output, and to that program it looks just like
//! the console:
//!
//!   * isatty() says yes, so the shell edits lines and shows a prompt;
//!   * what the terminal end writes arrives as keystrokes, and what the
//!     program prints comes out at the terminal end;
//!   * screen control (colours, moving the cursor, clearing) is sent along
//!     as standard ANSI escape sequences for the terminal to carry out;
//!   * Ctrl+C typed at the terminal interrupts the programs the shell has
//!     made the foreground of this terminal, exactly as on the console;
//!   * closing the terminal end hangs up: the foreground programs are sent
//!     SIGHUP, and the program end reads end-of-input.
//!
//! Every change wakes whoever is waiting on either end.

use alloc::collections::VecDeque;
use alloc::sync::Arc;
use alloc::vec::Vec;

use crate::pipe::{Fd, Transfer};
use crate::sched::{self, Wait};
use crate::sync::IrqLock;

/// How much each direction holds before the writer has to wait.
const CAPACITY: usize = 64 * 1024;

struct State {
    /// Typed at the terminal, waiting to be read by the program.
    input: VecDeque<u8>,
    /// Written by the program, waiting to be shown by the terminal.
    output: VecDeque<u8>,
    masters: u32,
    slaves: u32,
    cols: u16,
    rows: u16,
    /// The programs Ctrl+C at this terminal goes to.
    foreground: Vec<u32>,
}

pub struct Pty {
    state: IrqLock<State>,
}

impl Pty {
    /// A new pseudo-terminal, as its (terminal end, program end).
    pub fn create() -> (Fd, Fd) {
        let pty = Arc::new(Pty {
            state: IrqLock::new(State {
                input: VecDeque::new(),
                output: VecDeque::new(),
                masters: 1,
                slaves: 1,
                cols: 80,
                rows: 25,
                foreground: Vec::new(),
            }),
        });
        (Fd::PtyMaster(pty.clone()), Fd::PtySlave(pty))
    }

    /// What a task waiting on either end blocks on.
    pub fn channel(&self) -> Wait {
        Wait::Pipe(self as *const Pty as usize)
    }

    fn changed(&self) {
        sched::wake(self.channel());
    }

    // ------------------------------------------------------------ terminal end

    /// What the program has printed.
    pub fn master_read(&self, out: &mut [u8]) -> Transfer {
        let n = {
            let mut s = self.state.lock();
            if s.output.is_empty() {
                return if s.slaves == 0 { Transfer::Closed } else { Transfer::WouldBlock };
            }
            let n = out.len().min(s.output.len());
            for slot in out[..n].iter_mut() {
                *slot = s.output.pop_front().unwrap();
            }
            n
        };
        self.changed();
        Transfer::Done(n)
    }

    /// Keystrokes for the program. Ctrl+C becomes an interrupt signal for
    /// the foreground programs, if there are any.
    pub fn master_write(&self, data: &[u8]) -> Transfer {
        let mut interrupt = Vec::new();
        let mut used = 0;
        {
            let mut s = self.state.lock();
            if s.slaves == 0 {
                return Transfer::Closed;
            }
            for &b in data {
                if b == 0x03 && !s.foreground.is_empty() {
                    interrupt = s.foreground.clone();
                } else if s.input.len() < CAPACITY {
                    s.input.push_back(b);
                } else {
                    break;
                }
                used += 1;
            }
        }
        // Signal outside the lock. If every foreground program has already
        // gone, the key is passed on like any other.
        if !interrupt.is_empty() && !sched::signal_pids(&interrupt, sched::SIGINT) {
            self.state.lock().input.push_back(0x03);
        }
        if used == 0 && !data.is_empty() {
            return Transfer::WouldBlock;
        }
        self.changed();
        Transfer::Done(used)
    }

    // ------------------------------------------------------------ program end

    /// Keystrokes, exactly as they came (what getchar() wants).
    pub fn slave_read_raw(&self, out: &mut [u8]) -> Transfer {
        let n = {
            let mut s = self.state.lock();
            if s.input.is_empty() {
                return if s.masters == 0 { Transfer::Closed } else { Transfer::WouldBlock };
            }
            let n = out.len().min(s.input.len());
            for slot in out[..n].iter_mut() {
                *slot = s.input.pop_front().unwrap();
            }
            n
        };
        self.changed();
        Transfer::Done(n)
    }

    /// Like reading from the console with read(): what has been typed, up
    /// to the end of a line, shown on the terminal as it is taken. Ctrl+D
    /// on its own means end of input. Ok(empty) is end of input.
    pub fn slave_read_line(&self, max: usize) -> Result<Vec<u8>, Transfer> {
        let mut data = Vec::new();
        {
            let mut s = self.state.lock();
            if s.input.is_empty() {
                return if s.masters == 0 { Ok(data) } else { Err(Transfer::WouldBlock) };
            }
            while data.len() < max {
                match s.input.front().copied() {
                    None => break,
                    Some(0x04) => {
                        if data.is_empty() {
                            s.input.pop_front(); // deliver the end now
                        }
                        break; // or after the text that came before it
                    }
                    Some(b) => {
                        s.input.pop_front();
                        data.push(b);
                        if b == b'\n' {
                            break;
                        }
                    }
                }
            }
            for &b in &data {
                s.output.push_back(b); // echo
            }
        }
        self.changed();
        Ok(data)
    }

    pub fn slave_write(&self, data: &[u8]) -> Transfer {
        let n = {
            let mut s = self.state.lock();
            if s.masters == 0 {
                return Transfer::Closed;
            }
            let n = data.len().min(CAPACITY.saturating_sub(s.output.len()));
            if n == 0 && !data.is_empty() {
                return Transfer::WouldBlock;
            }
            s.output.extend(&data[..n]);
            n
        };
        self.changed();
        Transfer::Done(n)
    }

    /// Send a screen-control sequence to the terminal. These are small and
    /// must not be split, so they go in whole even if the buffer is full.
    pub fn control(&self, sequence: &[u8]) {
        {
            let mut s = self.state.lock();
            if s.masters == 0 {
                return;
            }
            s.output.extend(sequence);
        }
        self.changed();
    }

    // ------------------------------------------------------------ settings

    pub fn size(&self) -> (u16, u16) {
        let s = self.state.lock();
        (s.cols, s.rows)
    }

    pub fn set_size(&self, cols: u16, rows: u16) {
        let mut s = self.state.lock();
        s.cols = cols.clamp(1, 1000);
        s.rows = rows.clamp(1, 1000);
    }

    pub fn set_foreground(&self, pids: &[u32]) {
        let mut s = self.state.lock();
        s.foreground.clear();
        s.foreground.extend_from_slice(pids);
    }

    // ------------------------------------------------------------ poll

    /// (readable, writable, hung up) for the terminal end.
    pub fn master_ready(&self) -> (bool, bool, bool) {
        let s = self.state.lock();
        let gone = s.slaves == 0;
        (!s.output.is_empty() || gone, !gone && s.input.len() < CAPACITY, gone)
    }

    /// (readable, writable, hung up) for the program end.
    pub fn slave_ready(&self) -> (bool, bool, bool) {
        let s = self.state.lock();
        let gone = s.masters == 0;
        (!s.input.is_empty() || gone, !gone && s.output.len() < CAPACITY, gone)
    }

    // ------------------------------------------------------------ ends opening and closing

    pub fn add_end(&self, master: bool) {
        let mut s = self.state.lock();
        if master {
            s.masters += 1;
        } else {
            s.slaves += 1;
        }
    }

    pub fn drop_end(&self, master: bool) {
        let hang_up = {
            let mut s = self.state.lock();
            if master {
                s.masters -= 1;
                if s.masters == 0 { core::mem::take(&mut s.foreground) } else { Vec::new() }
            } else {
                s.slaves -= 1;
                Vec::new()
            }
        };
        // The terminal has gone away: tell the programs that were using it.
        if !hang_up.is_empty() {
            sched::signal_pids(&hang_up, sched::SIGHUP);
        }
        self.changed();
    }
}
