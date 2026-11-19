//! Random numbers that programs can use for keys (encryption needs them to
//! be unguessable, not just varied).
//!
//! Sources of unpredictability:
//!   * the CPU's own random number instruction (RDRAND), when it has one;
//!   * the time-stamp counter (cycles since power-on), read at start-up and
//!     at every interrupt - keys, packets, disk and timer interrupts all
//!     land at moments that vary in their low bits;
//!   * the hardware clock.
//! These are mixed into a pool. Output comes from ChaCha20 keyed from the
//! pool, and every request rekeys the generator with output nobody saw, so
//! what was handed out before cannot be worked back from its state later.

use core::sync::atomic::{AtomicU64, Ordering};

use crate::sync::IrqLock;

/// Mixed into continually from interrupts, without locking.
static POOL: AtomicU64 = AtomicU64::new(0x243F_6A88_85A3_08D3);

struct Generator {
    key: [u32; 8],
    counter: u64,
}

static GENERATOR: IrqLock<Generator> = IrqLock::new(Generator { key: [0; 8], counter: 0 });

pub fn cycles() -> u64 {
    let (low, high): (u32, u32);
    unsafe { core::arch::asm!("rdtsc", out("eax") low, out("edx") high, options(nomem, nostack, preserves_flags)) };
    (high as u64) << 32 | low as u64
}

/// Does this CPU have RDRAND?
fn has_rdrand() -> bool {
    #[allow(unused_unsafe)]
    let leaf = unsafe { core::arch::x86_64::__cpuid(1) };
    leaf.ecx & (1 << 30) != 0
}

fn rdrand() -> Option<u64> {
    for _ in 0..10 {
        let value: u64;
        let ok: u8;
        unsafe {
            core::arch::asm!("rdrand {v}", "setc {ok}", v = out(reg) value, ok = out(reg_byte) ok, options(nomem, nostack));
        }
        if ok != 0 {
            return Some(value);
        }
    }
    None
}

/// Add the moment something happened to the pool. Called on every
/// interrupt; cheap and lock-free.
pub fn stir() {
    let t = cycles();
    let old = POOL.load(Ordering::Relaxed);
    POOL.store(old.rotate_left(13) ^ t.wrapping_mul(0x9E37_79B9_7F4A_7C15), Ordering::Relaxed);
}

fn quarter(s: &mut [u32; 16], a: usize, b: usize, c: usize, d: usize) {
    s[a] = s[a].wrapping_add(s[b]);
    s[d] = (s[d] ^ s[a]).rotate_left(16);
    s[c] = s[c].wrapping_add(s[d]);
    s[b] = (s[b] ^ s[c]).rotate_left(12);
    s[a] = s[a].wrapping_add(s[b]);
    s[d] = (s[d] ^ s[a]).rotate_left(8);
    s[c] = s[c].wrapping_add(s[d]);
    s[b] = (s[b] ^ s[c]).rotate_left(7);
}

/// One 64-byte ChaCha20 block.
fn block(key: &[u32; 8], counter: u64) -> [u32; 16] {
    let mut s = [0u32; 16];
    s[0..4].copy_from_slice(&[0x6170_7865, 0x3320_646E, 0x7962_2D32, 0x6B20_6574]); // "expand 32-byte k"
    s[4..12].copy_from_slice(key);
    s[12] = counter as u32;
    s[13] = (counter >> 32) as u32;
    let start = s;
    for _ in 0..10 {
        quarter(&mut s, 0, 4, 8, 12);
        quarter(&mut s, 1, 5, 9, 13);
        quarter(&mut s, 2, 6, 10, 14);
        quarter(&mut s, 3, 7, 11, 15);
        quarter(&mut s, 0, 5, 10, 15);
        quarter(&mut s, 1, 6, 11, 12);
        quarter(&mut s, 2, 7, 8, 13);
        quarter(&mut s, 3, 4, 9, 14);
    }
    for i in 0..16 {
        s[i] = s[i].wrapping_add(start[i]);
    }
    s
}

/// Seed the generator. Called once at start-up.
pub fn init(extra: u64) {
    let mut g = GENERATOR.lock();
    let hardware = has_rdrand();
    for i in 0..8 {
        // Several counter reads with work in between, so their low bits
        // differ by however long that work happened to take.
        let mut v = cycles() ^ extra.rotate_left(i as u32 * 8);
        for _ in 0..(v & 0xFF) + 64 {
            v = v.rotate_left(5) ^ cycles();
        }
        if hardware {
            v ^= rdrand().unwrap_or(0);
        }
        g.key[i] ^= v as u32 ^ (v >> 32) as u32;
    }
    g.counter = cycles();
}

/// Fill `out` with random bytes.
pub fn fill(out: &mut [u8]) {
    let mut g = GENERATOR.lock();
    // Fold in whatever the pool has gathered since last time.
    let pool = POOL.load(Ordering::Relaxed) ^ cycles();
    let hardware = if has_rdrand() { rdrand().unwrap_or(0) } else { 0 };
    g.key[0] ^= pool as u32;
    g.key[1] ^= (pool >> 32) as u32;
    g.key[2] ^= hardware as u32;
    g.key[3] ^= (hardware >> 32) as u32;
    for chunk in out.chunks_mut(64) {
        let b = block(&g.key, g.counter);
        g.counter = g.counter.wrapping_add(1);
        for (i, byte) in chunk.iter_mut().enumerate() {
            *byte = (b[i / 4] >> (8 * (i % 4))) as u8;
        }
    }
    // New key from a block nobody sees.
    let next = block(&g.key, g.counter);
    g.counter = g.counter.wrapping_add(1);
    g.key.copy_from_slice(&next[..8]);
}
