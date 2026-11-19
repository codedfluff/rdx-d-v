//! `init`, and the kernel's built-in rescue shell.
//!
//! The shell people use is a normal program, sh.exe. The kernel's first
//! thread (`init`) starts it on the console and starts it again whenever it
//! exits. Only if that program cannot be run at all - it is missing from
//! the boot disk, or was overwritten with something broken - does the
//! machine drop into the shell in this file instead, which lives inside the
//! kernel and so is always there to repair things from.

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;
use core::sync::atomic::{AtomicI64, AtomicU32, Ordering};

use crate::arch::{self, TICK_HZ};
use crate::console::{self, BLUE, CYAN, DIM, GREEN, MAGENTA, RED, YELLOW};
use crate::sched::{self, State};
use alloc::vec;

use crate::keyboard::{KEY_DELETE, KEY_DOWN, KEY_END, KEY_HOME, KEY_LEFT, KEY_RIGHT, KEY_UP};
use crate::pipe::{Fd, Pipe};
use crate::sync::IrqLock;
use crate::{clock, exec, fs, input, net, procfs};

/// The shell's current working directory, as a filesystem key (root = "").
static CWD: IrqLock<String> = IrqLock::new(String::new());

fn cwd() -> String {
    CWD.lock().clone()
}

/// Resolve a path typed at the shell against the working directory.
fn path_key(name: &str) -> String {
    fs::resolve(&CWD.lock(), name)
}

/// The shell's environment variables, handed to every program it starts.
static ENV: IrqLock<exec::Env> = IrqLock::new(Vec::new());
/// Exit code of the last program run, for `$?`.
static LAST_STATUS: AtomicI64 = AtomicI64::new(0);

fn env() -> exec::Env {
    ENV.lock().clone()
}

fn env_value(name: &str) -> Option<String> {
    exec::env_get(&ENV.lock(), name).map(String::from)
}

fn env_set(name: &str, value: &str) {
    let mut env = ENV.lock();
    env.retain(|(n, _)| n != name);
    env.push((String::from(name), String::from(value)));
}

/// Where to look for programs: the PATH variable.
fn search_path() -> String {
    env_value("PATH").unwrap_or_else(|| String::from(exec::DEFAULT_PATH))
}

/// Find a program by name, as the shell would run it.
fn find_program(name: &str) -> Option<(String, Vec<u8>)> {
    exec::resolve(name, &cwd(), &search_path())
}

/// Replace `$NAME`, `${NAME}` and `$?` in a command line with their values.
/// An unset variable becomes nothing; a `$` followed by anything else is
/// left alone.
fn expand(line: &str) -> String {
    let bytes = line.as_bytes();
    let mut out = String::new();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] != b'$' {
            // Copy up to the next `$` (whole characters, so UTF-8 survives).
            let end = line[i..].find('$').map_or(line.len(), |n| i + n);
            out.push_str(&line[i..end]);
            i = end;
            continue;
        }
        let rest = &line[i + 1..];
        if rest.starts_with('?') {
            out.push_str(&format!("{}", LAST_STATUS.load(Ordering::Relaxed)));
            i += 2;
        } else if let Some(close) = rest.strip_prefix('{').and_then(|r| r.find('}')) {
            out.push_str(&env_value(&rest[1..1 + close]).unwrap_or_default());
            i += close + 3;
        } else {
            let len = rest.bytes().take_while(|b| b.is_ascii_alphanumeric() || *b == b'_').count();
            if len == 0 {
                out.push('$');
            } else {
                out.push_str(&env_value(&rest[..len]).unwrap_or_default());
            }
            i += 1 + len;
        }
    }
    out
}

/// Run each line of a file as a command. Blank lines and lines starting
/// with `#` are skipped.
fn run_script(key: &str) -> bool {
    let Some(data) = fs::read(key) else { return false };
    for line in String::from_utf8_lossy(&data).lines() {
        execute(line.trim());
    }
    true
}

pub fn banner() {
    console::set_fg(CYAN);
    println!(r"  ____  ______  __");
    println!(r" |  _ \|  _ \ \/ /");
    println!(r" | |_) | | | \  / ");
    println!(r" |  _ <| |_| /  \ ");
    println!(r" |_| \_\____/_/\_\   v{}", env!("CARGO_PKG_VERSION"));
    console::reset_fg();
    println!();
}

/// The program `init` runs as the shell.
const SHELL_PROGRAM: &str = "sh.exe";

/// Start the user-space shell on the console. Returns its process ID.
fn start_shell() -> Result<u32, &'static str> {
    let (file, image) = exec::resolve(SHELL_PROGRAM, "", exec::DEFAULT_PATH).ok_or("it is not on the boot disk")?;
    let env = vec![
        (String::from("PATH"), String::from(exec::DEFAULT_PATH)),
        (String::from("HOME"), String::from("/")),
        (String::from("SHELL"), String::from(SHELL_PROGRAM)),
    ];
    let fds = vec![Some(Fd::Console), Some(Fd::Console), Some(Fd::Console)];
    exec::spawn(&file, &image, SHELL_PROGRAM, fds, String::new(), env)
}

/// The kernel's first thread: keep a shell running on the console.
pub fn init() {
    let mut quick_exits = 0;
    loop {
        let started = arch::ticks();
        match start_shell() {
            Ok(pid) => {
                loop {
                    let seen = sched::events();
                    sched::reap();
                    if !sched::exists(pid) {
                        break;
                    }
                    sched::wait_for(sched::Wait::Exit, seen);
                }
                sched::set_foreground(&[]);
                console::set_mirror(true);
                console::reset_fg();
                // A shell that keeps dying at once is broken: stop retrying.
                quick_exits = if arch::ticks() < started + TICK_HZ { quick_exits + 1 } else { 0 };
                if quick_exits >= 3 {
                    rescue("it keeps stopping as soon as it starts");
                }
            }
            Err(reason) => rescue(reason),
        }
    }
}

/// Fall back to the shell built into the kernel. Never returns.
fn rescue(reason: &str) -> ! {
    console::set_fg(YELLOW);
    println!("The shell ({}) could not be run: {}.", SHELL_PROGRAM, reason);
    println!("This is the kernel's built-in rescue shell. If you replaced {}, 'rm {}' brings", SHELL_PROGRAM, SHELL_PROGRAM);
    println!("back the copy on the boot disk; then 'reboot'.");
    console::reset_fg();
    run()
}

pub fn run() -> ! {
    env_set("PATH", exec::DEFAULT_PATH);
    env_set("HOME", "/");
    env_set("SHELL", "rescue");
    println!("Type 'help' for a list of commands, 'ls' to see programs.");
    println!();
    let mut history: Vec<String> = Vec::new();
    loop {
        let line = read_line(&history);
        let trimmed = line.trim();
        // Remember non-empty commands, skipping an immediate repeat.
        if !trimmed.is_empty() && history.last().map(String::as_str) != Some(trimmed) {
            history.push(String::from(trimmed));
            if history.len() > 100 {
                history.remove(0);
            }
        }
        execute(trimmed);
    }
}

/// Draw the prompt, showing the working directory when it isn't the root.
fn prompt() {
    console::set_fg(GREEN);
    print!("rdx");
    let path = fs::display_path(&cwd());
    if path != "/" {
        console::set_fg(BLUE);
        print!("{}", path);
    }
    console::set_fg(DIM);
    print!("> ");
    console::reset_fg();
}

/// The line being typed, and where in it the cursor is.
struct LineEdit {
    text: String,
    cursor: usize,
}

impl LineEdit {
    /// Change the line and repaint it. `change` edits the text and cursor;
    /// the screen is then brought in line: step back to the start of the
    /// input, print the new text, blank out whatever the old one had beyond
    /// it, and step back to the cursor.
    fn update(&mut self, change: impl FnOnce(&mut LineEdit)) {
        let (old_len, old_cursor) = (self.text.len(), self.cursor);
        change(self);
        self.cursor = self.cursor.min(self.text.len());
        console::left(old_cursor);
        print!("{}", self.text);
        let extra = old_len.saturating_sub(self.text.len());
        for _ in 0..extra {
            print!(" ");
        }
        console::left(self.text.len() + extra - self.cursor);
    }

    /// Replace the whole line, leaving the cursor at its end.
    fn replace(&mut self, replacement: &str) {
        self.update(|e| {
            e.text = String::from(replacement);
            e.cursor = e.text.len();
        });
    }
}

/// Built-in command names, for tab completion.
const BUILTINS: &[&str] = &[
    "help", "clear", "echo", "about", "mem", "memmap", "uptime", "date", "cpu", "ps", "spawn", "kill",
    "sleep", "ls", "cat", "write", "pwd", "cd", "mkdir", "rmdir", "rm", "run", "mv", "cp", "usb", "net",
    "ping", "disk", "colors", "crash", "panic", "reboot", "shutdown", "set", "unset", "source",
];

/// The longest string that every candidate starts with.
fn common_prefix(items: &[String]) -> String {
    let mut prefix = items[0].clone();
    for item in &items[1..] {
        while !item.starts_with(&prefix) {
            prefix.pop();
        }
    }
    prefix
}

/// Complete the word being typed at the end of `line` on Tab. Completes
/// command names in the first position and file/directory names elsewhere.
fn complete(line: &mut String) {
    let word_start = line.rfind(' ').map_or(0, |i| i + 1);
    let word = &line[word_start..];
    let command_position = word_start == 0 && !word.contains('/');

    // Split a path-style word into the directory to search and the partial
    // final component.
    let (dir, partial): (String, String) = match word.rfind('/') {
        Some(i) => (fs::resolve(&cwd(), &word[..i]), String::from(&word[i + 1..])),
        None => (cwd(), String::from(word)),
    };

    let mut candidates: Vec<String> = Vec::new();
    if command_position {
        for &name in BUILTINS {
            if name.starts_with(&partial) {
                candidates.push(String::from(name));
            }
        }
        // Programs are files in the root directory; offer them without ".exe".
        for entry in fs::list("") {
            if let Some(stem) = entry.name.strip_suffix(".exe") {
                if stem.starts_with(&partial) {
                    candidates.push(String::from(stem));
                }
            }
        }
    } else {
        for entry in fs::list(&dir) {
            if entry.name.starts_with(&partial) {
                candidates.push(if entry.is_dir { format!("{}/", entry.name) } else { entry.name });
            }
        }
    }
    candidates.sort();
    candidates.dedup();
    if candidates.is_empty() {
        return;
    }

    if candidates.len() == 1 {
        let full = &candidates[0];
        let add = String::from(&full[partial.len()..]);
        line.push_str(&add);
        print!("{}", add);
        // A completed file name gets a trailing space; a directory a slash
        // (already appended) so the next Tab descends into it.
        if !full.ends_with('/') {
            line.push(' ');
            print!(" ");
        }
    } else {
        let common = common_prefix(&candidates);
        if common.len() > partial.len() {
            let add = String::from(&common[partial.len()..]);
            line.push_str(&add);
            print!("{}", add);
        } else {
            // No further common text: show the options and redraw the line.
            println!();
            console::set_fg(DIM);
            for c in &candidates {
                print!("{}  ", c);
            }
            console::reset_fg();
            println!();
            prompt();
            print!("{}", line);
        }
    }
}

/// Read one command line. Left/Right, Home/End (or Ctrl+A/Ctrl+E) move
/// within it and typing inserts at the cursor; Backspace and Delete remove
/// a character, Ctrl+U the whole line; Tab completes; Up/Down recall
/// earlier commands; Ctrl+C abandons the line.
fn read_line(history: &[String]) -> String {
    prompt();
    let mut edit = LineEdit { text: String::new(), cursor: 0 };
    // Position while browsing history; history.len() is "the line being typed".
    let mut browse = history.len();
    let mut stash = String::new();
    loop {
        match input::read() {
            b'\n' => {
                println!("{}", &edit.text[edit.cursor..]); // leave the cursor after the text
                break;
            }
            0x08 => {
                if edit.cursor > 0 {
                    edit.update(|e| {
                        e.cursor -= 1;
                        e.text.remove(e.cursor);
                    });
                }
            }
            KEY_DELETE => {
                if edit.cursor < edit.text.len() {
                    edit.update(|e| {
                        e.text.remove(e.cursor);
                    });
                }
            }
            KEY_LEFT => {
                if edit.cursor > 0 {
                    edit.cursor -= 1;
                    console::left(1);
                }
            }
            KEY_RIGHT => {
                if edit.cursor < edit.text.len() {
                    print!("{}", &edit.text[edit.cursor..edit.cursor + 1]);
                    edit.cursor += 1;
                }
            }
            KEY_HOME | 0x01 => edit.update(|e| e.cursor = 0),
            KEY_END | 0x05 => edit.update(|e| e.cursor = e.text.len()),
            0x15 => edit.replace(""),
            b'\t' => {
                // Completion works on the word at the end of the line.
                if edit.cursor == edit.text.len() {
                    browse = history.len();
                    complete(&mut edit.text);
                    edit.cursor = edit.text.len();
                }
            }
            0x03 => {
                println!("{}^C", &edit.text[edit.cursor..]);
                edit.text.clear();
                break;
            }
            KEY_UP => {
                if browse > 0 {
                    if browse == history.len() {
                        stash = edit.text.clone();
                    }
                    browse -= 1;
                    edit.replace(&history[browse]);
                }
            }
            KEY_DOWN => {
                if browse < history.len() {
                    browse += 1;
                    let next =
                        if browse == history.len() { stash.clone() } else { history[browse].clone() };
                    edit.replace(&next);
                }
            }
            b if (32..127).contains(&b) && edit.text.len() < 200 => {
                if edit.cursor == edit.text.len() {
                    // The common case, typing at the end: nothing to repaint.
                    edit.text.push(b as char);
                    edit.cursor += 1;
                    print!("{}", b as char);
                } else {
                    edit.update(|e| {
                        e.text.insert(e.cursor, b as char);
                        e.cursor += 1;
                    });
                }
            }
            _ => {}
        }
    }
    edit.text
}

fn execute(line: &str) {
    if line.starts_with('#') {
        return; // a comment, as in a script
    }
    let expanded = expand(line);
    let line = expanded.trim();
    // `program &` starts it in the background: the shell does not wait.
    if let Some(command) = line.strip_suffix('&') {
        if let Err(message) = run_pipeline(command.trim(), true) {
            error(&message);
        }
        return;
    }
    let mut parts = line.split_whitespace();
    let Some(cmd) = parts.next() else { return };
    let args: Vec<&str> = parts.collect();
    let rest = line[cmd.len()..].trim_start();

    // Pipelines and redirection always mean programs. So do the everyday
    // file commands whenever their program versions are on the disk; the
    // built-in ones below are only a fallback for a disk without them.
    let piped = line.contains(['|', '>', '<']);
    let has_program =
        matches!(cmd, "ls" | "cat" | "echo" | "rm" | "mv" | "cp") && find_program(cmd).is_some();
    if piped || has_program {
        if let Err(message) = run_pipeline(line, false) {
            error(&message);
        }
        return;
    }

    match cmd {
        "help" => help(),
        "clear" | "cls" => console::clear(),
        "echo" => println!("{}", rest),
        "about" | "mem" | "memmap" | "uptime" | "cpu" | "usb" | "net" | "disk" => {
            print!("{}", procfs::read(cmd).unwrap_or_default());
        }
        "date" => date(),
        "ps" => ps(),
        "spawn" => spawn(&args),
        "kill" => {
            // `kill 12` asks politely (signal 15, which a program may handle);
            // `kill -9 12` cannot be refused.
            let (signal, target) = match args.first().and_then(|a| a.strip_prefix('-')) {
                Some(number) => (number.parse::<u32>().ok(), args.get(1)),
                None => (Some(sched::SIGTERM), args.first()),
            };
            match (signal, target.and_then(|a| a.parse::<u32>().ok())) {
                (Some(signal), Some(id)) => match sched::send_signal(id, signal) {
                    Ok(()) if !sched::is_alive(id) => println!("killed task {}", id),
                    Ok(()) => println!("sent signal {} to task {}", signal, id),
                    Err(e) => error(e),
                },
                _ => error("usage: kill [-signal] <process id>   (signals: 2 interrupt, 15 terminate, 9 kill)"),
            }
        }
        "set" | "export" => match rest.split_once('=') {
            Some((name, value)) if !name.trim().is_empty() && !name.trim().contains(' ') => {
                env_set(name.trim(), value.trim());
            }
            None if rest.is_empty() => {
                for (name, value) in env() {
                    println!("{}={}", name, value);
                }
            }
            _ => error("usage: set NAME=value   (set on its own lists the variables)"),
        },
        "unset" => match args.first() {
            Some(name) => ENV.lock().retain(|(n, _)| n != name),
            None => error("usage: unset NAME"),
        },
        "source" | "." => match args.first() {
            Some(name) => {
                if !run_script(&path_key(name)) {
                    error("no such file");
                }
            }
            None => error("usage: source <file of commands>"),
        },
        "sleep" => match args.first().and_then(|a| a.parse::<u64>().ok()) {
            Some(ms) => sched::sleep_ms(ms),
            None => error("usage: sleep <milliseconds>"),
        },
        "ls" => ls(&args),
        "cat" => cat(&args),
        "write" => write(&args, rest),
        "pwd" => println!("{}", fs::display_path(&cwd())),
        "cd" => {
            let target = args.first().map_or_else(String::new, |a| path_key(a));
            if fs::is_dir(&target) {
                *CWD.lock() = target;
            } else {
                error("no such directory");
            }
        }
        "mkdir" => match args.first() {
            Some(name) => {
                if let Err(e) = fs::make_dir(&path_key(name)) {
                    error(e);
                }
            }
            None => error("usage: mkdir <directory>"),
        },
        "rmdir" => match args.first() {
            Some(name) => {
                if let Err(e) = fs::remove_dir(&path_key(name), false) {
                    error(e);
                }
            }
            None => error("usage: rmdir <directory>"),
        },
        "rm" => match args.first() {
            Some(name) => {
                let key = path_key(name);
                let recursive = *name == "-r";
                let target = if recursive { args.get(1).map(|n| path_key(n)) } else { Some(key) };
                match target {
                    Some(key) if fs::is_dir(&key) && !fs::exists(&key) => {
                        if let Err(e) = fs::remove_dir(&key, recursive) {
                            error(e);
                        }
                    }
                    Some(key) if !fs::remove(&key) => error("no such file"),
                    Some(_) => {}
                    None => error("usage: rm [-r] <file>"),
                }
            }
            None => error("usage: rm [-r] <file>"),
        },
        "run" => {
            if args.is_empty() {
                error("usage: run <program> [arguments]");
            } else if let Err(message) = run_pipeline(rest, false) {
                error(&message);
            }
        }
        "mv" | "rename" | "cp" | "copy" => {
            let keep_original = cmd == "cp" || cmd == "copy";
            match (args.first(), args.get(1)) {
                (Some(from), Some(to)) if from == to => error("the two names are the same"),
                (Some(from), Some(to)) => {
                    let (from, to) = (path_key(from), path_key(to));
                    match fs::read(&from) {
                        Some(data) => {
                            if fs::exists(&to) || fs::is_dir(&to) {
                                error("a file with the new name already exists (rm it first)");
                            } else if fs::write(&to, data).is_ok() && !keep_original {
                                fs::remove(&from);
                            }
                        }
                        None => error("no such file"),
                    }
                }
                _ => error(if keep_original { "usage: cp <file> <new name>" } else { "usage: mv <file> <new name>" }),
            }
        }
        "ping" => ping(&args),
        "colors" => colors(),
        "crash" => {
            let id = start_crasher();
            println!("spawned task {} which will dereference a null pointer", id);
            sched::sleep_ms(50);
        }
        "panic" => panic!("panic requested from the shell"),
        "reboot" => arch::reboot(),
        "shutdown" | "poweroff" => arch::shutdown(),
        // Anything else: maybe it's a program. `hello` runs hello.exe.
        _ => {
            if let Err(message) = run_pipeline(line, false) {
                error(&message);
            }
        }
    }
}

/// One program in a pipeline, with any `< file` or `> file` it was given.
struct Stage {
    file: String,
    image: Vec<u8>,
    cmdline: String,
    input: Option<String>,
    /// File name, and whether to append (`>>`) rather than replace (`>`).
    output: Option<(String, bool)>,
}

fn parse_stage(text: &str) -> Result<Stage, String> {
    let mut words: Vec<&str> = Vec::new();
    let (mut input, mut output) = (None, None);
    let mut tokens = text.split_whitespace();
    while let Some(token) = tokens.next() {
        // Accept both `> file` and `>file`.
        let (kind, attached) = if let Some(rest) = token.strip_prefix(">>") {
            (">>", rest)
        } else if let Some(rest) = token.strip_prefix('>') {
            (">", rest)
        } else if let Some(rest) = token.strip_prefix('<') {
            ("<", rest)
        } else {
            words.push(token);
            continue;
        };
        let name = if attached.is_empty() { tokens.next() } else { Some(attached) };
        let name = name.ok_or_else(|| format!("expected a file name after {}", kind))?;
        let key = path_key(name);
        match kind {
            "<" => input = Some(key),
            _ => output = Some((key, kind == ">>")),
        }
    }
    let Some(&program) = words.first() else { return Err(String::from("there is an empty command in the pipeline")) };
    let (file, image) = find_program(program).ok_or_else(|| format!("unknown command or program: {}", program))?;
    Ok(Stage { file, image, cmdline: words.join(" "), input, output })
}

/// Run `a | b | c`, each with optional `< file` / `> file`. A single
/// program with no pipes is just the one-stage case. In the foreground the
/// shell waits until all of them finish, and Ctrl+C goes to them; in the
/// background it reports their process IDs and carries on (stop one with
/// `kill`).
fn run_pipeline(line: &str, background: bool) -> Result<(), String> {
    let stages = line.split('|').map(parse_stage).collect::<Result<Vec<Stage>, String>>()?;

    // Give each program its standard input and output: a file if it asked
    // for one, else the pipe to its neighbour, else the console.
    let mut pids: Vec<u32> = Vec::new();
    let mut from_previous: Option<Fd> = None;
    let last = stages.len() - 1;
    for (i, stage) in stages.iter().enumerate() {
        let piped_in = from_previous.take();
        let stdin = match &stage.input {
            Some(key) => Fd::open_read(key)
                .ok_or_else(|| format!("{}: no such file", fs::display_path(key))),
            None => Ok(piped_in.unwrap_or(Fd::Console)),
        };
        let stdout = match &stage.output {
            Some((name, append)) => Fd::open_write(name, *append),
            None if i < last => {
                let (read_end, write_end) = Pipe::create();
                from_previous = Some(read_end);
                write_end
            }
            None => Fd::Console,
        };
        let started = stdin.and_then(|stdin| {
            exec::spawn(
                &stage.file,
                &stage.image,
                &stage.cmdline,
                vec![Some(stdin), Some(stdout), Some(Fd::Console)],
                cwd(),
                env(),
            )
            .map_err(|e| format!("cannot run {}: {}", stage.file, e))
        });
        match started {
            Ok(pid) => pids.push(pid),
            Err(message) => {
                // Don't leave half a pipeline running.
                for &pid in &pids {
                    let _ = sched::kill(pid);
                }
                return Err(message);
            }
        }
    }
    // The shell kept no copies of the pipe ends, so each pipe closes as soon
    // as the programs using it finish.

    if background {
        let ids: Vec<String> = pids.iter().map(|pid| format!("{}", pid)).collect();
        console::set_fg(DIM);
        println!("[running in the background as process {}]", ids.join(", "));
        console::reset_fg();
        return Ok(());
    }

    sched::set_foreground(&pids);
    loop {
        let seen = sched::events();
        if !pids.iter().any(|&pid| sched::is_alive(pid)) {
            break;
        }
        sched::wait_for(sched::Wait::Exit, seen);
    }
    // Reclaim their memory before going back to the prompt, rather than
    // leaving it to the idle task. (A program that has only just finished
    // may still be on a CPU for a moment; give it that moment.)
    for _ in 0..200 {
        sched::reap();
        if !pids.iter().any(|&pid| sched::exists(pid)) {
            break;
        }
        sched::yield_now();
    }
    sched::set_foreground(&[]);
    // Undo anything a full-screen program left behind.
    console::set_mirror(true);
    console::reset_fg();

    let stage = &stages[last];
    let status = sched::exit_code(pids[last]);
    LAST_STATUS.store(status.unwrap_or(-1), Ordering::Relaxed);
    match status {
        Some(0) => {}
        Some(code) => {
            console::set_fg(DIM);
            println!("[{} exited with code {}]", stage.file, code);
            console::reset_fg();
        }
        None => {
            console::set_fg(DIM);
            println!("[{} was terminated]", stage.file);
            console::reset_fg();
        }
    }
    Ok(())
}

fn error(msg: &str) {
    console::set_fg(RED);
    println!("{}", msg);
    console::reset_fg();
}

fn help() {
    const COMMANDS: &[(&str, &str)] = &[
        ("help", "show this list"),
        ("about", "what this is"),
        ("clear", "clear the screen"),
        ("echo <text>", "print text"),
        ("mem", "memory usage"),
        ("memmap", "usable physical memory regions"),
        ("cpu", "processor identification"),
        ("uptime", "time since boot"),
        ("date", "date and time from the hardware clock"),
        ("ps", "list tasks"),
        ("spawn [n]", "start n background worker tasks (default 1)"),
        ("kill [-9] <id>", "ask a process to stop (-9: force it)"),
        ("set NAME=value", "set a variable; use it as $NAME ($? = last exit code)"),
        ("unset NAME", "remove a variable"),
        ("source <file>", "run a file of commands (startup.rc runs at boot)"),
        ("sleep <ms>", "sleep the shell"),
        ("ls [dir]", "list files in a directory"),
        ("cd [dir]", "change directory (cd with no name goes to /)"),
        ("pwd", "print the working directory"),
        ("mkdir <dir>", "make a directory"),
        ("rmdir <dir>", "remove an empty directory"),
        ("disk", "show the disk that files are saved to"),
        ("usb", "show USB controllers, keyboards and drives"),
        ("net", "show the network adapter and its address"),
        ("ping <address>", "check that another machine answers, e.g. ping 10.0.2.2"),
        ("edit <file>", "open the text editor"),
        ("rx [script]", "run an RX script, or start the RX prompt"),
        ("c-comp <file.c>", "compile a C program into a .exe"),
        ("cat <file>", "print a file"),
        ("write <file> <text>", "create or overwrite a file"),
        ("rm [-r] <file>", "delete a file (or a directory, with -r)"),
        ("mv <file> <new name>", "rename a file"),
        ("cp <file> <new name>", "copy a file"),
        ("run <program> [args]", "run a .exe (or just type its name; Ctrl+C stops it)"),
        ("<program> &", "run it in the background (see ps; stop it with kill)"),
        ("colors", "show the console palette"),
        ("crash", "spawn a task that faults (the kernel survives)"),
        ("panic", "trigger a kernel panic"),
        ("reboot", "restart the machine"),
        ("shutdown", "power off"),
    ];
    for (name, desc) in COMMANDS {
        console::set_fg(YELLOW);
        print!("  {:<22}", name);
        console::reset_fg();
        println!("{}", desc);
    }
}

fn date() {
    println!("{} (hardware clock)", clock::format(clock::now()));
}

fn ps() {
    println!(" PID  TID  STATE     CORE  RING  CPU(s)  NAME");
    let me = sched::current_id();
    for t in sched::list() {
        let state = match t.state {
            State::Ready => "ready",
            State::Running => "running",
            State::Sleeping => "sleeping",
            State::Blocked => "waiting",
            State::Dead => "dead",
        };
        let core = t.cpu.map_or(String::from("-"), |c| format!("{}", c));
        println!(
            "{:>4} {:>4}  {:<8}  {:>4}  {:>4}  {:>6.2}  {}{}",
            t.pid,
            t.id,
            state,
            core,
            if t.user { 3 } else { 0 },
            t.cpu_ticks as f64 / TICK_HZ as f64,
            t.name,
            if t.id == me { " *" } else { "" }
        );
    }
}

static WORKER_SEQ: AtomicU32 = AtomicU32::new(1);

fn worker() {
    let n = WORKER_SEQ.fetch_add(1, Ordering::Relaxed);
    let id = sched::current_id();
    for i in 1..=5 {
        sched::sleep_ms(1000);
        console::set_fg(MAGENTA);
        println!("[worker {} / task {}] tick {} of 5", n, id, i);
        console::reset_fg();
    }
}

/// Start a kernel thread that prints five ticks a second apart: a test
/// that kernel threads are scheduled alongside everything else.
pub fn start_worker() -> u32 {
    sched::spawn("worker", worker)
}

fn spawn(args: &[&str]) {
    let count = args.first().and_then(|a| a.parse::<u32>().ok()).unwrap_or(1).clamp(1, 16);
    for _ in 0..count {
        println!("spawned task {}", start_worker());
    }
}

fn crash_task() {
    unsafe { core::ptr::write_volatile(0x10 as *mut u64, 0xDEAD) };
}

/// Start a kernel thread that dereferences a null pointer: a test that a
/// fault in one kernel thread does not take the kernel down.
pub fn start_crasher() -> u32 {
    sched::spawn("crash", crash_task)
}

fn ls(args: &[&str]) {
    let dir = args.first().map_or_else(cwd, |a| path_key(a));
    if !fs::is_dir(&dir) {
        return error("no such directory");
    }
    let entries = fs::list(&dir);
    if entries.is_empty() {
        println!("(empty)");
    }
    for entry in entries {
        let when = if entry.modified == 0 { String::from("-") } else { clock::format(entry.modified) };
        if entry.is_dir {
            print!("     <dir>         {:<19}  ", when);
            console::set_fg(BLUE);
            println!("{}/", entry.name);
        } else {
            print!("{:>8} bytes  {}  {:<19}  ", entry.size, if entry.saved { "saved" } else { "     " }, when);
            if entry.name.ends_with(".exe") {
                console::set_fg(GREEN);
            } else if entry.name.ends_with(".rx") {
                console::set_fg(CYAN);
            }
            println!("{}", entry.name);
        }
        console::reset_fg();
    }
}

static PING_SEQUENCE: AtomicU32 = AtomicU32::new(1);

/// Send four echo requests a second apart and report on the answers.
fn ping(args: &[&str]) {
    let Some(address) = args.first().and_then(|a| net::parse_ip(a)) else {
        return error("usage: ping <address>   (numbers only, e.g. ping 10.0.2.2)");
    };
    let mut received = 0;
    for _ in 0..4 {
        let sequence = PING_SEQUENCE.fetch_add(1, Ordering::Relaxed) as u16;
        let started = arch::ticks();
        match net::ping(address, sequence) {
            Ok(()) => {}
            Err(net::PingError::NoAdapter) => return error("there is no network adapter"),
            Err(net::PingError::NoAddress) => return error("no address yet (is a DHCP server reachable? see: net)"),
            Err(net::PingError::Unreachable) => {
                println!("no route to {}: nothing answered on the local network", net::format_ip(address));
                continue;
            }
        }
        // Wait up to a second for the reply, then pause out the rest of it.
        let mut answered = None;
        while arch::ticks() < started + TICK_HZ {
            answered = net::ping_reply(sequence);
            if answered.is_some() {
                break;
            }
            sched::sleep_ms(10);
        }
        match answered {
            Some(tick) => {
                received += 1;
                println!(
                    "reply from {}: time {} ms",
                    net::format_ip(address),
                    tick.saturating_sub(started) * 1000 / TICK_HZ
                );
            }
            None => println!("no reply from {} within 1 second", net::format_ip(address)),
        }
        while arch::ticks() < started + TICK_HZ {
            sched::sleep_ms(10);
        }
    }
    println!("4 sent, {} received", received);
}

fn cat(args: &[&str]) {
    let Some(name) = args.first() else { return error("usage: cat <file>") };
    match fs::read(&path_key(name)) {
        Some(data) => {
            let text = String::from_utf8_lossy(&data);
            print!("{}", text);
            if !text.ends_with('\n') {
                println!();
            }
        }
        None => error("no such file"),
    }
}

fn write(args: &[&str], rest: &str) {
    let Some(name) = args.first() else { return error("usage: write <file> <text>") };
    let mut data = rest[name.len()..].trim_start().as_bytes().to_vec();
    data.push(b'\n');
    if let Err(e) = fs::write(&path_key(name), data) {
        error(e);
    }
}

fn colors() {
    for (name, color) in [
        ("red", RED),
        ("green", GREEN),
        ("yellow", YELLOW),
        ("blue", BLUE),
        ("magenta", MAGENTA),
        ("cyan", CYAN),
        ("dim", DIM),
    ] {
        console::set_fg(color);
        print!("{} ", name);
    }
    console::reset_fg();
    println!();
}
