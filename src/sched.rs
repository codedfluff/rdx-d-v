//! Preemptive scheduler for kernel threads and user processes, on one or
//! many CPUs.
//!
//! Every task owns a kernel stack. When an interrupt arrives, the entry stub
//! saves the full CPU state onto that stack and passes a pointer to it here;
//! `schedule` remembers that pointer and returns another task's saved
//! pointer, which the stub then restores. That is the whole context switch.
//!
//! A user process is one or more such tasks (its threads) sharing an address
//! space. A thread's saved frame returns to ring 3, and the CPU switches to
//! the thread's kernel stack (via the TSS) whenever an interrupt or system
//! call pulls it back into the kernel.
//!
//! All CPUs share one list of tasks. Each CPU's timer interrupt calls
//! `schedule`, which picks the next ready task that no other CPU is running.
//! The scheduler lock stays held until the CPU has actually left the old
//! task's stack (see `finish_switch`), so two CPUs can never be on one stack.

use alloc::boxed::Box;
use alloc::string::String;
use alloc::sync::Arc;
use alloc::vec;
use alloc::vec::Vec;
use core::arch::asm;
use core::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

use crate::arch::{self, Frame, FX_AREA, KERNEL_CS, KERNEL_DS, MAX_CPUS, TICK_HZ, USER_CS, USER_DS};
use crate::memory::{self, PAGE, USER_BASE};
use crate::pipe::FdTable;
use crate::shm::Shm;
use crate::sync::IrqLock;

const STACK_SIZE: usize = 64 * 1024;

/// Thread stacks sit below this address, 1 MiB apart, 256 KiB each.
const THREAD_STACK_TOP: u64 = USER_BASE + 0x7000_0000;
const THREAD_STACK_SPACING: u64 = 0x10_0000;
const THREAD_STACK_PAGES: u64 = 64;
const MAX_THREADS: u64 = 256;

#[derive(Clone, Copy, PartialEq)]
pub enum State {
    Ready,
    Running,
    Sleeping,
    /// Waiting for something to happen (see `Wait`); woken by `wake`.
    Blocked,
    Dead,
}

/// What a blocked task is waiting for. Whoever makes that thing happen
/// calls `wake` with the same value.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Wait {
    /// A key from the keyboard or serial port.
    Input,
    /// Data, room or a hang-up on one particular pipe (its address).
    Pipe(usize),
    /// Some process or thread finishing.
    Exit,
    /// Anything changing in the network stack.
    Net,
    /// Several things at once (the poll system call): woken by every wake.
    Poll,
}

/// Signals a process can be sent. A process with a handler installed gets
/// to run it; otherwise (and always for KILL) the process is ended.
pub const SIGHUP: u32 = 1;
pub const SIGINT: u32 = 2;
pub const SIGKILL: u32 = 9;
pub const SIGTERM: u32 = 15;

/// A blocked task is looked at again after this long even if nobody woke
/// it: a safety net, not the mechanism.
const BLOCK_RECHECK_TICKS: u64 = 100;
/// Network waits are rechecked more often, because some of what they wait
/// for is the passing of time (retransmissions, timeouts) rather than an
/// arriving packet.
const NET_RECHECK_TICKS: u64 = 10;

/// Counts every `wake`. A task about to block passes the value it read
/// before checking its condition; if the count has moved since, something
/// happened in between and it must check again instead of sleeping.
static EVENTS: AtomicU64 = AtomicU64::new(0);

/// How many signals are waiting to be delivered, system-wide. Lets the
/// interrupt exit path skip the scheduler lock when there are none.
static SIGNALS_PENDING: AtomicU32 = AtomicU32::new(0);

/// Per CPU: its local APIC ID, for sending it interrupts.
static APIC_IDS: [AtomicU32; MAX_CPUS] = [const { AtomicU32::new(0) }; MAX_CPUS];
/// Per CPU: a reschedule interrupt is already on its way.
static KICKED: [AtomicBool; MAX_CPUS] = [const { AtomicBool::new(false) }; MAX_CPUS];
static CPUS: AtomicU32 = AtomicU32::new(1);

/// Register state put aside while a signal handler runs.
struct SavedContext {
    frame: Frame,
    fx: [u8; FX_AREA],
}

/// What the threads of one process share.
pub struct Process {
    /// Address space (physical address of the top-level page table).
    pub space: u64,
    /// Current end of the heap, grown with the sbrk system call.
    pub brk: IrqLock<u64>,
    /// Open file descriptors: standard input/output, pipes, files.
    pub fds: IrqLock<FdTable>,
    /// Shared memory objects mapped into this process (kept alive by it).
    pub shared: IrqLock<Vec<(u64, Arc<Shm>)>>,
    /// Where the next shared memory mapping will go.
    pub shm_next: AtomicU64,
    /// Current working directory, as a filesystem key (root is the empty
    /// string). File paths from this process are resolved against it.
    pub cwd: IrqLock<String>,
    /// Environment variables, inherited by the programs it starts.
    pub env: IrqLock<Vec<(String, String)>>,
    /// Address of the function to call when a signal arrives (0: none, so
    /// signals end the process).
    pub signal_handler: AtomicU64,
    /// Why the process's last failed system call failed, for error messages.
    pub last_error: IrqLock<&'static str>,
    threads_created: AtomicU64,
}

impl Drop for Process {
    fn drop(&mut self) {
        // Runs when the last thread has been reaped, so no CPU is using it.
        memory::free_address_space(self.space);
    }
}

struct Task {
    /// Unique per task: the thread ID.
    id: u32,
    /// The ID of the process's first thread; shared by all its threads.
    pid: u32,
    name: String,
    state: State,
    frame: usize,
    wake_at: u64,
    /// What a Blocked task is waiting for.
    waiting: Option<Wait>,
    /// Signals not yet delivered (bit n = signal n).
    signals: u32,
    /// The interrupted state, while this task is inside a signal handler.
    in_handler: Option<Box<SavedContext>>,
    /// The task's last system call could not finish and is set to run
    /// again (see `block_current`). Cleared when the next call starts.
    retrying: bool,
    /// When a waiting system call with a time limit gives up (in ticks).
    deadline: u64,
    cpu_ticks: u64,
    /// When the task was created, in ticks since boot.
    started: u64,
    /// None for kernel threads, which all share the kernel's address space.
    process: Option<Arc<Process>>,
    /// The CPU this task is on right now, if any.
    on_cpu: Option<usize>,
    /// Per-CPU idle tasks run only when nothing else can.
    idle: bool,
    kstack_top: u64,
    _kstack: Vec<u8>,
}

struct Scheduler {
    tasks: Vec<Task>,
    current: [u32; MAX_CPUS],
    idle: [u32; MAX_CPUS],
    next_id: u32,
}

impl Scheduler {
    fn index_of(&self, id: u32) -> Option<usize> {
        self.tasks.iter().position(|t| t.id == id)
    }

    /// The task running on the calling CPU.
    fn current(&mut self) -> &mut Task {
        let id = self.current[arch::cpu_id()];
        let i = self.index_of(id).expect("current task vanished");
        &mut self.tasks[i]
    }

    /// Mark every live thread of a process dead. Returns how many there were.
    fn kill_process(&mut self, pid: u32) -> usize {
        let mut count = 0;
        for t in self.tasks.iter_mut() {
            if t.pid == pid && t.state != State::Dead && !t.idle {
                t.state = State::Dead;
                count += 1;
            }
        }
        if count > 0 {
            self.wake(Wait::Exit);
        }
        count
    }

    /// Make every task blocked on `on` runnable again.
    fn wake(&mut self, on: Wait) {
        EVENTS.fetch_add(1, Ordering::SeqCst);
        let mut woke = 0;
        for t in self.tasks.iter_mut() {
            if t.state == State::Blocked && (t.waiting == Some(on) || t.waiting == Some(Wait::Poll)) {
                t.state = State::Ready;
                t.waiting = None;
                woke += 1;
            }
        }
        self.kick_idle(woke);
    }

    /// Up to `count` tasks just became runnable: nudge that many idle CPUs
    /// so they pick them up now rather than at their next timer tick.
    fn kick_idle(&mut self, mut count: usize) {
        let me = arch::cpu_id();
        let cpus = CPUS.load(Ordering::Relaxed) as usize;
        for cpu in 0..cpus {
            if count == 0 {
                break;
            }
            if cpu != me && self.current[cpu] == self.idle[cpu] && !KICKED[cpu].swap(true, Ordering::Relaxed) {
                crate::timer::send_ipi(APIC_IDS[cpu].load(Ordering::Relaxed), crate::timer::VEC_RESCHEDULE as u32);
                count -= 1;
            }
        }
    }

    /// Send a signal to a process. Returns false if there is no such live
    /// user process.
    fn signal(&mut self, pid: u32, signal: u32) -> bool {
        let Some(process) =
            self.tasks.iter().find(|t| t.pid == pid && t.state != State::Dead).and_then(|t| t.process.clone())
        else {
            return false;
        };
        if signal == SIGKILL || process.signal_handler.load(Ordering::Relaxed) == 0 {
            self.kill_process(pid);
            return true;
        }
        // The process's first thread handles it if it is still around.
        let main = self.tasks.iter().position(|t| t.pid == pid && t.state != State::Dead && t.id == pid);
        let any = self.tasks.iter().position(|t| t.pid == pid && t.state != State::Dead);
        if let Some(i) = main.or(any) {
            let t = &mut self.tasks[i];
            if t.signals & (1 << signal) == 0 {
                t.signals |= 1 << signal;
                SIGNALS_PENDING.fetch_add(1, Ordering::SeqCst);
            }
            // Interrupt whatever it was waiting for.
            if matches!(t.state, State::Blocked | State::Sleeping) {
                t.state = State::Ready;
                t.waiting = None;
                self.kick_idle(1);
            }
        }
        true
    }
}

static SCHED: IrqLock<Option<Scheduler>> = IrqLock::new(None);

const NOT_PENDING: AtomicBool = AtomicBool::new(false);
/// Per CPU: `schedule` left the lock held; `finish_switch` must release it.
static UNLOCK_PENDING: [AtomicBool; MAX_CPUS] = [NOT_PENDING; MAX_CPUS];

/// The processes that own the keyboard (one pipeline): Ctrl+C kills them.
static FOREGROUND: IrqLock<Vec<u32>> = IrqLock::new(Vec::new());
/// (process id, exit code) of recently finished processes, oldest first.
static EXIT_CODES: IrqLock<Vec<(u32, i64)>> = IrqLock::new(Vec::new());

fn record_exit(pid: u32, code: i64) {
    let mut codes = EXIT_CODES.lock();
    if codes.len() >= 64 {
        codes.remove(0);
    }
    codes.push((pid, code));
}

fn idle_task(id: u32, cpu: usize) -> Task {
    Task {
        id,
        pid: id,
        name: alloc::format!("idle/{}", cpu),
        state: State::Running,
        frame: 0,
        wake_at: 0,
        waiting: None,
        signals: 0,
        in_handler: None,
        retrying: false,
        deadline: 0,
        cpu_ticks: 0,
        started: arch::ticks(),
        process: None,
        on_cpu: Some(cpu),
        idle: true,
        kstack_top: 0, // runs on its boot stack and never enters ring 3
        _kstack: Vec::new(),
    }
}

/// Turn the currently running boot code into task 0, CPU 0's idle task.
pub fn init() {
    APIC_IDS[0].store(crate::timer::local_id(), Ordering::Relaxed);
    *SCHED.lock() =
        Some(Scheduler { tasks: vec![idle_task(0, 0)], current: [0; MAX_CPUS], idle: [0; MAX_CPUS], next_id: 1 });
}

/// The same for a secondary CPU: its start-up code becomes its idle task.
pub fn adopt_idle() {
    let mut guard = SCHED.lock();
    let s = guard.as_mut().expect("scheduler not initialised");
    let cpu = arch::cpu_id();
    let id = s.next_id;
    s.next_id += 1;
    s.tasks.push(idle_task(id, cpu));
    s.current[cpu] = id;
    s.idle[cpu] = id;
    APIC_IDS[cpu].store(crate::timer::local_id(), Ordering::Relaxed);
    CPUS.fetch_max(cpu as u32 + 1, Ordering::Relaxed);
}

extern "sysv64" fn task_entry(entry: usize) -> ! {
    let entry: fn() = unsafe { core::mem::transmute(entry) };
    entry();
    exit()
}

/// Allocate a kernel stack and lay out an initial saved frame on it, so the
/// first "return" into this task starts it running at `template.rip`.
/// `pid` 0 means "a new process whose ID is this task's ID".
fn create(name: &str, pid: u32, process: Option<Arc<Process>>, template: Frame) -> u32 {
    let mut stack = vec![0u8; STACK_SIZE];
    let top = (stack.as_mut_ptr() as usize + STACK_SIZE) & !0xF;
    let frame = (top - core::mem::size_of::<Frame>()) as *mut Frame;
    unsafe {
        // The stub restores FPU/SSE state from just below the frame, so give
        // the new task a sane one: default x87 control word and MXCSR.
        let fx = (frame as usize - FX_AREA) as *mut u8;
        (fx as *mut u16).write_unaligned(0x037F);
        (fx.add(24) as *mut u32).write_unaligned(0x1F80);
        let mut f = template;
        if process.is_none() {
            f.rsp = (top - 8) as u64; // kernel threads run on this same stack
        }
        frame.write(f);
    }

    let mut guard = SCHED.lock();
    let s = guard.as_mut().expect("scheduler not initialised");
    let id = s.next_id;
    s.next_id += 1;
    s.tasks.push(Task {
        id,
        pid: if pid == 0 { id } else { pid },
        name: String::from(name),
        state: State::Ready,
        frame: frame as usize,
        wake_at: 0,
        waiting: None,
        signals: 0,
        in_handler: None,
        retrying: false,
        deadline: 0,
        cpu_ticks: 0,
        started: arch::ticks(),
        process,
        on_cpu: None,
        idle: false,
        kstack_top: top as u64,
        _kstack: stack,
    });
    id
}

/// Start a kernel thread.
pub fn spawn(name: &str, entry: fn()) -> u32 {
    let frame = Frame {
        rdi: entry as usize as u64,
        rip: task_entry as *const () as u64,
        cs: KERNEL_CS,
        rflags: 0x202, // interrupts enabled
        ss: KERNEL_DS,
        ..Frame::default()
    };
    create(name, 0, None, frame)
}

fn user_frame(entry: u64, stack: u64, arg1: u64, arg2: u64) -> Frame {
    Frame {
        rcx: arg1, // first two arguments in the Microsoft x64 calling convention
        rdx: arg2,
        rip: entry,
        cs: USER_CS,
        rflags: 0x202,
        rsp: stack,
        ss: USER_DS,
        ..Frame::default()
    }
}

/// Start a new process in the address space `space`, which it now owns.
/// `arg` arrives as the entry point's first argument. Returns the process ID.
#[allow(clippy::too_many_arguments)]
pub fn spawn_process(
    name: &str,
    space: u64,
    entry: u64,
    stack: u64,
    heap_start: u64,
    arg: u64,
    fds: FdTable,
    cwd: String,
    env: Vec<(String, String)>,
) -> u32 {
    let process = Arc::new(Process {
        space,
        brk: IrqLock::new(heap_start),
        fds: IrqLock::new(fds),
        shared: IrqLock::new(Vec::new()),
        shm_next: AtomicU64::new(crate::shm::MAP_BASE),
        cwd: IrqLock::new(cwd),
        env: IrqLock::new(env),
        signal_handler: AtomicU64::new(0),
        last_error: IrqLock::new(""),
        threads_created: AtomicU64::new(0),
    });
    create(name, 0, Some(process), user_frame(entry, stack, arg, 0))
}

/// Start another thread in the calling process, running `entry(arg1, arg2)`
/// on a fresh stack. Returns the new thread's ID.
pub fn spawn_thread(entry: u64, arg1: u64, arg2: u64) -> Option<u32> {
    let (process, pid, name) = {
        let mut guard = SCHED.lock();
        let t = guard.as_mut()?.current();
        (t.process.clone()?, t.pid, t.name.clone())
    };
    let n = process.threads_created.fetch_add(1, Ordering::Relaxed);
    if n >= MAX_THREADS {
        return None;
    }
    let top = THREAD_STACK_TOP - n * THREAD_STACK_SPACING;
    let mut top_page = 0;
    for i in 0..THREAD_STACK_PAGES {
        top_page = memory::map_user_page(process.space, top - (THREAD_STACK_PAGES - i) * PAGE, true)?;
    }
    // Enter like a called function. The arguments go in registers for code
    // built with clang and also on the stack for code built with c-comp,
    // which passes everything there.
    let stack = top - 64 - 8;
    unsafe {
        ((top_page + PAGE - 64) as *mut u64).write(arg1);
        ((top_page + PAGE - 56) as *mut u64).write(arg2);
    }
    Some(create(&name, pid, Some(process), user_frame(entry, stack, arg1, arg2)))
}

/// Called from the interrupt dispatcher. Saves the interrupted task's frame
/// and returns the frame of the task this CPU should run next.
///
/// On return the scheduler lock is STILL HELD: the caller is still standing
/// on the old task's stack. `finish_switch` releases it once that is no
/// longer true.
pub fn schedule(frame: *mut Frame, timer_tick: bool) -> *mut Frame {
    let mut guard = SCHED.lock();
    let Some(s) = guard.as_mut() else { return frame };
    let cpu = arch::cpu_id();
    KICKED[cpu].store(false, Ordering::Relaxed);
    let now = arch::ticks();
    let cur = s.index_of(s.current[cpu]).expect("current task vanished");

    {
        let t = &mut s.tasks[cur];
        t.frame = frame as usize;
        if timer_tick {
            t.cpu_ticks += 1;
        }
        if t.state == State::Running {
            t.state = State::Ready;
        }
        t.on_cpu = None;
    }
    // A finished task has now left its CPU, so its memory can be reclaimed:
    // tell whoever is waiting for exactly that (see the wait system call).
    if s.tasks[cur].state == State::Dead {
        s.wake(Wait::Exit);
    }
    for t in s.tasks.iter_mut() {
        if matches!(t.state, State::Sleeping | State::Blocked) && now >= t.wake_at {
            t.state = State::Ready;
            t.waiting = None;
        }
    }

    // Round-robin: the first ready task after the current one that no CPU
    // is running. Failing that, this CPU's idle task.
    let n = s.tasks.len();
    let mut next = None;
    for i in 1..=n {
        let t = &s.tasks[(cur + i) % n];
        if !t.idle && t.state == State::Ready && t.on_cpu.is_none() {
            next = Some((cur + i) % n);
            break;
        }
    }
    let next = next.unwrap_or_else(|| s.index_of(s.idle[cpu]).expect("idle task vanished"));

    let t = &mut s.tasks[next];
    t.state = State::Running;
    t.on_cpu = Some(cpu);
    if t.kstack_top != 0 {
        arch::set_kernel_stack(t.kstack_top);
    }
    memory::switch_address_space(t.process.as_ref().map_or(memory::kernel_address_space(), |p| p.space));
    let next_frame = t.frame as *mut Frame;
    s.current[cpu] = t.id;

    core::mem::forget(guard);
    UNLOCK_PENDING[cpu].store(true, Ordering::Relaxed);
    next_frame
}

/// Called by the interrupt exit path once the CPU is on the stack of the
/// task it is resuming. Releases the lock `schedule` kept.
pub fn finish_switch() {
    if UNLOCK_PENDING[arch::cpu_id()].swap(false, Ordering::Relaxed) {
        unsafe { SCHED.force_unlock() };
    }
}

pub fn yield_now() {
    unsafe { asm!("int 0x81") };
}

/// Mark the running task as asleep for `ticks` timer ticks. It keeps running
/// until the next call to `schedule`, so follow this with a yield.
pub fn sleep_current(ticks: u64) -> bool {
    let mut guard = SCHED.lock();
    let Some(s) = guard.as_mut() else { return false };
    let t = s.current();
    if t.idle {
        return false; // idle tasks must always stay runnable
    }
    // The task may have been killed from another core while it was on its way
    // here (its own `retry!` loop, say). Never bring a dead task back to life:
    // leave it dead so the scheduler never runs it again.
    if t.state == State::Dead {
        return false;
    }
    t.wake_at = arch::ticks() + ticks.max(1);
    t.state = State::Sleeping;
    true
}

/// How many wake-ups there have been so far. Read this *before* checking
/// whether there is anything to wait for, and hand it to `block_current`.
pub fn events() -> u64 {
    EVENTS.load(Ordering::SeqCst)
}

/// Mark the running task as blocked until `wake(on)` is called. Does nothing
/// (returns false) if a wake-up has happened since the caller read `seen`
/// from `events`: the thing it is waiting for may already be there. Like
/// `sleep_current`, the task keeps running until the next `schedule`.
pub fn block_current(on: Wait, seen: u64) -> bool {
    let recheck = if on == Wait::Net { NET_RECHECK_TICKS } else { BLOCK_RECHECK_TICKS };
    block_until(on, seen, arch::ticks() + recheck)
}

/// Like `block_current`, but look again at tick `until` at the latest.
pub fn block_until(on: Wait, seen: u64, until: u64) -> bool {
    let mut guard = SCHED.lock();
    let Some(s) = guard.as_mut() else { return false };
    let t = s.current();
    if t.idle || t.state == State::Dead {
        return false;
    }
    t.retrying = true;
    // A signal is waiting: go back to user mode so its handler can run.
    if t.signals != 0 && t.in_handler.is_none() {
        return false;
    }
    if EVENTS.load(Ordering::SeqCst) != seen {
        return false;
    }
    t.wake_at = until.max(arch::ticks() + 1);
    t.waiting = Some(on);
    t.state = State::Blocked;
    true
}

/// A system call is starting: whatever the task's previous one was doing,
/// it is no longer waiting to be run again. Returns true if this is that
/// same call being run again (rather than a new one).
pub fn syscall_started() -> bool {
    let mut guard = SCHED.lock();
    guard.as_mut().map_or(false, |s| core::mem::replace(&mut s.current().retrying, false))
}

/// The time limit of the calling task's current system call, in ticks.
pub fn call_deadline() -> u64 {
    let mut guard = SCHED.lock();
    guard.as_mut().map_or(0, |s| s.current().deadline)
}

pub fn set_call_deadline(tick: u64) {
    let mut guard = SCHED.lock();
    if let Some(s) = guard.as_mut() {
        s.current().deadline = tick;
    }
}

/// What a waiting system call returns when a signal handler ran instead of
/// the thing it was waiting for ("interrupted").
pub const EINTR: i64 = -4;

/// Block the calling kernel thread until `wake(on)` (or a wake-up that
/// raced with the caller's check, in which case it returns at once).
pub fn wait_for(on: Wait, seen: u64) {
    if block_current(on, seen) {
        yield_now();
    }
}

/// Make every task blocked on `on` runnable. Safe from interrupt handlers.
pub fn wake(on: Wait) {
    let mut guard = SCHED.lock();
    match guard.as_mut() {
        Some(s) => s.wake(on),
        None => {
            EVENTS.fetch_add(1, Ordering::SeqCst);
        }
    }
}

pub fn ms_to_ticks(ms: u64) -> u64 {
    (ms.saturating_mul(TICK_HZ).saturating_add(999)) / 1000
}

pub fn sleep_ms(ms: u64) {
    if sleep_current(ms_to_ticks(ms)) {
        yield_now();
    }
}

/// End the calling task's whole process (all its threads) and remember the
/// exit code. The caller keeps running until the next call to `schedule`.
///
/// Returns the process, so the caller can close its files and pipes straight
/// away (others may be waiting on them) without holding the scheduler lock.
pub fn exit_current(code: i64) -> Option<Arc<Process>> {
    let mut guard = SCHED.lock();
    let s = guard.as_mut()?;
    let t = s.current();
    if t.idle {
        return None;
    }
    let pid = t.pid;
    let process = t.process.clone();
    s.kill_process(pid);
    record_exit(pid, code);
    process
}

/// End just the calling thread. If it was the process's last one, the
/// process is over (exit code 0) and is returned, as from `exit_current`.
pub fn exit_thread() -> Option<Arc<Process>> {
    let mut guard = SCHED.lock();
    let s = guard.as_mut()?;
    let t = s.current();
    if t.idle {
        return None;
    }
    t.state = State::Dead;
    let pid = t.pid;
    let process = t.process.clone();
    s.wake(Wait::Exit);
    if s.tasks.iter().any(|t| t.pid == pid && t.state != State::Dead) {
        return None; // other threads carry on
    }
    record_exit(pid, 0);
    process
}

/// End the calling kernel thread.
pub fn exit() -> ! {
    let _ = exit_thread();
    loop {
        yield_now();
    }
}

/// Thread ID of the calling task.
pub fn current_id() -> u32 {
    let mut guard = SCHED.lock();
    guard.as_mut().map_or(0, |s| s.current().id)
}

/// Process ID of the calling task.
pub fn current_pid() -> u32 {
    let mut guard = SCHED.lock();
    guard.as_mut().map_or(0, |s| s.current().pid)
}

/// True if the caller is one of the per-CPU idle tasks (or early boot code).
pub fn current_is_idle() -> bool {
    let mut guard = SCHED.lock();
    guard.as_mut().map_or(true, |s| s.current().idle)
}

/// The process the calling task belongs to, if it is a user task.
pub fn current_process() -> Option<Arc<Process>> {
    let mut guard = SCHED.lock();
    guard.as_mut()?.current().process.clone()
}

/// Kill whatever was running when an exception hit: the whole process for a
/// user task, just the thread for a kernel one. Returns its process ID and
/// the frame to resume instead, or None if an idle task (the kernel proper)
/// was at fault.
pub fn kill_current(frame: *mut Frame) -> Option<(u32, *mut Frame)> {
    let pid = {
        let mut guard = SCHED.lock();
        let s = guard.as_mut()?;
        let t = s.current();
        if t.idle {
            return None;
        }
        let pid = t.pid;
        if t.process.is_some() {
            s.kill_process(pid);
        } else {
            t.state = State::Dead;
            s.wake(Wait::Exit);
        }
        pid
    };
    Some((pid, schedule(frame, false)))
}

/// Kill a process (all its threads). Safe from interrupt context: a victim
/// that is running right now is simply never scheduled again.
fn kill_pid(pid: u32) -> Option<bool> {
    let mut guard = SCHED.lock();
    let s = guard.as_mut()?;
    let own_pid = s.current().pid;
    (s.kill_process(pid) > 0).then_some(pid == own_pid)
}

pub fn kill(pid: u32) -> Result<(), &'static str> {
    match kill_pid(pid) {
        None => Err("no such process (or it is an idle task, which cannot be killed)"),
        Some(true) => loop {
            yield_now();
        },
        Some(false) => Ok(()),
    }
}

/// Is any thread of this process still alive?
pub fn is_alive(pid: u32) -> bool {
    let guard = SCHED.lock();
    guard.as_ref().map_or(false, |s| s.tasks.iter().any(|t| t.pid == pid && t.state != State::Dead))
}

/// Does any trace of this process remain: a live thread, or a dead one
/// whose memory has not been reclaimed yet?
pub fn exists(pid: u32) -> bool {
    let guard = SCHED.lock();
    guard.as_ref().map_or(false, |s| s.tasks.iter().any(|t| t.pid == pid))
}

/// Is this thread alive, and part of the caller's process?
pub fn thread_alive(tid: u32) -> bool {
    let mut guard = SCHED.lock();
    let Some(s) = guard.as_mut() else { return false };
    let pid = s.current().pid;
    s.tasks.iter().any(|t| t.id == tid && t.pid == pid && t.state != State::Dead)
}

/// Say which processes Ctrl+C should kill (empty: none).
pub fn set_foreground(pids: &[u32]) {
    let mut foreground = FOREGROUND.lock();
    foreground.clear();
    foreground.extend_from_slice(pids);
}

/// Ctrl+C from the keyboard or serial interrupt: every foreground process
/// gets an interrupt signal, which ends it unless it has a handler. Returns
/// true if there was a foreground process to signal.
pub fn interrupt_foreground() -> bool {
    let pids = FOREGROUND.lock().clone();
    signal_pids(&pids, SIGINT)
}

/// Send a signal to each of these processes. Returns true if any of them
/// existed.
pub fn signal_pids(pids: &[u32], signal: u32) -> bool {
    let mut guard = SCHED.lock();
    let Some(s) = guard.as_mut() else { return false };
    let mut signalled = false;
    for &pid in pids {
        signalled |= s.signal(pid, signal);
    }
    signalled
}

/// Send a signal to a user process. A kernel thread cannot run handlers, so
/// any signal simply ends it.
pub fn send_signal(pid: u32, signal: u32) -> Result<(), &'static str> {
    if !(1..32).contains(&signal) {
        return Err("no such signal");
    }
    let delivered = {
        let mut guard = SCHED.lock();
        guard.as_mut().map_or(false, |s| s.signal(pid, signal))
    };
    if !delivered {
        return kill(pid);
    }
    Ok(())
}

/// Install (or with 0, remove) the calling process's signal handler.
pub fn set_signal_handler(handler: u64) {
    if let Some(process) = current_process() {
        process.signal_handler.store(handler, Ordering::Relaxed);
    }
}

/// Where a signal handler returns to: a page the loader maps into every
/// process, holding the `sigreturn` system call.
pub const SIGNAL_RETURN: u64 = USER_BASE + 0x1000;

/// Called on the way back to user mode. If the task has a signal waiting
/// and is not already handling one, rearrange its saved registers so that
/// it resumes inside its handler instead; `signal_return` undoes this.
pub fn deliver_signals(frame: *mut Frame) {
    if SIGNALS_PENDING.load(Ordering::Relaxed) == 0 {
        return;
    }
    let f = unsafe { &mut *frame };
    let mut guard = SCHED.lock();
    let Some(s) = guard.as_mut() else { return };
    let t = s.current();
    if t.signals == 0 || t.in_handler.is_some() || t.state == State::Dead {
        return;
    }
    let Some(process) = t.process.clone() else { return };
    let signal = t.signals.trailing_zeros();
    t.signals &= !(1 << signal);
    SIGNALS_PENDING.fetch_sub(1, Ordering::SeqCst);
    let pid = t.pid;

    // The handler is entered like a called function: return address on the
    // stack, the signal number in rcx and (for c-comp code, which takes its
    // arguments from the stack) in the slot above the return address.
    let handler = process.signal_handler.load(Ordering::Relaxed);
    let stack = (f.rsp.wrapping_sub(256) & !0xF).wrapping_sub(8);
    if handler == 0 || !memory::user_range_ok(process.space, stack, 48, true) {
        s.kill_process(pid);
        return;
    }
    let fx_area = ((frame as usize - FX_AREA) & !0xF) as *mut u8;
    let mut saved = Box::new(SavedContext { frame: *f, fx: [0; FX_AREA] });
    // If the handler is interrupting a system call that was waiting (its
    // `int 0x80` is set to run again), that call does not go back to
    // waiting afterwards: it returns EINTR, so the program gets a chance to
    // act on whatever its handler did.
    let t = s.current();
    if t.retrying {
        t.retrying = false;
        saved.frame.rip += 2;
        saved.frame.rax = EINTR as u64;
    }
    unsafe {
        core::ptr::copy_nonoverlapping(fx_area, saved.fx.as_mut_ptr(), FX_AREA);
        (stack as *mut u64).write(SIGNAL_RETURN);
        ((stack + 8) as *mut u64).write(signal as u64);
    }
    s.current().in_handler = Some(saved);
    f.rip = handler;
    f.rsp = stack;
    f.rcx = signal as u64;
    f.rflags = 0x202;
}

/// The `sigreturn` system call: a signal handler has finished, so put back
/// the state it interrupted. Returns false if no handler was running.
pub fn signal_return(frame: *mut Frame) -> bool {
    let saved = {
        let mut guard = SCHED.lock();
        guard.as_mut().and_then(|s| s.current().in_handler.take())
    };
    let Some(saved) = saved else { return false };
    let fx_area = ((frame as usize - FX_AREA) & !0xF) as *mut u8;
    unsafe {
        *frame = saved.frame;
        core::ptr::copy_nonoverlapping(saved.fx.as_ptr(), fx_area, FX_AREA);
    }
    true
}

/// Exit code of process `pid`, if it called exit (rather than being killed)
/// recently enough to still be on record.
pub fn exit_code(pid: u32) -> Option<i64> {
    EXIT_CODES.lock().iter().rev().find(|(p, _)| *p == pid).map(|&(_, code)| code)
}

/// Free the stacks (and, with a process's last thread, the address space)
/// of dead tasks that no CPU is still standing on. Called from idle tasks.
pub fn reap() {
    // Collect the dead under the lock, but let them go after releasing it:
    // dropping a process closes files and frees memory, which is slow and
    // takes other locks.
    let mut dead = Vec::new();
    {
        let mut guard = SCHED.lock();
        if let Some(s) = guard.as_mut() {
            let mut i = 0;
            while i < s.tasks.len() {
                if s.tasks[i].state == State::Dead && s.tasks[i].on_cpu.is_none() {
                    let lost = s.tasks[i].signals.count_ones();
                    if lost > 0 {
                        SIGNALS_PENDING.fetch_sub(lost, Ordering::SeqCst);
                    }
                    dead.push(s.tasks.remove(i));
                } else {
                    i += 1;
                }
            }
        }
    }
    drop(dead);
}

pub struct TaskInfo {
    pub id: u32,
    pub pid: u32,
    pub name: String,
    pub state: State,
    pub cpu_ticks: u64,
    pub user: bool,
    pub cpu: Option<usize>,
    /// Ticks since boot at which it was started.
    pub started: u64,
}

pub fn list() -> Vec<TaskInfo> {
    let guard = SCHED.lock();
    guard.as_ref().map_or(Vec::new(), |s| {
        s.tasks
            .iter()
            .map(|t| TaskInfo {
                id: t.id,
                pid: t.pid,
                name: t.name.clone(),
                state: t.state,
                cpu_ticks: t.cpu_ticks,
                user: t.process.is_some(),
                cpu: t.on_cpu,
                started: t.started,
            })
            .collect()
    })
}
