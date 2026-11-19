//! File descriptors: what a process's numbered inputs and outputs point at.
//!
//! Every process has a small table of them. By convention 0 is standard
//! input, 1 standard output and 2 standard error. Each entry is one of:
//!
//!   * the console (keyboard in, screen out),
//!   * one end of a pipe — a 64 KiB queue of bytes in the kernel that one
//!     process writes and another reads, which is how `a | b` works,
//!   * a file being read from (`< file`) or collected for writing (`> file`),
//!   * a network socket (see net.rs).
//!
//! Reads and writes never wait inside the kernel. When a pipe is empty or
//! full they report "would block", and the system call layer takes the
//! process off the CPU until the other end does something (every change to
//! a pipe wakes whoever is waiting on it), then has it try again.

use alloc::collections::VecDeque;
use alloc::string::String;
use alloc::sync::Arc;
use alloc::vec::Vec;

use crate::net::{self, Io, SocketHandle};
use crate::pty::Pty;
use crate::{fs, input};
use crate::sched::{self, Wait};
use crate::sync::IrqLock;

const PIPE_CAPACITY: usize = 64 * 1024;
/// A writer that found the pipe full is woken once this much room has been
/// made (or the pipe has emptied), not for every byte read: otherwise a
/// reader taking one byte at a time would wake it a byte at a time too.
const WRITER_WAKE_ROOM: usize = 16 * 1024;

struct PipeState {
    data: VecDeque<u8>,
    readers: u32,
    writers: u32,
    /// Someone found the pipe empty (or full) and may be blocked on it.
    /// Wake-ups are skipped when nobody is, which is most of the time.
    reader_waiting: bool,
    writer_waiting: bool,
}

pub struct Pipe {
    state: IrqLock<PipeState>,
}

pub enum Transfer {
    /// This many bytes were moved.
    Done(usize),
    /// Nothing can be done right now; try again shortly.
    WouldBlock,
    /// Reading: every writer has gone and the pipe is empty.
    /// Writing: every reader has gone, so nobody will ever see the data.
    Closed,
}

impl From<Io> for Transfer {
    fn from(io: Io) -> Transfer {
        match io {
            Io::Done(n) => Transfer::Done(n),
            Io::WouldBlock => Transfer::WouldBlock,
            Io::Closed => Transfer::Closed,
        }
    }
}

impl Pipe {
    /// A pipe with no ends yet; descriptors made with `Fd::stream` take them.
    pub fn bare() -> Arc<Pipe> {
        Arc::new(Pipe {
            state: IrqLock::new(PipeState {
                data: VecDeque::new(),
                readers: 0,
                writers: 0,
                reader_waiting: false,
                writer_waiting: false,
            }),
        })
    }

    /// A new pipe, as its (read end, write end).
    pub fn create() -> (Fd, Fd) {
        let pipe = Arc::new(Pipe {
            state: IrqLock::new(PipeState {
                data: VecDeque::new(),
                readers: 1,
                writers: 1,
                reader_waiting: false,
                writer_waiting: false,
            }),
        });
        (Fd::PipeRead(pipe.clone()), Fd::PipeWrite(pipe))
    }

    /// What a task waiting on this pipe blocks on.
    fn channel(&self) -> Wait {
        Wait::Pipe(self as *const Pipe as usize)
    }

    fn read(&self, out: &mut [u8]) -> Transfer {
        let (n, wake) = {
            let mut s = self.state.lock();
            if s.data.is_empty() {
                if s.writers == 0 {
                    return Transfer::Closed;
                }
                s.reader_waiting = true;
                return Transfer::WouldBlock;
            }
            let n = out.len().min(s.data.len());
            for slot in out[..n].iter_mut() {
                *slot = s.data.pop_front().unwrap();
            }
            let room = PIPE_CAPACITY - s.data.len();
            let wake = s.writer_waiting && (room >= WRITER_WAKE_ROOM || s.data.is_empty());
            if wake {
                s.writer_waiting = false;
            }
            (n, wake)
        };
        if wake {
            sched::wake(self.channel()); // there is room again
        }
        Transfer::Done(n)
    }

    /// (readable, writable, hung up) for the read end. If it is not ready,
    /// the next write will wake whoever is waiting.
    fn read_ready(&self) -> (bool, bool, bool) {
        let mut s = self.state.lock();
        if !s.data.is_empty() {
            return (true, false, false);
        }
        if s.writers == 0 {
            return (true, false, true);
        }
        s.reader_waiting = true;
        (false, false, false)
    }

    /// The same for the write end.
    fn write_ready(&self) -> (bool, bool, bool) {
        let mut s = self.state.lock();
        if s.readers == 0 {
            return (false, true, true);
        }
        if s.data.len() < PIPE_CAPACITY {
            return (false, true, false);
        }
        s.writer_waiting = true;
        (false, false, false)
    }

    fn write(&self, data: &[u8]) -> Transfer {
        let (n, wake) = {
            let mut s = self.state.lock();
            if s.readers == 0 {
                return Transfer::Closed;
            }
            let n = data.len().min(PIPE_CAPACITY - s.data.len());
            if n == 0 && !data.is_empty() {
                s.writer_waiting = true;
                return Transfer::WouldBlock;
            }
            s.data.extend(&data[..n]);
            let wake = n > 0 && s.reader_waiting;
            if wake {
                s.reader_waiting = false;
            }
            (n, wake)
        };
        if wake {
            sched::wake(self.channel()); // there is something to read
        }
        Transfer::Done(n)
    }
}

/// A file being written through a descriptor. The bytes are collected in
/// memory and stored as the file when the last descriptor is closed.
pub struct FileSink {
    name: String,
    data: IrqLock<Vec<u8>>,
}

impl Drop for FileSink {
    fn drop(&mut self) {
        let _ = fs::write(&self.name, core::mem::take(&mut *self.data.lock()));
    }
}

pub enum Fd {
    Console,
    PipeRead(Arc<Pipe>),
    PipeWrite(Arc<Pipe>),
    FileRead { data: Arc<Vec<u8>>, position: usize },
    FileWrite(Arc<FileSink>),
    /// A network socket: a TCP connection, a listening port or a UDP port.
    Socket(Arc<SocketHandle>),
    /// The terminal end of a pseudo-terminal (see pty.rs).
    PtyMaster(Arc<Pty>),
    /// The program end of a pseudo-terminal: behaves like the console.
    PtySlave(Arc<Pty>),
    /// The display, taken over by a graphical program: reading gives key
    /// and mouse events. Closing the last copy gives the display back.
    Display(Arc<crate::display::Handle>),
    /// A two-way connection (see local.rs): reads come from one pipe,
    /// writes go into another.
    Stream { rx: Arc<Pipe>, tx: Arc<Pipe> },
    /// A name a server is listening on; reading is done with local_accept.
    Listener(Arc<crate::local::Listener>),
}

impl Fd {
    /// One end of a two-way connection: reading from `rx`, writing to `tx`.
    pub fn stream(rx: Arc<Pipe>, tx: Arc<Pipe>) -> Fd {
        rx.state.lock().readers += 1;
        tx.state.lock().writers += 1;
        Fd::Stream { rx, tx }
    }

    /// Open a file for reading. None if it does not exist.
    pub fn open_read(name: &str) -> Option<Fd> {
        Some(Fd::FileRead { data: Arc::new(fs::read(name)?), position: 0 })
    }

    /// Open a file for writing, replacing it (or, with `append`, adding to it).
    pub fn open_write(name: &str, append: bool) -> Fd {
        let existing = if append { fs::read(name).unwrap_or_default() } else { Vec::new() };
        Fd::FileWrite(Arc::new(FileSink { name: String::from(name), data: IrqLock::new(existing) }))
    }

    /// The socket behind this descriptor, if it is one.
    pub fn socket(&self) -> Option<u32> {
        match self {
            Fd::Socket(socket) => Some(socket.id()),
            _ => None,
        }
    }

    pub fn is_console(&self) -> bool {
        matches!(self, Fd::Console)
    }

    /// Is this a terminal - the console, or the program end of a pty?
    pub fn is_terminal(&self) -> bool {
        matches!(self, Fd::Console | Fd::PtySlave(_))
    }

    /// (readable, writable, hung up / finished), for poll.
    pub fn readiness(&self) -> (bool, bool, bool) {
        match self {
            Fd::Console => (input::available(), true, false),
            Fd::PipeRead(pipe) => pipe.read_ready(),
            Fd::PipeWrite(pipe) => pipe.write_ready(),
            Fd::FileRead { .. } | Fd::FileWrite(_) => (true, true, false),
            Fd::Socket(socket) => net::readiness(socket.id()),
            Fd::PtyMaster(pty) => pty.master_ready(),
            Fd::PtySlave(pty) => pty.slave_ready(),
            Fd::Display(_) => (crate::display::has_events(), false, false),
            Fd::Stream { rx, tx } => {
                let (readable, _, closed) = rx.read_ready();
                let (_, writable, gone) = tx.write_ready();
                (readable, writable, closed || gone)
            }
            Fd::Listener(listener) => (listener.has_pending(), false, false),
        }
    }

    /// What a reader blocks on when this descriptor has nothing yet.
    pub fn read_channel(&self) -> Wait {
        match self {
            Fd::Stream { rx, .. } => rx.channel(),
            other => other.channel(),
        }
    }

    /// What a writer blocks on when this descriptor has no room.
    pub fn write_channel(&self) -> Wait {
        match self {
            Fd::Stream { tx, .. } => tx.channel(),
            other => other.channel(),
        }
    }

    /// What to block on when this descriptor reports "would block".
    pub fn channel(&self) -> Wait {
        match self {
            Fd::Stream { rx, .. } => rx.channel(),
            Fd::Listener(listener) => listener.channel(),
            Fd::PipeRead(pipe) | Fd::PipeWrite(pipe) => pipe.channel(),
            Fd::PtyMaster(pty) | Fd::PtySlave(pty) => pty.channel(),
            Fd::Socket(_) => Wait::Net,
            Fd::Display(_) => Wait::Input,
            _ => Wait::Input,
        }
    }

    /// Read from anything except the console (which the caller handles,
    /// because keyboard input lives elsewhere).
    pub fn read(&mut self, out: &mut [u8]) -> Transfer {
        match self {
            Fd::PipeRead(pipe) => pipe.read(out),
            Fd::FileRead { data, position } => {
                let n = out.len().min(data.len() - *position);
                out[..n].copy_from_slice(&data[*position..*position + n]);
                *position += n;
                if n == 0 && !out.is_empty() { Transfer::Closed } else { Transfer::Done(n) }
            }
            Fd::Socket(socket) => net::read(socket.id(), out).into(),
            Fd::PtyMaster(pty) => pty.master_read(out),
            Fd::PtySlave(pty) => pty.slave_read_raw(out),
            Fd::Stream { rx, .. } => rx.read(out),
            Fd::Display(_) => {
                if out.len() < 16 {
                    return Transfer::Done(0);
                }
                match crate::display::read(out) {
                    0 => Transfer::WouldBlock,
                    n => Transfer::Done(n),
                }
            }
            _ => Transfer::Closed,
        }
    }

    /// Write to anything except the console.
    pub fn write(&mut self, data: &[u8]) -> Transfer {
        match self {
            Fd::PipeWrite(pipe) => pipe.write(data),
            Fd::FileWrite(sink) => {
                sink.data.lock().extend_from_slice(data);
                Transfer::Done(data.len())
            }
            Fd::Socket(socket) => net::write(socket.id(), data).into(),
            Fd::PtyMaster(pty) => pty.master_write(data),
            Fd::PtySlave(pty) => pty.slave_write(data),
            Fd::Stream { tx, .. } => tx.write(data),
            _ => Transfer::Closed,
        }
    }
}

// Pipes count their open ends, so the other side can tell when it is alone.
impl Clone for Fd {
    fn clone(&self) -> Fd {
        match self {
            Fd::Console => Fd::Console,
            Fd::PipeRead(pipe) => {
                pipe.state.lock().readers += 1;
                Fd::PipeRead(pipe.clone())
            }
            Fd::PipeWrite(pipe) => {
                pipe.state.lock().writers += 1;
                Fd::PipeWrite(pipe.clone())
            }
            Fd::FileRead { data, position } => Fd::FileRead { data: data.clone(), position: *position },
            Fd::FileWrite(sink) => Fd::FileWrite(sink.clone()),
            Fd::Socket(socket) => Fd::Socket(socket.clone()),
            Fd::PtyMaster(pty) => {
                pty.add_end(true);
                Fd::PtyMaster(pty.clone())
            }
            Fd::PtySlave(pty) => {
                pty.add_end(false);
                Fd::PtySlave(pty.clone())
            }
            Fd::Display(handle) => Fd::Display(handle.clone()),
            Fd::Stream { rx, tx } => Fd::stream(rx.clone(), tx.clone()),
            Fd::Listener(listener) => Fd::Listener(listener.clone()),
        }
    }
}

impl Drop for Fd {
    fn drop(&mut self) {
        // The other side may be waiting, and must now see the hang-up.
        match self {
            Fd::PipeRead(pipe) => {
                pipe.state.lock().readers -= 1;
                sched::wake(pipe.channel());
            }
            Fd::PipeWrite(pipe) => {
                pipe.state.lock().writers -= 1;
                sched::wake(pipe.channel());
            }
            Fd::PtyMaster(pty) => pty.drop_end(true),
            Fd::PtySlave(pty) => pty.drop_end(false),
            Fd::Stream { rx, tx } => {
                rx.state.lock().readers -= 1;
                sched::wake(rx.channel());
                tx.state.lock().writers -= 1;
                sched::wake(tx.channel());
            }
            _ => {}
        }
    }
}

/// A process's descriptor table: index = descriptor number.
pub type FdTable = Vec<Option<Fd>>;
