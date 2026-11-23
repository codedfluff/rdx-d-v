# RDX

A hobby operating system for x86-64 PCs, written from scratch. The kernel is
Rust and boots straight from UEFI; the programs that run on it are C, built
against RDX's own small C library. It runs in QEMU (and the code is written
with real hardware in mind).

What's in it:

- **Kernel:** paging, a preemptive scheduler on multiple cores, user processes
  in ring 3, pipes, pseudo-terminals, shared memory, and a filesystem kept in
  memory and saved to a data disk.
- **Drivers:** PS/2 and USB (xHCI) keyboards, mouse/tablet, AHCI disks, an
  e1000 network card, ACPI and the framebuffer.
- **Networking:** TCP/UDP, DHCP and DNS. `wget`, `nc`, `ping` and a small web
  server are included.
- **TLS:** TLS 1.3 and 1.2 with certificate checking (`user/tls.c`,
  `user/x509.c`).
- **Shell:** pipes, redirection, variables, background jobs, and about 80
  command-line programs.
- **Desktop:** a graphical desktop with windows: terminal, file manager,
  system monitor, paint, image viewer.
- **Languages:** a small scripting language (`rx`) and a C compiler that runs
  inside RDX (`c-comp`).
- **Web browser** (`user/programs/browser.c` and `user/web/`):
  - HTML parsing and the CSS cascade;
  - layout: block, inline, flex, grid and tables;
  - TrueType and web fonts (WOFF, WOFF2);
  - backgrounds, gradients, rounded corners and shadows;
  - a JavaScript engine with a DOM, `fetch`, iframes and workers.

## Building and running

Windows is needed for the build scripts. Install:

- [Rust](https://rustup.rs), then `rustup target add x86_64-unknown-uefi`
- LLVM (clang and lld-link): `winget install LLVM.LLVM`
- QEMU, which includes the UEFI firmware: `winget install SoftwareFreedomConservancy.QEMU`
- Python 3, only for the tests and tools

Then:

```
build.cmd        builds everything into build\
run.cmd          builds and boots RDX in QEMU
run.cmd nobuild  boots the last build
```

Inside RDX, type `help` for the commands, or `desktop` for the graphical
desktop. Files you create are saved to `build\disk.img`. RDX only ever
writes to a disk whose first sector starts with `RDXDISK1`, so it leaves
other disks alone.

## Tests

```
python tools/smoke.py
```

This boots RDX several times in QEMU and checks around 200 things: the
shell, the filesystem, processes, networking, TLS, the desktop and the
browser. It takes 10 to 15 minutes. To run a few commands in a fresh VM:

```
python tools/try.py "ls" "cat /proc/version"
```

`tools/layoutdiff.py` compares how RDX and headless Chrome lay out a page,
element by element.

## Layout

```
src/            the kernel (Rust)
user/           the C library and shared code (gui, images, inflate, brotli, tls, x509)
user/programs/  every program; each .c file becomes a .exe
user/web/       the browser engine: HTML, CSS, layout, painting, fonts, JavaScript
user/files/     copied onto the boot disk: start page, scripts, fonts, CA certificates
tools/          test scripts and generators
tools/webtests/ pages for the browser tests
```

## Third-party data

These files in `user/files` come from other projects, under their own licences:

- the Liberation fonts (SIL Open Font License; `liberation-license.txt`)
- DejaVu Sans (`dejavu-license.txt`)
- the Brotli dictionary (MIT; `brotli-license.txt`)
- `roots.pem`, Mozilla's CA certificate list (MPL 2.0)

Everything else was written for RDX.
