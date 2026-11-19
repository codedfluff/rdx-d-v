//! System calls: how ring 3 programs ask the kernel to do things.
//!
//! A program loads the call number into rax and up to three arguments into
//! rdi, rsi, rdx, then executes `int 0x80`. The result comes back in rax
//! (-1 on error). All other registers are preserved.
//!
//!   0 exit(code)                      1 write(buf, len)        to stdout
//!   2 getchar()                       3 sleep(ms)
//!   4 uptime_ms()                     5 getpid()
//!   6 sbrk(increment)                 7 file_read(name, buf, max)
//!   8 file_write(name, buf, len)      9 yield()
//!  10 console(op, a, b)              11 pollchar()
//!  12 thread_create(entry, a1, a2)   13 thread_exit()
//!  14 thread_join(tid)               15 gettid()
//!  16 cpu_count()                    17 fd_write(fd, buf, len)
//!  18 fd_read(fd, buf, max)          19 pipe(int fds[2])
//!  20 close(fd)                      21 spawn(cmdline, stdin_fd, stdout_fd)
//!  22 wait(pid)                      23 shm_open(name, size)
//!  24 shm_unlink(name)               25 file_list(index, name_buf, max)
//!  26 file_delete(name)              27 console_key()
//!  28 net_resolve(name, ip_out)      29 tcp_open(ip, port)
//!  30 tcp_wait(fd)                   31 tcp_listen(port)
//!  32 tcp_accept(fd)                 33 udp_open(port)
//!  34 udp_sendto(fd, ip, port, buf, len)
//!  35 udp_recvfrom(fd, buf, max, from_out)
//!  36 net_info(out)                  37 tcp_shutdown(fd)
//!  38 recv_nb(fd, buf, max)   non-blocking read (-2 = nothing yet)
//!  39 getcwd(buf)                   40 chdir(path)
//!  41 mkdir(path)                   42 rmdir(path, recursive)
//!
//! Calls with more than three arguments pass the fourth and fifth in r10
//! and r8.
//!
//! console ops: 0 clear, 1 goto(col,row), 2 size -> cols<<16|rows,
//! 3 color(rgb), 4 clear to end of line, 5 serial mirror on/off,
//! 6 move the cursor left by n cells without erasing.
//!
//!  43 signal(handler)                44 sigreturn()
//!  45 kill(pid, signal)              46 getenv(name, buf, max)
//!  47 setenv(name, value)            48 env_list(index, buf, max)
//!  49 time()   seconds since 1970    50 file_stat(path, out)
//!  51 proc_list(index, out)          52 sysinfo(out)
//!  53 power(op)   0 shut down, 1 restart
//!  54 isatty(fd)  1 if the descriptor is the console
//!  55 foreground(pids, count)   which processes Ctrl+C goes to
//!  56 open(path, mode)          0 read, 1 write, 2 append; returns a descriptor
//!  57 last_error(buf, max)      why this process's last failed call failed
//!  58 debug(op)                 kernel self-tests: 0 worker, 1 crash, 2 panic
//!  59 ping_send(ip, sequence)   60 ping_reply(sequence)
//!  61 poll(fds, count, timeout_ms)   wait until any of several is ready
//!  62 pty_open(int fds[2])          a pseudo-terminal: terminal end, program end
//!  63 pty_resize(fd, cols, rows)    64 spawn_fds(cmdline, in, out, err)
//!  65 display_open(info)            take over the screen, keys and mouse
//!  66 local_listen(name)            67 local_connect(name)
//!  68 local_accept(fd)              69 shm_close(address)
//!  70 pty_foreground(fd, pids, count)   who Ctrl+C and hang-up at that pty go to
//!  71 getrandom(buf, len)           unpredictable bytes (up to 4096), for keys
//!
//! Calls that have to wait (for a key, a pipe, another process) do not block
//! inside the kernel. They back the instruction pointer up over the 2-byte
//! `int 0x80`, take the caller off the CPU until the thing it is waiting
//! for happens (see `sched::Wait`) and return, so the call simply runs again
//! when the caller is woken.
//!
//! Signals: a process that has called signal(handler) is not killed by
//! Ctrl+C or kill; instead its first thread is diverted into
//! handler(signal_number), and carries on where it was when that returns.
//! A call that was waiting is woken for this, and afterwards returns -4
//! ("interrupted") rather than going back to waiting.

use alloc::string::String;
use alloc::sync::Arc;
use alloc::vec;
use alloc::vec::Vec;

use crate::arch::{self, Frame, TICK_HZ};
use crate::console;
use crate::exec::{self, HEAP_BASE, HEAP_MAX};
use crate::memory::{self, PAGE};
use crate::pipe::{Fd, Pipe, Transfer};
use crate::sched::{self, Process, Wait};
use crate::pty::Pty;
use crate::{clock, disk, fs, input, net, shm, smp};

const ERROR: u64 = u64::MAX;
const MAX_IO: u64 = 1024 * 1024;
/// A whole file can be written in one go up to this size.
const MAX_FILE: u64 = 16 * 1024 * 1024;

/// Copy `len` bytes out of the calling process, refusing anything that is
/// not entirely its own readable memory.
fn user_bytes(addr: u64, len: u64) -> Option<Vec<u8>> {
    user_bytes_up_to(addr, len, MAX_IO)
}

fn user_bytes_up_to(addr: u64, len: u64, limit: u64) -> Option<Vec<u8>> {
    if len > limit || !memory::user_range_ok(memory::current_address_space(), addr, len, false) {
        return None;
    }
    Some(unsafe { core::slice::from_raw_parts(addr as *const u8, len as usize) }.to_vec())
}

/// Copy bytes into the calling process, if the whole range is writable.
fn copy_to_user(addr: u64, data: &[u8]) -> bool {
    if !memory::user_range_ok(memory::current_address_space(), addr, data.len() as u64, true) {
        return false;
    }
    unsafe { core::ptr::copy_nonoverlapping(data.as_ptr(), addr as *mut u8, data.len()) };
    true
}

/// Read a NUL-terminated string of at most `max` bytes from the caller.
fn user_string(addr: u64, max: u64) -> Option<String> {
    let space = memory::current_address_space();
    let mut text = String::new();
    for i in 0..max {
        if !memory::user_range_ok(space, addr + i, 1, false) {
            return None;
        }
        let b = unsafe { *((addr + i) as *const u8) };
        if b == 0 {
            return if text.is_empty() { None } else { Some(text) };
        }
        text.push(b as char);
    }
    None
}

fn user_name(addr: u64) -> Option<String> {
    user_string(addr, 64)
}

fn sbrk(increment: i64) -> u64 {
    let Some(process) = sched::current_process() else { return ERROR };
    // Held throughout, so two threads growing the heap at once take turns.
    let mut brk = process.brk.lock();
    let old = *brk;
    let Some(new) = old.checked_add_signed(increment) else { return ERROR };
    if new < HEAP_BASE || new > HEAP_BASE + HEAP_MAX {
        return ERROR;
    }
    // Map any pages the heap grew into. (Shrinking keeps the pages mapped.)
    let mut page = (old + PAGE - 1) & !(PAGE - 1);
    while page < new {
        if memory::map_user_page(process.space, page, true).is_none() {
            return ERROR;
        }
        page += PAGE;
    }
    *brk = new;
    old
}

/// How a call that may have to wait turned out.
enum Outcome {
    Value(u64),
    /// Not possible yet: wait for this, then run the call again.
    Retry(Wait),
    /// The other end of the pipe is gone and will never read this: the
    /// writer is ended, as `yes | head` needs.
    BrokenPipe,
}

/// Close everything a finished process had open, now rather than whenever
/// its memory is reclaimed: something may be waiting for the pipe to end.
fn close_all(process: Option<Arc<Process>>) {
    if let Some(process) = process {
        let fds = core::mem::take(&mut *process.fds.lock());
        drop(fds);
    }
}

fn fd_write(fd: u64, data: &[u8]) -> Outcome {
    let Some(process) = sched::current_process() else { return Outcome::Value(ERROR) };
    let mut fds = process.fds.lock();
    match fds.get_mut(fd as usize).and_then(|slot| slot.as_mut()) {
        None => Outcome::Value(ERROR),
        Some(Fd::Console) => {
            crate::print!("{}", String::from_utf8_lossy(data));
            Outcome::Value(data.len() as u64)
        }
        Some(target) => match target.write(data) {
            Transfer::Done(n) => Outcome::Value(n as u64),
            Transfer::WouldBlock => Outcome::Retry(target.write_channel()),
            Transfer::Closed if matches!(target, Fd::PipeWrite(_)) => Outcome::BrokenPipe,
            Transfer::Closed => Outcome::Value(ERROR),
        },
    }
}

/// Read up to `max` bytes. Returns the bytes; empty means end of input.
/// From a pseudo-terminal, `raw` takes keystrokes exactly as they come;
/// otherwise it reads like the console does (a line at a time, echoed).
fn fd_read(fd: u64, max: usize, raw: bool) -> Result<Vec<u8>, Outcome> {
    let Some(process) = sched::current_process() else { return Err(Outcome::Value(ERROR)) };
    let mut fds = process.fds.lock();
    match fds.get_mut(fd as usize).and_then(|slot| slot.as_mut()) {
        None => Err(Outcome::Value(ERROR)),
        Some(Fd::PtySlave(pty)) if !raw => {
            let pty = pty.clone();
            pty.slave_read_line(max).map_err(|_| Outcome::Retry(pty.channel()))
        }
        Some(Fd::Console) => {
            // Keyboard: wait for at least one key, echo what is typed, and
            // hand over what is there, up to the end of a line. Ctrl+D on
            // its own means "end of input".
            let mut data = Vec::new();
            while data.len() < max {
                match input::pop() {
                    None => break,
                    Some(0x04) => {
                        if !data.is_empty() {
                            input::unpop(0x04); // deliver the text first, then the end
                        }
                        return Ok(data);
                    }
                    Some(b) => {
                        crate::print!("{}", b as char);
                        data.push(b);
                        if b == b'\n' {
                            break;
                        }
                    }
                }
            }
            if data.is_empty() { Err(Outcome::Retry(Wait::Input)) } else { Ok(data) }
        }
        Some(source) => {
            let mut data = vec![0u8; max];
            match source.read(&mut data) {
                Transfer::Done(n) => {
                    data.truncate(n);
                    Ok(data)
                }
                Transfer::WouldBlock => Err(Outcome::Retry(source.read_channel())),
                Transfer::Closed => Ok(Vec::new()),
            }
        }
    }
}

/// What the calling process treats as its terminal: the first of its
/// standard output, standard error and standard input that is one. Screen
/// control and Ctrl+C are about this terminal.
enum Term {
    Console,
    Pty(Arc<Pty>),
}

fn terminal() -> Term {
    let Some(process) = sched::current_process() else { return Term::Console };
    let fds = process.fds.lock();
    for i in [1, 2, 0] {
        match fds.get(i).and_then(|slot| slot.as_ref()) {
            Some(Fd::Console) => return Term::Console,
            Some(Fd::PtySlave(pty)) => return Term::Pty(pty.clone()),
            _ => {}
        }
    }
    Term::Console
}

/// Screen control for a program whose terminal is a pty: the standard
/// ANSI escape sequence for it, which the terminal carries out.
fn pty_control(pty: &Pty, op: u64, b: u64, c: u64) -> u64 {
    let sequence = match op {
        0 => alloc::format!("\x1b[2J\x1b[H"),
        1 => alloc::format!("\x1b[{};{}H", c + 1, b + 1),
        2 => {
            let (cols, rows) = pty.size();
            return ((cols as u64) << 16) | rows as u64;
        }
        3 => alloc::format!("\x1b[38;2;{};{};{}m", (b >> 16) & 0xFF, (b >> 8) & 0xFF, b & 0xFF),
        4 => alloc::format!("\x1b[K"),
        5 => return 0, // copying to the serial log is the console's business
        6 if b > 0 => alloc::format!("\x1b[{}D", b.min(4096)),
        6 => return 0,
        _ => return ERROR,
    };
    pty.control(sequence.as_bytes());
    0
}

fn stdin_is_console() -> bool {
    sched::current_process().map_or(true, |p| p.fds.lock().first().and_then(|f| f.as_ref()).map_or(false, Fd::is_console))
}

/// The calling process's working directory, as a filesystem key.
fn cwd() -> String {
    sched::current_process().map_or_else(String::new, |p| p.cwd.lock().clone())
}

/// Resolve a user-supplied path against the process's working directory.
fn resolve_user(addr: u64) -> Option<String> {
    user_string(addr, 256).map(|path| fs::resolve(&cwd(), &path))
}

/// Record why a system call is failing (for `last_error`) and return -1.
fn fail(reason: &'static str) -> u64 {
    if let Some(process) = sched::current_process() {
        *process.last_error.lock() = reason;
    }
    ERROR
}

/// Start a program for the calling process. Its standard input and output
/// are copies of the caller's descriptors `stdin_fd` / `stdout_fd` (or of
/// the caller's own standard ones, for -1); its descriptor 2 is a copy of
/// the caller's. Returns the process ID, -1 if there is no such program, or
/// -2 if it could not be started (`last_error` says why).
fn spawn(cmdline: &str, stdin_fd: i64, stdout_fd: i64, stderr_fd: i64) -> u64 {
    const NOT_RUNNABLE: u64 = (-2i64) as u64;
    let Some(process) = sched::current_process() else { return ERROR };
    let Some(program) = cmdline.split_whitespace().next() else { return fail("there is no command") };
    let working = process.cwd.lock().clone();
    let env = process.env.lock().clone();
    let path = String::from(exec::env_get(&env, "PATH").unwrap_or(exec::DEFAULT_PATH));
    let Some((file, image)) = exec::resolve(program, &working, &path) else {
        return fail("no such program");
    };
    let table = {
        let fds = process.fds.lock();
        let pick = |wanted: i64, default: usize| {
            let index = if wanted < 0 { default } else { wanted as usize };
            fds.get(index).and_then(|slot| slot.clone())
        };
        let (Some(stdin), Some(stdout)) = (pick(stdin_fd, 0), pick(stdout_fd, 1)) else {
            fail("no such descriptor");
            return NOT_RUNNABLE;
        };
        let stderr = pick(stderr_fd, 2).unwrap_or(Fd::Console);
        vec![Some(stdin), Some(stdout), Some(stderr)]
    };
    match exec::spawn(&file, &image, cmdline, table, working, env) {
        Ok(pid) => pid as u64,
        Err(reason) => {
            fail(reason);
            NOT_RUNNABLE
        }
    }
}

/// Put something into the calling process's descriptor table; returns its
/// descriptor number.
fn install(fd: Fd) -> u64 {
    let Some(process) = sched::current_process() else { return ERROR };
    let mut fds = process.fds.lock();
    let slot = match fds.iter().position(|s| s.is_none()) {
        Some(slot) => slot,
        None if fds.len() < 64 => {
            fds.push(None);
            fds.len() - 1
        }
        None => return ERROR, // too many open descriptors; `fd` is dropped
    };
    fds[slot] = Some(fd);
    slot as u64
}

/// The network socket behind a descriptor of the calling process.
fn socket_of(fd: u64) -> Option<u32> {
    let process = sched::current_process()?;
    let fds = process.fds.lock();
    fds.get(fd as usize)?.as_ref()?.socket()
}

/// Copy a string to the caller as NUL-terminated text, cut to fit `max`
/// bytes. Returns the string's full length, or ERROR.
fn string_to_user(text: &str, addr: u64, max: u64) -> u64 {
    if max == 0 {
        return ERROR;
    }
    let mut bytes = text.as_bytes().to_vec();
    bytes.truncate(max as usize - 1);
    bytes.push(0);
    if copy_to_user(addr, &bytes) { text.len() as u64 } else { ERROR }
}

/// One line of the task list, as proc_list hands it to programs (64 bytes):
/// pid, tid, state, cpu (-1: none) as 32-bit values; CPU time used and
/// uptime at start, both in milliseconds, as 64-bit; then a 32-bit "is a
/// user program" flag and the name in the remaining 28 bytes.
fn task_record(t: &sched::TaskInfo) -> [u8; 64] {
    let mut record = [0u8; 64];
    let state: u32 = match t.state {
        sched::State::Ready => 0,
        sched::State::Running => 1,
        sched::State::Sleeping => 2,
        sched::State::Blocked => 3,
        sched::State::Dead => 4,
    };
    record[0..4].copy_from_slice(&t.pid.to_le_bytes());
    record[4..8].copy_from_slice(&t.id.to_le_bytes());
    record[8..12].copy_from_slice(&state.to_le_bytes());
    record[12..16].copy_from_slice(&t.cpu.map_or(-1i32, |c| c as i32).to_le_bytes());
    record[16..24].copy_from_slice(&(t.cpu_ticks * 1000 / TICK_HZ).to_le_bytes());
    record[24..32].copy_from_slice(&(t.started * 1000 / TICK_HZ).to_le_bytes());
    record[32..36].copy_from_slice(&(t.user as u32).to_le_bytes());
    let name = t.name.as_bytes();
    let n = name.len().min(27);
    record[36..36 + n].copy_from_slice(&name[..n]);
    record
}

/// Facts about the machine and the system, as sysinfo hands them to
/// programs: ten 64-bit numbers, then the version as text (16 bytes).
fn system_record() -> [u8; 96] {
    let m = memory::stats();
    let d = disk::info();
    let (file_bytes, file_count) = fs::usage();
    let numbers: [u64; 10] = [
        m.total_bytes,
        m.used_bytes,
        m.heap_size as u64,
        m.heap_used as u64,
        d.as_ref().map_or(0, |d| d.capacity_bytes),
        d.as_ref().map_or(0, |d| d.used_bytes),
        smp::online() as u64,
        arch::ticks() * 1000 / TICK_HZ,
        file_bytes as u64,
        file_count as u64,
    ];
    let mut record = [0u8; 96];
    for (i, n) in numbers.iter().enumerate() {
        record[i * 8..i * 8 + 8].copy_from_slice(&n.to_le_bytes());
    }
    let version = env!("CARGO_PKG_VERSION").as_bytes();
    let n = version.len().min(15);
    record[80..80 + n].copy_from_slice(&version[..n]);
    record
}

/// Read a 4-byte IPv4 address from the caller.
fn user_ip(addr: u64) -> Option<net::Ip> {
    user_bytes(addr, 4).map(|b| [b[0], b[1], b[2], b[3]])
}

pub fn handle(frame: *mut Frame) -> *mut Frame {
    let f = unsafe { &mut *frame };
    if f.cs & 3 != 3 {
        return frame; // system calls are for ring 3 only
    }
    let (number, a, b, c) = (f.rax, f.rdi, f.rsi, f.rdx);
    let (d, e) = (f.r10, f.r8);
    // Read before any "is it there yet?" check below, so that a wake-up
    // arriving between the check and going to sleep is never missed.
    let seen = sched::events();
    let resumed = sched::syscall_started();

    // Wait for something, then run this same system call again.
    macro_rules! retry {
        ($on:expr) => {{
            f.rip -= 2;
            sched::block_current($on, seen);
            return sched::schedule(frame, false);
        }};
    }
    macro_rules! settle {
        ($outcome:expr) => {
            match $outcome {
                Outcome::Value(v) => v,
                Outcome::Retry(on) => retry!(on),
                Outcome::BrokenPipe => {
                    close_all(sched::exit_current(141));
                    return sched::schedule(frame, false);
                }
            }
        };
    }

    f.rax = match number {
        // exit. Files and pipes are closed first, so that whoever is waiting
        // for this process finds its output complete the moment it is gone.
        0 => {
            close_all(sched::current_process());
            close_all(sched::exit_current(a as i64));
            return sched::schedule(frame, false);
        }
        // write: to standard output
        1 => match user_bytes(a, b) {
            Some(bytes) => settle!(fd_write(1, &bytes)),
            None => ERROR,
        },
        // getchar: one byte of standard input, or -1 at its end. From the
        // keyboard this is raw: no echo, and every key comes straight through.
        2 => {
            if stdin_is_console() {
                match input::pop() {
                    Some(ch) => ch as u64,
                    None => retry!(Wait::Input),
                }
            } else {
                match fd_read(0, 1, true) {
                    Ok(data) => data.first().map_or(ERROR, |&b| b as u64),
                    Err(outcome) => settle!(outcome),
                }
            }
        }
        // sleep
        3 => {
            f.rax = 0;
            sched::sleep_current(sched::ms_to_ticks(a));
            return sched::schedule(frame, false);
        }
        // uptime_ms
        4 => arch::ticks() * 1000 / TICK_HZ,
        // getpid
        5 => sched::current_pid() as u64,
        // sbrk
        6 => sbrk(a as i64),
        // file_read: returns the file's full size, copies at most `max` bytes
        7 => match resolve_user(a).and_then(|key| fs::read(&key)) {
            Some(data) => {
                let n = (data.len() as u64).min(c) as usize;
                if copy_to_user(b, &data[..n]) { data.len() as u64 } else { ERROR }
            }
            None => ERROR,
        },
        // file_write
        8 => match (resolve_user(a), user_bytes_up_to(b, c, MAX_FILE)) {
            (Some(key), Some(data)) => match fs::write(&key, data) {
                Ok(()) => 0,
                Err(reason) => fail(reason),
            },
            _ => fail("bad file name or data"),
        },
        // yield
        9 => {
            f.rax = 0;
            return sched::schedule(frame, false);
        }
        // console control
        10 => match terminal() {
            Term::Pty(pty) => pty_control(&pty, a, b, c),
            Term::Console => match a {
            0 => {
                console::clear();
                0
            }
            1 => {
                console::goto(b as usize, c as usize);
                0
            }
            2 => {
                let (cols, rows) = console::size();
                ((cols as u64) << 16) | rows as u64
            }
            3 => {
                console::set_fg(b as u32 & 0xFF_FFFF);
                0
            }
            4 => {
                console::clear_to_eol();
                0
            }
            5 => {
                console::set_mirror(b != 0);
                0
            }
            6 => {
                console::left((b as usize).min(4096));
                0
            }
            _ => ERROR,
            },
        },
        // pollchar: like getchar, but returns -1 instead of waiting
        11 => {
            if stdin_is_console() {
                input::pop().map_or(ERROR, |ch| ch as u64)
            } else {
                match fd_read(0, 1, true) {
                    Ok(data) => data.first().map_or(ERROR, |&b| b as u64),
                    Err(_) => ERROR,
                }
            }
        }
        // thread_create(entry, arg1, arg2): returns the new thread's ID
        12 => {
            if memory::user_range_ok(memory::current_address_space(), a, 1, false) {
                sched::spawn_thread(a, b, c).map_or(ERROR, |tid| tid as u64)
            } else {
                ERROR
            }
        }
        // thread_exit: end the calling thread only
        13 => {
            close_all(sched::exit_thread());
            return sched::schedule(frame, false);
        }
        // thread_join(tid): wait until that thread has finished
        14 => {
            if sched::thread_alive(a as u32) && a as u32 != sched::current_id() {
                retry!(Wait::Exit);
            }
            0
        }
        // gettid
        15 => sched::current_id() as u64,
        // cpu_count: how many cores the kernel is using
        16 => smp::online() as u64,
        // fd_write: may write less than asked when a pipe fills up
        17 => match user_bytes(b, c) {
            Some(bytes) => settle!(fd_write(a, &bytes)),
            None => ERROR,
        },
        // fd_read: 0 means end of input
        18 => match fd_read(a, c.min(64 * 1024) as usize, false) {
            Ok(data) => {
                if copy_to_user(b, &data) { data.len() as u64 } else { ERROR }
            }
            Err(outcome) => settle!(outcome),
        },
        // pipe: stores the read end's and write end's descriptor numbers
        19 => match sched::current_process() {
            Some(process) => {
                let (read_end, write_end) = Pipe::create();
                let mut fds = process.fds.lock();
                let mut numbers = [0i32; 2];
                for (i, end) in [read_end, write_end].into_iter().enumerate() {
                    let slot = fds.iter().position(|s| s.is_none()).unwrap_or_else(|| {
                        fds.push(None);
                        fds.len() - 1
                    });
                    fds[slot] = Some(end);
                    numbers[i] = slot as i32;
                }
                let bytes = [numbers[0].to_le_bytes(), numbers[1].to_le_bytes()].concat();
                if copy_to_user(a, &bytes) && fds.len() <= 64 {
                    0
                } else {
                    let closed = [fds[numbers[0] as usize].take(), fds[numbers[1] as usize].take()];
                    drop(fds);
                    drop(closed);
                    ERROR
                }
            }
            None => ERROR,
        },
        // close
        20 => match sched::current_process() {
            Some(process) => {
                let closed = process.fds.lock().get_mut(a as usize).and_then(|slot| slot.take());
                if closed.is_some() { 0 } else { ERROR }
            }
            None => ERROR,
        },
        // spawn: start another program; returns its process ID
        21 => match user_string(a, 256) {
            Some(cmdline) => spawn(&cmdline, b as i64, c as i64, -1),
            None => ERROR,
        },
        // wait: until a process has finished; returns its exit code, or -1
        // if it was killed. "Finished" means completely gone: its files
        // written out and its memory reclaimed, which is done right here
        // rather than left for an idle moment.
        22 => {
            if a as u32 != sched::current_pid() {
                sched::reap();
                if sched::exists(a as u32) {
                    retry!(Wait::Exit);
                }
            }
            sched::exit_code(a as u32).map_or(ERROR, |code| code as u64)
        }
        // shm_open: map the named shared memory object (creating it with
        // `size` bytes if new); returns its address in this process
        23 => match (user_name(a), sched::current_process()) {
            (Some(name), Some(process)) => match shm::open(&name, b) {
                Some(object) => {
                    use core::sync::atomic::Ordering;
                    let address = process.shm_next.fetch_add(object.size() + PAGE, Ordering::Relaxed);
                    if shm::map(&object, process.space, address) {
                        process.shared.lock().push((address, object));
                        address
                    } else {
                        ERROR
                    }
                }
                None => ERROR,
            },
            _ => ERROR,
        },
        // shm_unlink
        24 => match user_name(a) {
            Some(name) if shm::unlink(&name) => 0,
            _ => ERROR,
        },
        // file_list: the index-th entry of the working directory. Writes the
        // entry's name; returns its size, or -2 for a directory, or -1 past
        // the end.
        25 => match fs::list(&cwd()).get(a as usize) {
            Some(entry) => {
                let mut bytes = entry.name.as_bytes().to_vec();
                bytes.truncate((c as usize).saturating_sub(1));
                bytes.push(0);
                if c == 0 || !copy_to_user(b, &bytes) {
                    ERROR
                } else if entry.is_dir {
                    (-2i64) as u64
                } else {
                    entry.size as u64
                }
            }
            None => ERROR,
        },
        // file_delete (files only; directories use rmdir)
        26 => match resolve_user(a) {
            Some(key) if fs::remove(&key) => 0,
            _ => ERROR,
        },
        // console_key: wait for a key from the keyboard itself, even when
        // standard input is a pipe or a file (what a pager needs)
        27 => match terminal() {
            Term::Console => match input::pop() {
                Some(ch) => ch as u64,
                None => retry!(Wait::Input),
            },
            Term::Pty(pty) => {
                let mut key = [0u8; 1];
                match pty.slave_read_raw(&mut key) {
                    Transfer::Done(_) => key[0] as u64,
                    Transfer::WouldBlock => retry!(pty.channel()),
                    Transfer::Closed => ERROR,
                }
            }
        },
        // net_resolve: host name (or dotted address) -> 4-byte address
        28 => match user_string(a, 256) {
            Some(name) => match net::lookup(&name) {
                net::Lookup::Found(ip) => {
                    if copy_to_user(b, &ip) { 0 } else { ERROR }
                }
                net::Lookup::Pending => retry!(Wait::Net),
                net::Lookup::Failed => ERROR,
            },
            None => ERROR,
        },
        // tcp_open: begin connecting; returns a descriptor at once
        29 => match user_ip(a).and_then(|ip| net::tcp_open(ip, b as u16)) {
            Some(socket) => install(Fd::Socket(Arc::new(socket))),
            None => ERROR,
        },
        // tcp_wait: until that connection is established (0) or has failed (-1)
        30 => match socket_of(a).map(net::tcp_connected) {
            Some(net::Connect::Connected) => 0,
            Some(net::Connect::Connecting) => retry!(Wait::Net),
            _ => ERROR,
        },
        // tcp_listen: accept connections on a port
        31 => match net::tcp_listen(a as u16) {
            Some(socket) => install(Fd::Socket(Arc::new(socket))),
            None => ERROR,
        },
        // tcp_accept: wait for an incoming connection; returns its descriptor
        32 => match socket_of(a).map(net::tcp_accept) {
            Some(Ok(Some(socket))) => install(Fd::Socket(Arc::new(socket))),
            Some(Ok(None)) => retry!(Wait::Net),
            _ => ERROR,
        },
        // udp_open: a UDP port (0 = any free one)
        33 => match net::udp_open(a as u16) {
            Some(socket) => install(Fd::Socket(Arc::new(socket))),
            None => ERROR,
        },
        // udp_sendto(fd, ip, port, buf, len)
        34 => match (socket_of(a), user_ip(b), user_bytes(d, e.min(2048))) {
            (Some(socket), Some(ip), Some(data)) => match net::udp_send(socket, ip, c as u16, &data) {
                net::Io::Done(n) => n as u64,
                net::Io::WouldBlock => retry!(Wait::Net),
                net::Io::Closed => ERROR,
            },
            _ => ERROR,
        },
        // udp_recvfrom(fd, buf, max, from_out): from_out gets 4 address
        // bytes then the port (2 bytes, little-endian); it may be null
        35 => match socket_of(a).map(net::udp_receive) {
            Some(Ok(Some((ip, port, data)))) => {
                let n = data.len().min(c as usize);
                let mut from = ip.to_vec();
                from.extend_from_slice(&port.to_le_bytes());
                if copy_to_user(b, &data[..n]) && (d == 0 || copy_to_user(d, &from)) { n as u64 } else { ERROR }
            }
            Some(Ok(None)) => retry!(Wait::Net),
            _ => ERROR,
        },
        // net_info: 16 bytes - address, mask, gateway, DNS server
        36 => match net::config() {
            Some(config) => {
                if copy_to_user(a, &config.concat()) { 0 } else { ERROR }
            }
            None => ERROR,
        },
        // tcp_shutdown: finish sending, keep receiving
        37 => match socket_of(a) {
            Some(socket) if net::tcp_shutdown(socket) => 0,
            _ => ERROR,
        },
        // recv_nb(fd, buf, max): like read, but when there is nothing to
        // read yet it returns -2 at once instead of waiting. This lets a
        // program enforce its own timeout on a slow or silent peer.
        38 => match fd_read(a, c.min(64 * 1024) as usize, false) {
            Ok(data) => {
                if copy_to_user(b, &data) { data.len() as u64 } else { ERROR }
            }
            Err(Outcome::Retry(_)) => (-2i64) as u64,
            Err(other) => settle!(other),
        },
        // getcwd: the working directory as a path like "/docs"
        39 => {
            let path = fs::display_path(&cwd());
            if copy_to_user(a, &[path.as_bytes(), &[0]].concat()) { 0 } else { ERROR }
        }
        // chdir: change the working directory (must be an existing directory)
        40 => match (resolve_user(a), sched::current_process()) {
            (Some(key), Some(process)) if fs::is_dir(&key) => {
                *process.cwd.lock() = key;
                0
            }
            _ => fail("no such directory"),
        },
        // mkdir
        41 => match resolve_user(a).map(|key| fs::make_dir(&key)) {
            Some(Ok(())) => 0,
            Some(Err(reason)) => fail(reason),
            None => fail("bad directory name"),
        },
        // rmdir(path, recursive)
        42 => match resolve_user(a).map(|key| fs::remove_dir(&key, b != 0)) {
            Some(Ok(())) => 0,
            Some(Err(reason)) => fail(reason),
            None => fail("bad directory name"),
        },
        // signal(handler): call handler(signal_number) instead of ending
        // the process when a signal arrives; 0 goes back to that default
        43 => {
            if a == 0 || memory::user_range_ok(memory::current_address_space(), a, 1, false) {
                sched::set_signal_handler(a);
                0
            } else {
                ERROR
            }
        }
        // sigreturn: a signal handler has finished (reached through the
        // return address the kernel gave it). Restores every register, so
        // nothing is "returned".
        44 => {
            if sched::signal_return(frame) {
                return frame;
            }
            ERROR
        }
        // kill(pid, signal): 2 interrupt, 15 terminate (both can be handled
        // by the target), 9 kill (cannot)
        45 => {
            let result = if sched::send_signal(a as u32, b as u32).is_ok() { 0 } else { ERROR };
            // The caller may just have ended itself.
            f.rax = result;
            return sched::schedule(frame, false);
        }
        // getenv: the variable's value; returns its length, -1 if unset
        46 => match (user_name(a), sched::current_process()) {
            (Some(name), Some(process)) => {
                let value = exec::env_get(&process.env.lock(), &name).map(String::from);
                value.map_or(ERROR, |value| string_to_user(&value, b, c))
            }
            _ => ERROR,
        },
        // setenv(name, value): for this process and the programs it starts
        // from now on. A null value removes the variable.
        47 => match (user_name(a), sched::current_process()) {
            (Some(name), Some(process)) if !name.contains('=') => {
                let value = if b == 0 { None } else { Some(user_string(b, 1024).unwrap_or_default()) };
                let mut env = process.env.lock();
                env.retain(|(n, _)| *n != name);
                match value {
                    Some(value) if env.len() < 128 => {
                        env.push((name, value));
                        0
                    }
                    Some(_) => ERROR,
                    None => 0,
                }
            }
            _ => ERROR,
        },
        // env_list: the index-th variable as "NAME=value"; -1 past the end
        48 => match sched::current_process() {
            Some(process) => {
                let entry = process.env.lock().get(a as usize).map(|(n, v)| alloc::format!("{}={}", n, v));
                entry.map_or(ERROR, |entry| string_to_user(&entry, b, c))
            }
            None => ERROR,
        },
        // time: seconds since 1970-01-01, from the hardware clock
        49 => clock::now(),
        // file_stat(path, out): out gets the size and the last-modified
        // time (two 64-bit values). Returns 0 for a file, 1 for a
        // directory, -1 if there is nothing there.
        50 => match resolve_user(a).and_then(|key| fs::stat(&key)) {
            Some((is_dir, size, modified)) => {
                let bytes = [(size as u64).to_le_bytes(), modified.to_le_bytes()].concat();
                if copy_to_user(b, &bytes) { is_dir as u64 } else { ERROR }
            }
            None => ERROR,
        },
        // proc_list: the index-th task (see `task_record`); -1 past the end
        51 => match sched::list().get(a as usize) {
            Some(task) => {
                if copy_to_user(b, &task_record(task)) { 0 } else { ERROR }
            }
            None => ERROR,
        },
        // sysinfo (see `system_record`)
        52 => {
            if copy_to_user(a, &system_record()) { 0 } else { ERROR }
        }
        // power: 0 shuts the machine down, 1 restarts it
        53 => match a {
            0 => arch::shutdown(),
            1 => arch::reboot(),
            _ => ERROR,
        },
        // isatty: is this descriptor the console (rather than a pipe, a
        // file or a socket)?
        54 => match sched::current_process() {
            Some(process) => {
                let fds = process.fds.lock();
                fds.get(a as usize).and_then(|slot| slot.as_ref()).map_or(ERROR, |fd| fd.is_terminal() as u64)
            }
            None => ERROR,
        },
        // foreground(pids, count): the processes that Ctrl+C at the caller's
        // terminal is sent to (a shell names the pipeline it is waiting for;
        // count 0 means none)
        55 => {
            let pids: Vec<u32> = if b == 0 {
                Vec::new()
            } else {
                match user_bytes(a, b.min(32) * 4) {
                    Some(bytes) => {
                        bytes.chunks_exact(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect()
                    }
                    None => return { f.rax = ERROR; frame },
                }
            };
            match terminal() {
                Term::Console => sched::set_foreground(&pids),
                Term::Pty(pty) => pty.set_foreground(&pids),
            }
            0
        }
        // open(path, mode): a descriptor for reading a file (0), or for
        // writing one from scratch (1) or adding to it (2). What is written
        // becomes the file when the last copy of the descriptor is closed.
        56 => match resolve_user(a) {
            Some(key) if b == 0 => match Fd::open_read(&key) {
                Some(fd) => install(fd),
                None => fail("no such file"),
            },
            Some(key) if b <= 2 => {
                if fs::is_dir(&key) {
                    fail("that is a directory")
                } else {
                    install(Fd::open_write(&key, b == 2))
                }
            }
            _ => fail("bad file name"),
        },
        // last_error: text saying why this process's last failed call failed
        57 => match sched::current_process() {
            Some(process) => {
                let reason = *process.last_error.lock();
                string_to_user(reason, a, b)
            }
            None => ERROR,
        },
        // debug: hooks for testing the kernel itself. 0 starts a kernel
        // worker thread, 1 a kernel thread that faults, 2 panics the kernel.
        58 => match a {
            0 => crate::shell::start_worker() as u64,
            1 => crate::shell::start_crasher() as u64,
            2 => panic!("panic requested by process {}", sched::current_pid()),
            _ => ERROR,
        },
        // ping_send(ip, sequence): send one ICMP echo request. 0 sent,
        // -1 no adapter, -2 no address yet, -3 still finding the next hop
        // (try again shortly).
        59 => match user_ip(a) {
            Some(ip) => match net::ping_try(ip, b as u16) {
                Ok(true) => 0,
                Ok(false) => (-3i64) as u64,
                Err(net::PingError::NoAdapter) => ERROR,
                Err(_) => (-2i64) as u64,
            },
            None => ERROR,
        },
        // ping_reply(sequence): uptime in ms at which the answer to that
        // request arrived, or -1 if it has not
        60 => net::ping_reply(a as u16).map_or(ERROR, |tick| tick * 1000 / TICK_HZ),
        // poll(fds, count, timeout_ms): fds is an array of {int fd; short
        // events; short revents}. Waits until at least one of them is ready
        // (or timeout_ms passes; -1 = no limit), fills in revents and
        // returns how many are ready (0: timed out).
        61 => {
            const POLLIN: u16 = 1;
            const POLLOUT: u16 = 4;
            const POLLHUP: u16 = 16;
            const POLLNVAL: u16 = 32;
            let count = b.min(64);
            let Some(mut entries) = user_bytes(a, count * 8) else { return { f.rax = ERROR; frame } };
            let deadline = if (c as i64) < 0 {
                u64::MAX
            } else if resumed {
                sched::call_deadline()
            } else {
                let d = arch::ticks() + sched::ms_to_ticks(c);
                sched::set_call_deadline(d);
                d
            };
            let Some(process) = sched::current_process() else { return { f.rax = ERROR; frame } };
            let (mut ready, mut sockets) = (0u64, false);
            {
                let fds = process.fds.lock();
                for entry in entries.chunks_exact_mut(8) {
                    let fd = i32::from_le_bytes([entry[0], entry[1], entry[2], entry[3]]);
                    let wanted = u16::from_le_bytes([entry[4], entry[5]]);
                    let found = if fd < 0 { None } else { fds.get(fd as usize).and_then(|slot| slot.as_ref()) };
                    let events = match found {
                        None if fd < 0 => 0, // a slot the caller is not using
                        None => POLLNVAL,
                        Some(descriptor) => {
                            sockets |= matches!(descriptor, Fd::Socket(_));
                            let (readable, writable, hung_up) = descriptor.readiness();
                            let mut got = 0;
                            if readable {
                                got |= POLLIN;
                            }
                            if writable {
                                got |= POLLOUT;
                            }
                            if hung_up {
                                got |= POLLHUP;
                            }
                            got & (wanted | POLLHUP)
                        }
                    };
                    entry[6..8].copy_from_slice(&events.to_le_bytes());
                    if events != 0 {
                        ready += 1;
                    }
                }
            }
            if ready == 0 && arch::ticks() < deadline {
                // Nothing yet: wait for anything to change, then look again.
                // Sockets also need the passing of time watched.
                let recheck = arch::ticks() + if sockets { 10 } else { 100 };
                f.rip -= 2;
                sched::block_until(Wait::Poll, seen, recheck.min(deadline));
                return sched::schedule(frame, false);
            }
            if copy_to_user(a, &entries) { ready } else { ERROR }
        }
        // pty_open(int fds[2]): a new pseudo-terminal. fds[0] is the
        // terminal end, fds[1] the end to give a program.
        62 => {
            let (master, slave) = Pty::create();
            let m = install(master);
            let s = install(slave);
            let bytes = [(m as i32).to_le_bytes(), (s as i32).to_le_bytes()].concat();
            if m != ERROR && s != ERROR && copy_to_user(a, &bytes) {
                0
            } else {
                if let Some(process) = sched::current_process() {
                    let closed: Vec<Option<Fd>> = {
                        let mut fds = process.fds.lock();
                        [m, s].iter().filter(|&&n| n != ERROR).map(|&n| fds[n as usize].take()).collect()
                    };
                    drop(closed);
                }
                ERROR
            }
        }
        // pty_resize(fd, cols, rows): the size programs in it are told
        63 => match sched::current_process() {
            Some(process) => {
                let pty = match process.fds.lock().get(a as usize).and_then(|slot| slot.as_ref()) {
                    Some(Fd::PtyMaster(pty)) | Some(Fd::PtySlave(pty)) => Some(pty.clone()),
                    _ => None,
                };
                match pty {
                    Some(pty) => {
                        pty.set_size(b as u16, c as u16);
                        0
                    }
                    None => ERROR,
                }
            }
            None => ERROR,
        },
        // display_open(info): take over the screen. The framebuffer is
        // mapped into the caller, `info` gets its address and layout (see
        // display::Info), and the returned descriptor delivers key and
        // mouse events. Closing it (or exiting) gives the screen back.
        65 => match sched::current_process() {
            Some(process) => match crate::display::acquire(sched::current_pid(), process.space) {
                Ok(info) => {
                    let mut bytes = alloc::vec::Vec::with_capacity(32);
                    bytes.extend_from_slice(&info.address.to_le_bytes());
                    for value in [info.width, info.height, info.stride, info.format] {
                        bytes.extend_from_slice(&value.to_le_bytes());
                    }
                    bytes.extend_from_slice(&info.x.to_le_bytes());
                    bytes.extend_from_slice(&info.y.to_le_bytes());
                    let fd = if copy_to_user(a, &bytes) {
                        install(Fd::Display(Arc::new(crate::display::Handle)))
                    } else {
                        ERROR
                    };
                    if fd == ERROR {
                        crate::display::release();
                    }
                    fd
                }
                Err(reason) => fail(reason),
            },
            None => ERROR,
        },
        // local_listen(name): be the server for that name; returns a
        // descriptor to accept connections on
        66 => match user_name(a).map(|name| crate::local::listen(&name)) {
            Some(Ok(listener)) => install(Fd::Listener(listener)),
            Some(Err(reason)) => fail(reason),
            None => fail("bad name"),
        },
        // local_connect(name): connect to the server for that name
        67 => match user_name(a).map(|name| crate::local::connect(&name)) {
            Some(Ok(stream)) => install(stream),
            Some(Err(reason)) => fail(reason),
            None => fail("bad name"),
        },
        // local_accept(fd): wait for the next client; returns its descriptor
        68 => {
            let listener = sched::current_process().and_then(|process| {
                match process.fds.lock().get(a as usize).and_then(|slot| slot.as_ref()) {
                    Some(Fd::Listener(listener)) => Some(listener.clone()),
                    _ => None,
                }
            });
            match listener {
                Some(listener) => match listener.accept() {
                    Some(stream) => install(stream),
                    None => retry!(listener.channel()),
                },
                None => fail("not a listening descriptor"),
            }
        }
        // shm_close(address): unmap a shared memory object mapped with
        // shm_open. It is freed once nobody has it and its name is unlinked.
        69 => match sched::current_process() {
            Some(process) => {
                let found = {
                    let mut shared = process.shared.lock();
                    shared.iter().position(|(at, _)| *at == a).map(|i| shared.remove(i))
                };
                match found {
                    Some((address, object)) => {
                        shm::unmap(&object, process.space, address);
                        0
                    }
                    None => fail("nothing is mapped there"),
                }
            }
            None => ERROR,
        },
        // pty_foreground(fd, pids, count): for a terminal program that runs
        // something without a shell in between, which would normally do
        // this: the processes Ctrl+C typed at the pty, and hanging it up,
        // are sent to. `fd` is either end of the pty.
        70 => {
            let pty = sched::current_process().and_then(|process| {
                match process.fds.lock().get(a as usize).and_then(|slot| slot.as_ref()) {
                    Some(Fd::PtyMaster(pty)) | Some(Fd::PtySlave(pty)) => Some(pty.clone()),
                    _ => None,
                }
            });
            match (pty, user_bytes(b, c.min(32) * 4)) {
                (Some(pty), Some(bytes)) => {
                    let pids: Vec<u32> =
                        bytes.chunks_exact(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect();
                    pty.set_foreground(&pids);
                    0
                }
                _ => fail("not a pseudo-terminal"),
            }
        }
        // getrandom(buf, len): random bytes suitable for encryption keys
        71 => {
            let mut bytes = alloc::vec![0u8; b.min(4096) as usize];
            crate::random::fill(&mut bytes);
            if copy_to_user(a, &bytes) { bytes.len() as u64 } else { ERROR }
        }
        // spawn_fds(cmdline, in, out, err): spawn, also choosing standard error
        64 => match user_string(a, 256) {
            Some(cmdline) => spawn(&cmdline, b as i64, c as i64, d as i64),
            None => ERROR,
        },
        _ => ERROR,
    };
    frame
}
