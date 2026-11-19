//! The filesystem. Files live in memory; if an RDX disk is attached, every
//! change is also saved to it and loaded back at the next boot.
//!
//! Paths are hierarchical, like `/docs/notes.txt`. Internally a path is
//! stored as a normalised *key*: no leading slash, no `.` or `..`, with the
//! root directory being the empty string. A file key is its whole path
//! (`docs/notes.txt`); a directory key is the path of the directory
//! (`docs`). Directories that were made explicitly, or that hold files, are
//! tracked so they can be listed and persisted even when empty.
//!
//! Two kinds of file:
//!   * boot files — programs and scripts read from \bin on the boot disk.
//!     They are reloaded from there on every boot and are not saved. These
//!     all live in the root directory.
//!   * your files — anything created or changed from inside RDX, at any
//!     path. These are saved to the RDX disk, and win over a boot file of
//!     the same name.
//!
//! Every file and directory has a "last modified" time. On disk these
//! travel in one extra snapshot entry with a name no real file can have
//! (it starts with a control character), so disks written before
//! timestamps existed still load: their files just show no date.

use alloc::collections::{BTreeMap, BTreeSet};
use alloc::string::String;
use alloc::vec::Vec;
use core::sync::atomic::{AtomicBool, Ordering};

use crate::disk::{self, FileMap, Mount};
use crate::{clock, console, procfs};
use crate::sync::IrqLock;
use crate::uefi::BootInfo;

struct Fs {
    files: FileMap,
    /// Directories (made explicitly, or holding files). Root is not listed.
    dirs: BTreeSet<String>,
    /// Keys that came from the boot disk and haven't been modified.
    boot: BTreeSet<String>,
    /// When each file or directory was last changed (seconds since 1970).
    times: BTreeMap<String, u64>,
}

static FS: IrqLock<Fs> = IrqLock::new(Fs {
    files: FileMap::new(),
    dirs: BTreeSet::new(),
    boot: BTreeSet::new(),
    times: BTreeMap::new(),
});

/// Name of the snapshot entry that carries the timestamps.
const TIMES_ENTRY: &str = "\u{1}mtimes";

/// Can this key be used for a new file or directory? Control characters
/// are reserved (see `TIMES_ENTRY`), and there has to be a name.
fn valid_key(key: &str) -> bool {
    !key.is_empty() && key.bytes().all(|b| b >= 0x20 && b != 0x7F)
}

/// Set when files have changed that a USB disk has yet to be given. The USB
/// driver's thread watches it (see `take_dirty`).
static DIRTY: AtomicBool = AtomicBool::new(false);

// ---------------------------------------------------------------- paths

/// Combine a working directory (a key) with a path (absolute or relative)
/// and return the normalised key it names. `.` and `..` are resolved;
/// `..` at the root stays at the root.
pub fn resolve(cwd: &str, path: &str) -> String {
    let mut parts: Vec<&str> = if path.starts_with('/') {
        Vec::new()
    } else {
        cwd.split('/').filter(|c| !c.is_empty()).collect()
    };
    for component in path.split('/') {
        match component {
            "" | "." => {}
            ".." => {
                parts.pop();
            }
            name => parts.push(name),
        }
    }
    parts.join("/")
}

/// The display form of a key: the root is `/`, everything else is `/key`.
pub fn display_path(key: &str) -> String {
    let mut s = String::from("/");
    s.push_str(key);
    s
}

/// The parent directory of a key, and the final component.
fn split_parent(key: &str) -> (&str, &str) {
    match key.rfind('/') {
        Some(i) => (&key[..i], &key[i + 1..]),
        None => ("", key),
    }
}

impl Fs {
    /// Does a directory exist at this key? The root always does.
    fn is_dir(&self, key: &str) -> bool {
        if key.is_empty() {
            return true;
        }
        if self.dirs.contains(key) {
            return true;
        }
        // Implicit: something lives inside it.
        let prefix = alloc::format!("{}/", key);
        self.files.keys().any(|k| k.starts_with(&prefix)) || self.dirs.iter().any(|k| k.starts_with(&prefix))
    }

    /// Record a directory and all of its parents.
    fn add_dir(&mut self, key: &str) {
        let mut parts: Vec<&str> = key.split('/').filter(|c| !c.is_empty()).collect();
        while !parts.is_empty() {
            let dir = parts.join("/");
            if self.dirs.insert(dir.clone()) {
                self.times.entry(dir).or_insert_with(clock::now);
            }
            parts.pop();
        }
    }

    /// The timestamps as the contents of their snapshot entry:
    /// (u16 name length, name, u64 time) for each saved file and directory.
    fn encode_times(&self) -> Vec<u8> {
        let mut out = Vec::new();
        for (key, &time) in &self.times {
            if self.boot.contains(key) || !(self.files.contains_key(key) || self.dirs.contains(key)) {
                continue;
            }
            out.extend_from_slice(&(key.len() as u16).to_le_bytes());
            out.extend_from_slice(key.as_bytes());
            out.extend_from_slice(&time.to_le_bytes());
        }
        out
    }

    /// Take in timestamps read from a snapshot, for things that exist.
    fn decode_times(&mut self, data: &[u8]) {
        let mut pos = 0;
        while pos + 2 <= data.len() {
            let len = u16::from_le_bytes([data[pos], data[pos + 1]]) as usize;
            pos += 2;
            let (Some(name), Some(time)) = (data.get(pos..pos + len), data.get(pos + len..pos + len + 8)) else {
                break;
            };
            pos += len + 8;
            let key = String::from_utf8_lossy(name).into_owned();
            if self.files.contains_key(&key) || self.dirs.contains(&key) {
                self.times.insert(key, u64::from_le_bytes(time.try_into().unwrap()));
            }
        }
    }

    /// Save everything that isn't an untouched boot file: straight away on
    /// a SATA disk, or by flagging the change for the USB thread.
    fn persist(&self) {
        if !disk::sata_mounted() {
            DIRTY.store(true, Ordering::Relaxed);
            return;
        }
        let entries = self.snapshot_entries();
        if let Err(e) = disk::save(entries.iter().map(|(name, data)| (name, data))) {
            console::set_fg(console::RED);
            crate::println!("warning: could not save to disk: {}", e);
            console::reset_fg();
        }
    }

    /// Everything to write to disk: non-boot files, plus every directory as
    /// a marker whose name ends in `/` (which no real file name does).
    fn snapshot_entries(&self) -> Vec<(String, Vec<u8>)> {
        let mut entries: Vec<(String, Vec<u8>)> = self
            .files
            .iter()
            .filter(|(name, _)| !self.boot.contains(*name))
            .map(|(name, data)| (name.clone(), data.clone()))
            .collect();
        for dir in &self.dirs {
            entries.push((alloc::format!("{}/", dir), Vec::new()));
        }
        entries.push((String::from(TIMES_ENTRY), self.encode_times()));
        entries
    }
}

/// Have files changed since this was last asked? (Clears the flag.)
pub fn take_dirty() -> bool {
    DIRTY.swap(false, Ordering::Relaxed)
}

/// Everything that should be on disk, as snapshot bytes.
pub fn snapshot() -> Vec<u8> {
    let fs = FS.lock();
    let entries = fs.snapshot_entries();
    disk::serialize(entries.iter().map(|(name, data)| (name, data)))
}

/// Sort loaded entries into files and directory markers.
fn load_entries(fs: &mut Fs, entries: Vec<(String, Vec<u8>)>, from_boot: bool) -> usize {
    let mut count = 0;
    let mut times = Vec::new();
    for (name, data) in entries {
        if name == TIMES_ENTRY {
            times = data;
            continue;
        }
        // A disk from before timestamps: files show no date rather than a
        // made-up one. (Overridden below when the disk does carry them.)
        if !from_boot {
            fs.times.insert(String::from(name.trim_end_matches('/')), 0);
        }
        if let Some(dir) = name.strip_suffix('/') {
            if !dir.is_empty() {
                fs.add_dir(dir);
            }
            continue;
        }
        // Make sure the parents of a loaded file exist as directories.
        let (parent, _) = split_parent(&name);
        if !parent.is_empty() {
            fs.add_dir(parent);
        }
        if from_boot {
            fs.boot.insert(name.clone());
        } else {
            fs.boot.remove(&name);
        }
        fs.files.insert(name, data);
        count += 1;
    }
    fs.decode_times(&times);
    count
}

/// A disk appeared after boot: bring in the files saved on it. Files already
/// created or changed in this session win over the disk's copies, and are
/// then written to it along with everything else.
pub fn merge_saved(saved: Vec<(String, Vec<u8>)>) -> usize {
    let mut fs = FS.lock();
    let mut added = 0;
    // Timestamps from the disk apply only to what is actually taken from it.
    let mut disk_times = Fs { files: FileMap::new(), dirs: BTreeSet::new(), boot: BTreeSet::new(), times: BTreeMap::new() };
    let mut times = Vec::new();
    let mut taken: Vec<String> = Vec::new();
    for (name, data) in saved {
        if name == TIMES_ENTRY {
            times = data;
            continue;
        }
        if let Some(dir) = name.strip_suffix('/') {
            if !dir.is_empty() && !fs.dirs.contains(dir) {
                fs.add_dir(dir);
                taken.push(String::from(dir));
            }
            continue;
        }
        if !fs.files.contains_key(&name) || fs.boot.contains(&name) {
            let (parent, _) = split_parent(&name);
            if !parent.is_empty() {
                fs.add_dir(parent);
            }
            fs.boot.remove(&name);
            taken.push(name.clone());
            fs.files.insert(name, data);
            added += 1;
        }
    }
    for key in &taken {
        disk_times.dirs.insert(key.clone());
        fs.times.insert(key.clone(), 0);
    }
    disk_times.decode_times(&times);
    for (key, time) in disk_times.times {
        fs.times.insert(key, time);
    }
    DIRTY.store(true, Ordering::Relaxed);
    added
}

/// Returns (files from the boot disk, files from the RDX disk or None if
/// there is no RDX disk).
pub fn init(boot: &BootInfo) -> (usize, Option<usize>) {
    let mut fs = FS.lock();
    fs.files.insert(
        String::from("readme.txt"),
        b"Welcome to RDX.\n\nPrograms (.exe) and scripts (.rx) come from the \\bin folder of the boot disk.\nRun a program by typing its name:   hello      guess      primes 1000\nThe graphical desktop:              desktop    (Log out on its taskbar comes back here)\nMake folders and move around:       mkdir docs    cd docs    pwd    cd ..\nWrite a script:                     edit mine.rx   then   rx mine\nWrite a C program:                  edit mine.c    then   c-comp mine.c   then   mine\nExamples:                           rx demo    rx worm    c-comp sample.c\nFiles you create are saved to the RDX disk when one is attached (see: disk).\n".to_vec(),
    );
    fs.boot.insert(String::from("readme.txt"));
    // Boot files are loaded fresh each time, so they date from this boot.
    let now = clock::now();
    fs.times.insert(String::from("readme.txt"), now);

    let mut from_boot = 1;
    for f in boot.files() {
        let name = String::from_utf8_lossy(f.name()).to_ascii_lowercase();
        fs.boot.insert(name.clone());
        fs.times.insert(name.clone(), now);
        fs.files.insert(name, f.data().to_vec());
        from_boot += 1;
    }

    let from_disk = match disk::init() {
        Mount::NoDisk => None,
        Mount::Ready(saved) => Some(load_entries(&mut fs, saved, false)),
    };
    (from_boot, from_disk)
}

// ---------------------------------------------------------------- files

pub fn read(key: &str) -> Option<Vec<u8>> {
    // Files in /proc are made up on the spot (and before taking the lock:
    // some of them report on the filesystem itself).
    if let Some(name) = procfs::entry(key) {
        return procfs::read(name).map(String::into_bytes);
    }
    FS.lock().files.get(key).cloned()
}

pub fn exists(key: &str) -> bool {
    if let Some(name) = procfs::entry(key) {
        return procfs::ENTRIES.contains(&name);
    }
    FS.lock().files.contains_key(key)
}

pub fn is_dir(key: &str) -> bool {
    key == procfs::DIR || FS.lock().is_dir(key)
}

/// Is this key /proc or something inside it? Nothing there can be changed.
fn in_proc(key: &str) -> bool {
    key == procfs::DIR || procfs::entry(key).is_some()
}

const PROC_READ_ONLY: &str = "/proc is maintained by the kernel and cannot be changed";

/// Write a file, creating any parent directories. Fails only if the path
/// names an existing directory.
pub fn write(key: &str, data: Vec<u8>) -> Result<(), &'static str> {
    if !valid_key(key) {
        return Err("that is not a usable file name");
    }
    if in_proc(key) {
        return Err(PROC_READ_ONLY);
    }
    let mut fs = FS.lock();
    if fs.is_dir(key) {
        return Err("a directory with that name already exists");
    }
    let (parent, _) = split_parent(key);
    if !parent.is_empty() {
        fs.add_dir(parent);
    }
    fs.boot.remove(key);
    fs.times.insert(String::from(key), clock::now());
    fs.files.insert(String::from(key), data);
    fs.persist();
    Ok(())
}

pub fn remove(key: &str) -> bool {
    if in_proc(key) {
        return false;
    }
    let mut fs = FS.lock();
    if fs.files.remove(key).is_none() {
        return false;
    }
    fs.times.remove(key);
    if !fs.boot.remove(key) {
        fs.persist();
    }
    true
}

/// Make an empty directory. Fails if something already exists at the path.
pub fn make_dir(key: &str) -> Result<(), &'static str> {
    let mut fs = FS.lock();
    if key.is_empty() {
        return Err("the root directory already exists");
    }
    if !valid_key(key) {
        return Err("that is not a usable directory name");
    }
    if in_proc(key) {
        return Err(PROC_READ_ONLY);
    }
    if fs.files.contains_key(key) {
        return Err("a file with that name already exists");
    }
    if fs.dirs.contains(key) {
        return Err("that directory already exists");
    }
    let (parent, _) = split_parent(key);
    if !parent.is_empty() && !fs.is_dir(parent) {
        return Err("the parent directory does not exist");
    }
    fs.add_dir(key);
    fs.persist();
    Ok(())
}

/// Remove a directory. Without `recursive` it must be empty.
pub fn remove_dir(key: &str, recursive: bool) -> Result<(), &'static str> {
    let mut fs = FS.lock();
    if key.is_empty() {
        return Err("the root directory cannot be removed");
    }
    if in_proc(key) {
        return Err(PROC_READ_ONLY);
    }
    if !fs.is_dir(key) {
        return Err("no such directory");
    }
    let prefix = alloc::format!("{}/", key);
    let has_children =
        fs.files.keys().any(|k| k.starts_with(&prefix)) || fs.dirs.iter().any(|k| k.starts_with(&prefix));
    if has_children && !recursive {
        return Err("the directory is not empty (use rm -r)");
    }
    let doomed: Vec<String> =
        fs.files.keys().filter(|k| k.starts_with(&prefix)).cloned().collect();
    let mut persist = false;
    for k in doomed {
        fs.files.remove(&k);
        if !fs.boot.remove(&k) {
            persist = true;
        }
    }
    fs.dirs.retain(|k| k != key && !k.starts_with(&prefix));
    fs.times.retain(|k, _| k != key && !k.starts_with(&prefix));
    fs.persist();
    let _ = persist;
    Ok(())
}

/// One entry in a directory listing.
pub struct Entry {
    pub name: String,
    pub size: usize,
    pub is_dir: bool,
    pub saved: bool,
    /// Last modified, in seconds since 1970 (0: unknown).
    pub modified: u64,
}

/// What is at a key: (is it a directory, size in bytes, last modified).
pub fn stat(key: &str) -> Option<(bool, usize, u64)> {
    if key == procfs::DIR {
        return Some((true, 0, clock::now()));
    }
    if let Some(name) = procfs::entry(key) {
        return procfs::read(name).map(|text| (false, text.len(), clock::now()));
    }
    let fs = FS.lock();
    let modified = fs.times.get(key).copied().unwrap_or(0);
    if let Some(data) = fs.files.get(key) {
        return Some((false, data.len(), modified));
    }
    fs.is_dir(key).then_some((true, 0, modified))
}

/// Bytes held in files, and how many files there are.
pub fn usage() -> (usize, usize) {
    let fs = FS.lock();
    (fs.files.values().map(Vec::len).sum(), fs.files.len())
}

/// List the immediate children of a directory.
pub fn list(dir: &str) -> Vec<Entry> {
    if dir == procfs::DIR {
        let now = clock::now();
        return procfs::ENTRIES
            .iter()
            .map(|name| Entry {
                name: String::from(*name),
                size: procfs::read(name).map_or(0, |text| text.len()),
                is_dir: false,
                saved: false,
                modified: now,
            })
            .collect();
    }
    let fs = FS.lock();
    let mounted = disk::is_mounted();
    let prefix = if dir.is_empty() { String::new() } else { alloc::format!("{}/", dir) };
    // child name -> (is_dir, size, is a saved (non-boot) file?)
    let mut children: BTreeMap<String, (bool, usize, bool)> = BTreeMap::new();

    let mut consider = |key: &str, size: usize, is_file: bool, saved: bool| {
        let Some(rest) = key.strip_prefix(&prefix) else { return };
        if rest.is_empty() {
            return;
        }
        match rest.find('/') {
            // A deeper key: its first component is a subdirectory.
            Some(cut) => {
                children.entry(String::from(&rest[..cut])).or_insert((true, 0, false));
            }
            None => {
                let entry = children.entry(String::from(rest)).or_insert((!is_file, size, saved));
                if is_file {
                    *entry = (false, size, saved);
                }
            }
        }
    };

    for (key, data) in fs.files.iter() {
        consider(key, data.len(), true, mounted && !fs.boot.contains(key));
    }
    for key in fs.dirs.iter() {
        consider(key, 0, false, false);
    }
    consider(procfs::DIR, 0, false, false); // /proc shows up in the root

    children
        .into_iter()
        .map(|(name, (is_dir, size, saved))| {
            let modified = fs.times.get(&alloc::format!("{}{}", prefix, name)).copied().unwrap_or(0);
            Entry { name, size, is_dir, saved, modified }
        })
        .collect()
}
