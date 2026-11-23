//! The network stack, on top of the Ethernet driver.
//!
//! One kernel thread polls the adapter for incoming frames and drives:
//!
//!   * ARP — answering "who has my address?" and learning other machines'
//!     hardware addresses;
//!   * IPv4 with ICMP echo, so the machine answers pings and can send them;
//!   * UDP — datagram sockets, plus the two built-in users of UDP: the DHCP
//!     client that configures the machine, and the DNS resolver that turns
//!     names into addresses;
//!   * TCP — reliable connections, outgoing (connect) and incoming (listen
//!     and accept), with retransmission and flow control.
//!
//! Programs reach all of this through sockets, which they hold as ordinary
//! file descriptors (see pipe.rs and syscall.rs). Nothing here blocks: every
//! operation either completes or says "not yet", and the system call layer
//! turns "not yet" into a short sleep and a retry.
//!
//! Simplifications worth knowing about: no IP fragmentation, no TCP
//! congestion control or out-of-order reassembly (a segment that arrives
//! early is dropped and retransmitted by the sender), and no IPv6.

use alloc::collections::VecDeque;
use alloc::format;
use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;

use crate::e1000::Nic;
use crate::sync::IrqLock;
use crate::{arch, sched, serial};

pub type Ip = [u8; 4];
type Mac = [u8; 6];

const BROADCAST_MAC: Mac = [0xFF; 6];
const ETHERTYPE_IP: u16 = 0x0800;
const ETHERTYPE_ARP: u16 = 0x0806;
const PROTO_ICMP: u8 = 1;
const PROTO_TCP: u8 = 6;
const PROTO_UDP: u8 = 17;

/// Local UDP port the DNS resolver sends its questions from.
const DNS_PORT: u16 = 53535;

// ---------------------------------------------------------------- helpers

/// The Internet checksum: one's-complement sum of 16-bit big-endian words.
fn checksum(data: &[u8]) -> u16 {
    let mut sum = 0u32;
    for pair in data.chunks(2) {
        sum += (pair[0] as u32) << 8 | *pair.get(1).unwrap_or(&0) as u32;
    }
    while sum >> 16 != 0 {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    !(sum as u16)
}

pub fn format_ip(ip: Ip) -> String {
    format!("{}.{}.{}.{}", ip[0], ip[1], ip[2], ip[3])
}

pub fn parse_ip(text: &str) -> Option<Ip> {
    let mut ip = [0u8; 4];
    let mut parts = text.split('.');
    for slot in ip.iter_mut() {
        *slot = parts.next()?.parse().ok()?;
    }
    parts.next().is_none().then_some(ip)
}

fn ms(milliseconds: u64) -> u64 {
    sched::ms_to_ticks(milliseconds)
}

// ---------------------------------------------------------------- TCP

const TCP_FIN: u8 = 0x01;
const TCP_SYN: u8 = 0x02;
const TCP_RST: u8 = 0x04;
const TCP_PSH: u8 = 0x08;
const TCP_ACK: u8 = 0x10;

/// How much we accept before the application reads it. More than 64 KiB
/// needs window scaling (RFC 7323): without it, a download can go no faster
/// than 64 KiB per round trip.
const RECV_CAPACITY: usize = 1024 * 1024;
/// The window scale we offer: windows we advertise count in units of 2^5.
const OUR_SHIFT: u8 = 5;
const SEND_CAPACITY: usize = 64 * 1024;
/// Most data we keep in flight at once, whatever window the peer offers.
const MAX_IN_FLIGHT: usize = 32 * 1024;
const OUR_MSS: usize = 1460;

#[derive(PartialEq, Clone, Copy)]
enum TcpState {
    /// We sent a SYN and await the peer's SYN+ACK.
    SynSent,
    /// A peer's SYN arrived on a listening port; we answered SYN+ACK.
    SynReceived,
    Established,
    Closed,
}

/// A packet waiting to be handed to the IP layer.
struct Outgoing {
    to: Ip,
    protocol: u8,
    payload: Vec<u8>,
}

/// One end of a TCP connection.
struct Tcp {
    state: TcpState,
    local_port: u16,
    remote: Ip,
    remote_port: u16,
    /// Our initial sequence number.
    iss: u32,
    /// Oldest sequence number the peer has not acknowledged yet.
    snd_una: u32,
    /// Next sequence number we will send.
    snd_nxt: u32,
    /// How much the peer says it can take.
    snd_wnd: usize,
    peer_mss: usize,
    /// Bytes not yet acknowledged, starting at sequence number `snd_una`.
    send_buf: VecDeque<u8>,
    /// Next sequence number we expect from the peer.
    rcv_nxt: u32,
    recv_buf: VecDeque<u8>,
    /// The application has finished sending: a FIN should follow the data.
    fin_wanted: bool,
    fin_sent: bool,
    fin_acked: bool,
    /// The peer has finished sending.
    peer_fin: bool,
    /// The connection was refused, reset or timed out.
    failed: bool,
    /// When to retransmit if nothing has been acknowledged by then.
    retransmit_at: u64,
    retries: u32,
    /// For an incoming connection: the listening socket it belongs to,
    /// until the application accepts it.
    listener: Option<u32>,
    /// The application has closed its descriptor; free this when done.
    orphaned_at: Option<u64>,
    /// The receive window we last told the peer about. When reading frees
    /// a good deal more room than that, the peer must be told (a "window
    /// update"): otherwise, after we have said "full", it waits on its own
    /// slow timer before trying again.
    advertised: core::cell::Cell<usize>,
    /// Whether our SYN offers window scaling (an incoming connection's
    /// answer does only if the peer's SYN offered it).
    offer_scale: bool,
    /// Agreed in the handshake: the shift for the windows we advertise, and
    /// for the ones the peer advertises (both 0 without scaling).
    rcv_shift: u8,
    snd_shift: u8,
}

impl Tcp {
    fn new(state: TcpState, local_port: u16, remote: Ip, remote_port: u16, iss: u32) -> Tcp {
        Tcp {
            state,
            local_port,
            remote,
            remote_port,
            iss,
            snd_una: iss,
            snd_nxt: iss,
            snd_wnd: 0,
            peer_mss: 536,
            send_buf: VecDeque::new(),
            rcv_nxt: 0,
            recv_buf: VecDeque::new(),
            fin_wanted: false,
            fin_sent: false,
            fin_acked: false,
            peer_fin: false,
            failed: false,
            retransmit_at: 0,
            retries: 0,
            listener: None,
            orphaned_at: None,
            advertised: core::cell::Cell::new(RECV_CAPACITY),
            offer_scale: true,
            rcv_shift: 0,
            snd_shift: 0,
        }
    }

    fn retransmit_delay(&self) -> u64 {
        ms(300 << self.retries.min(4))
    }

    /// Build one segment and queue it. `sequence` is where it starts.
    fn emit(&self, local: Ip, flags: u8, sequence: u32, data: &[u8], out: &mut Vec<Outgoing>) {
        let with_mss = flags & TCP_SYN != 0;
        let with_scale = with_mss && self.offer_scale;
        let header = if with_scale { 28 } else if with_mss { 24 } else { 20 };
        let mut s = vec![0u8; header];
        s[0..2].copy_from_slice(&self.local_port.to_be_bytes());
        s[2..4].copy_from_slice(&self.remote_port.to_be_bytes());
        s[4..8].copy_from_slice(&sequence.to_be_bytes());
        if flags & TCP_ACK != 0 {
            s[8..12].copy_from_slice(&self.rcv_nxt.to_be_bytes());
        }
        s[12] = ((header / 4) as u8) << 4;
        s[13] = flags;
        // (the window in a SYN is never scaled)
        let room = RECV_CAPACITY - self.recv_buf.len();
        let shift = if flags & TCP_SYN != 0 { 0 } else { self.rcv_shift };
        let window = (room >> shift).min(65535) as u16;
        s[14..16].copy_from_slice(&window.to_be_bytes());
        self.advertised.set((window as usize) << shift);
        if with_mss {
            s[20] = 2; // option: maximum segment size
            s[21] = 4;
            s[22..24].copy_from_slice(&(OUR_MSS as u16).to_be_bytes());
        }
        if with_scale {
            s[24] = 1; // no-op (padding)
            s[25] = 3; // option: window scale
            s[26] = 3;
            s[27] = OUR_SHIFT;
        }
        s.extend_from_slice(data);
        // The checksum also covers a "pseudo header" of the IP addresses.
        let mut pseudo = Vec::with_capacity(12 + s.len());
        pseudo.extend_from_slice(&local);
        pseudo.extend_from_slice(&self.remote);
        pseudo.extend_from_slice(&[0, PROTO_TCP]);
        pseudo.extend_from_slice(&(s.len() as u16).to_be_bytes());
        pseudo.extend_from_slice(&s);
        let sum = checksum(&pseudo);
        s[16..18].copy_from_slice(&sum.to_be_bytes());
        out.push(Outgoing { to: self.remote, protocol: PROTO_TCP, payload: s });
    }

    fn fail(&mut self) {
        self.failed = true;
        self.state = TcpState::Closed;
    }

    /// Send whatever can be sent now: handshake retries, new data, the FIN,
    /// and retransmissions of anything that has gone unacknowledged too long.
    fn output(&mut self, local: Ip, now: u64, out: &mut Vec<Outgoing>) {
        match self.state {
            TcpState::Closed => {}
            TcpState::SynSent | TcpState::SynReceived => {
                if now >= self.retransmit_at {
                    if self.retries >= 6 {
                        return self.fail(); // nobody is answering
                    }
                    let flags = if self.state == TcpState::SynSent { TCP_SYN } else { TCP_SYN | TCP_ACK };
                    self.emit(local, flags, self.iss, &[], out);
                    self.retransmit_at = now + self.retransmit_delay();
                    self.retries += 1;
                }
            }
            TcpState::Established => {
                // The application has read enough to reopen a window we
                // said was (nearly) closed: say so straight away.
                let room = RECV_CAPACITY - self.recv_buf.len();
                let threshold = (RECV_CAPACITY / 2).min(2 * OUR_MSS);
                if !self.peer_fin && room >= self.advertised.get() + threshold {
                    self.emit(local, TCP_ACK, self.snd_nxt, &[], out);
                }
                // Timed out waiting for an acknowledgement: go back and send
                // again from the first unacknowledged byte.
                if self.snd_nxt != self.snd_una && now >= self.retransmit_at {
                    if self.retries >= 8 {
                        self.emit(local, TCP_RST, self.snd_nxt, &[], out);
                        return self.fail();
                    }
                    self.retries += 1;
                    self.snd_nxt = self.snd_una;
                    self.fin_sent = false;
                    self.retransmit_at = now + self.retransmit_delay();
                }
                if self.fin_sent {
                    return; // everything, including the FIN, is on its way
                }
                let window = self.snd_wnd.clamp(1, MAX_IN_FLIGHT);
                let mss = self.peer_mss.min(OUR_MSS);
                loop {
                    let in_flight = self.snd_nxt.wrapping_sub(self.snd_una) as usize;
                    if in_flight >= self.send_buf.len() || in_flight >= window {
                        break;
                    }
                    let n = mss.min(self.send_buf.len() - in_flight).min(window - in_flight);
                    let data: Vec<u8> = self.send_buf.range(in_flight..in_flight + n).copied().collect();
                    if in_flight == 0 {
                        self.retransmit_at = now + self.retransmit_delay();
                    }
                    self.emit(local, TCP_ACK | TCP_PSH, self.snd_nxt, &data, out);
                    self.snd_nxt = self.snd_nxt.wrapping_add(n as u32);
                }
                let in_flight = self.snd_nxt.wrapping_sub(self.snd_una) as usize;
                if self.fin_wanted && in_flight == self.send_buf.len() {
                    if in_flight == 0 {
                        self.retransmit_at = now + self.retransmit_delay();
                    }
                    self.emit(local, TCP_FIN | TCP_ACK, self.snd_nxt, &[], out);
                    self.snd_nxt = self.snd_nxt.wrapping_add(1);
                    self.fin_sent = true;
                }
            }
        }
    }

    /// Process one arriving segment. Returns true if this segment completed
    /// an incoming connection's handshake.
    #[allow(clippy::too_many_arguments)]
    fn input(
        &mut self,
        local: Ip,
        now: u64,
        sequence: u32,
        acknowledged: u32,
        flags: u8,
        window: usize,
        mss: Option<usize>,
        scale: Option<u8>,
        data: &[u8],
        out: &mut Vec<Outgoing>,
    ) -> bool {
        let mut just_established = false;
        // (windows in SYN segments are never scaled)
        let window = if flags & TCP_SYN != 0 { window } else { window << self.snd_shift };
        if flags & TCP_RST != 0 {
            // During connect, only a reset that answers our SYN counts.
            if self.state != TcpState::SynSent || (flags & TCP_ACK != 0 && acknowledged == self.iss.wrapping_add(1)) {
                self.fail();
            }
            return false;
        }
        match self.state {
            TcpState::Closed => return false,
            TcpState::SynSent => {
                if flags & (TCP_SYN | TCP_ACK) == TCP_SYN | TCP_ACK && acknowledged == self.iss.wrapping_add(1) {
                    self.rcv_nxt = sequence.wrapping_add(1);
                    self.snd_una = acknowledged;
                    self.snd_nxt = acknowledged;
                    self.snd_wnd = window;
                    self.peer_mss = mss.unwrap_or(536);
                    // Scaling applies only if both SYNs offered it.
                    if let (Some(theirs), true) = (scale, self.offer_scale) {
                        self.snd_shift = theirs.min(14);
                        self.rcv_shift = OUR_SHIFT;
                    }
                    self.state = TcpState::Established;
                    self.retries = 0;
                    self.emit(local, TCP_ACK, self.snd_nxt, &[], out);
                }
                return false;
            }
            TcpState::SynReceived => {
                if flags & TCP_ACK != 0 && acknowledged == self.iss.wrapping_add(1) {
                    self.snd_una = acknowledged;
                    self.snd_nxt = acknowledged;
                    self.snd_wnd = window;
                    self.state = TcpState::Established;
                    self.retries = 0;
                    just_established = true;
                    // Fall through: the same segment may carry data.
                } else {
                    return false;
                }
            }
            TcpState::Established => {}
        }

        // What of ours has the peer received?
        if flags & TCP_ACK != 0 {
            let newly = acknowledged.wrapping_sub(self.snd_una);
            let outstanding = self.snd_nxt.wrapping_sub(self.snd_una);
            if newly > 0 && newly <= outstanding {
                let mut bytes = newly as usize;
                if self.fin_sent && acknowledged == self.snd_nxt {
                    self.fin_acked = true;
                    bytes -= 1; // the FIN takes a sequence number but no buffer space
                }
                self.send_buf.drain(..bytes.min(self.send_buf.len()));
                self.snd_una = acknowledged;
                self.retries = 0;
                self.retransmit_at = now + self.retransmit_delay();
            }
            self.snd_wnd = window;
        }

        // What has the peer sent us? Only data that continues exactly where
        // the last ended is accepted; anything else just gets re-acknowledged.
        let mut reply = false;
        if !data.is_empty() {
            if sequence == self.rcv_nxt {
                let n = data.len().min(RECV_CAPACITY - self.recv_buf.len());
                self.recv_buf.extend(&data[..n]);
                self.rcv_nxt = self.rcv_nxt.wrapping_add(n as u32);
            }
            reply = true;
        }
        if flags & TCP_FIN != 0 {
            if !self.peer_fin && sequence.wrapping_add(data.len() as u32) == self.rcv_nxt {
                self.rcv_nxt = self.rcv_nxt.wrapping_add(1);
                self.peer_fin = true;
            }
            reply = true;
        }
        if reply {
            self.emit(local, TCP_ACK, self.snd_nxt, &[], out);
        }
        if self.fin_acked && self.peer_fin {
            self.state = TcpState::Closed; // both directions finished cleanly
        }
        just_established
    }
}

// ---------------------------------------------------------------- sockets

enum Kind {
    /// A UDP port, with datagrams that have arrived: (from, port, data).
    Udp { port: u16, queue: VecDeque<(Ip, u16, Vec<u8>)> },
    Tcp(Tcp),
    /// A TCP port accepting connections; `ready` are established ones the
    /// application has not collected yet.
    Listener { port: u16, ready: VecDeque<u32> },
}

struct Socket {
    id: u32,
    kind: Kind,
}

/// What a program's descriptor holds. Closing the last copy closes the socket.
pub struct SocketHandle(u32);

impl SocketHandle {
    pub fn id(&self) -> u32 {
        self.0
    }
}

impl Drop for SocketHandle {
    fn drop(&mut self) {
        close(self.0);
    }
}

/// Result of a read or write on a socket.
pub enum Io {
    Done(usize),
    WouldBlock,
    /// Reading: the peer has finished and everything has been read.
    /// Writing: the connection is gone.
    Closed,
}

pub enum Connect {
    Connecting,
    Connected,
    Failed,
}

pub enum Lookup {
    Found(Ip),
    Pending,
    Failed,
}

struct DnsQuery {
    name: String,
    id: u16,
    started: u64,
    last_sent: u64,
    /// None while waiting; then the answer (or None for "no such name").
    result: Option<Option<Ip>>,
}

// ---------------------------------------------------------------- the stack

#[derive(PartialEq, Clone, Copy)]
enum Dhcp {
    /// Asking whether any server is out there.
    Discovering,
    /// A server offered an address; we have asked to take it.
    Requesting { server: Ip, offered: Ip },
    /// We have an address.
    Bound,
}

struct Net {
    nic: Nic,
    ip: Ip,
    mask: Ip,
    gateway: Ip,
    dns: Ip,
    dhcp: Dhcp,
    dhcp_id: u32,
    dhcp_next_try: u64,
    /// Hardware addresses we have learned.
    arp: Vec<(Ip, Mac)>,
    /// Addresses we have recently asked about: (address, when).
    arp_asked: Vec<(Ip, u64)>,
    /// Echo replies received: (sequence number, tick it arrived).
    echo_replies: Vec<(u16, u64)>,
    sockets: Vec<Socket>,
    next_socket: u32,
    next_port: u16,
    dns_queries: Vec<DnsQuery>,
    sent: u64,
    received: u64,
}

static NET: IrqLock<Option<Net>> = IrqLock::new(None);

impl Net {
    fn send_frame(&mut self, to: Mac, ethertype: u16, payload: &[u8]) {
        let mut frame = Vec::with_capacity(14 + payload.len());
        frame.extend_from_slice(&to);
        frame.extend_from_slice(&self.nic.mac);
        frame.extend_from_slice(&ethertype.to_be_bytes());
        frame.extend_from_slice(payload);
        if self.nic.send(&frame) {
            self.sent += 1;
        }
    }

    /// Wrap `payload` in an IPv4 header and send it to hardware address `to`.
    fn send_ip(&mut self, to: Mac, source: Ip, destination: Ip, protocol: u8, payload: &[u8]) {
        let total = (20 + payload.len()) as u16;
        let mut packet = vec![0x45, 0, 0, 0, 0, 0, 0x40, 0, 64, protocol, 0, 0];
        packet[2..4].copy_from_slice(&total.to_be_bytes());
        packet.extend_from_slice(&source);
        packet.extend_from_slice(&destination);
        let sum = checksum(&packet);
        packet[10..12].copy_from_slice(&sum.to_be_bytes());
        packet.extend_from_slice(payload);
        self.send_frame(to, ETHERTYPE_IP, &packet);
    }

    /// ARP: operation 1 = "who has `target_ip`?", 2 = "I do".
    fn send_arp(&mut self, operation: u16, target_mac: Mac, target_ip: Ip) {
        let mut packet = vec![0, 1, 8, 0, 6, 4]; // Ethernet, IPv4, address sizes
        packet.extend_from_slice(&operation.to_be_bytes());
        packet.extend_from_slice(&self.nic.mac);
        packet.extend_from_slice(&self.ip);
        packet.extend_from_slice(&if operation == 1 { [0; 6] } else { target_mac });
        packet.extend_from_slice(&target_ip);
        self.send_frame(if operation == 1 { BROADCAST_MAC } else { target_mac }, ETHERTYPE_ARP, &packet);
    }

    fn learn(&mut self, ip: Ip, mac: Mac) {
        if ip == [0; 4] {
            return;
        }
        match self.arp.iter_mut().find(|(known, _)| *known == ip) {
            Some(entry) => entry.1 = mac,
            None => {
                if self.arp.len() >= 32 {
                    self.arp.remove(0);
                }
                self.arp.push((ip, mac));
            }
        }
    }

    /// Which machine on the local network a packet for `ip` goes to: the
    /// destination itself if it is on our network, otherwise the gateway.
    fn next_hop(&self, ip: Ip) -> Ip {
        let local = (0..4).all(|i| ip[i] & self.mask[i] == self.ip[i] & self.mask[i]);
        if local || self.gateway == [0; 4] { ip } else { self.gateway }
    }

    /// Hardware address to use for a packet to `ip`. If it is not known yet
    /// this asks for it (at most every 200 ms) and returns None; the caller
    /// drops the packet and whatever sent it tries again shortly.
    fn mac_for(&mut self, ip: Ip) -> Option<Mac> {
        let hop = self.next_hop(ip);
        if let Some(&(_, mac)) = self.arp.iter().find(|(known, _)| *known == hop) {
            return Some(mac);
        }
        let now = arch::ticks();
        let recently = self.arp_asked.iter().any(|&(asked, when)| asked == hop && now < when + ms(200));
        if !recently {
            self.arp_asked.retain(|&(asked, when)| asked != hop && now < when + ms(5000));
            self.arp_asked.push((hop, now));
            self.send_arp(1, [0; 6], hop);
        }
        None
    }

    /// Send a queued packet, if its next hop's hardware address is known.
    fn deliver(&mut self, packet: Outgoing) -> bool {
        match self.mac_for(packet.to) {
            Some(mac) => {
                let ip = self.ip;
                self.send_ip(mac, ip, packet.to, packet.protocol, &packet.payload);
                true
            }
            None => false,
        }
    }

    fn send_udp(&mut self, to: Ip, from_port: u16, to_port: u16, data: &[u8]) -> bool {
        let mut udp = Vec::with_capacity(8 + data.len());
        udp.extend_from_slice(&from_port.to_be_bytes());
        udp.extend_from_slice(&to_port.to_be_bytes());
        udp.extend_from_slice(&((8 + data.len()) as u16).to_be_bytes());
        udp.extend_from_slice(&[0, 0]); // checksum is optional over IPv4
        udp.extend_from_slice(data);
        self.deliver(Outgoing { to, protocol: PROTO_UDP, payload: udp })
    }

    // ------------------------------------------------------------ DHCP

    fn send_dhcp(&mut self, message_type: u8, requested: Option<(Ip, Ip)>) {
        // BOOTP request header, then options.
        let mut m = vec![0u8; 240];
        m[0] = 1; // request
        m[1] = 1; // Ethernet
        m[2] = 6; // hardware address length
        m[4..8].copy_from_slice(&self.dhcp_id.to_be_bytes());
        m[10] = 0x80; // please reply by broadcast: we have no address yet
        m[28..34].copy_from_slice(&self.nic.mac);
        m[236..240].copy_from_slice(&[99, 130, 83, 99]); // magic cookie
        m.extend_from_slice(&[53, 1, message_type]);
        if let Some((offered, server)) = requested {
            m.extend_from_slice(&[50, 4]);
            m.extend_from_slice(&offered);
            m.extend_from_slice(&[54, 4]);
            m.extend_from_slice(&server);
        }
        m.extend_from_slice(&[55, 3, 1, 3, 6]); // ask for: mask, router, DNS
        m.push(255);

        let mut udp = vec![0, 68, 0, 67, 0, 0, 0, 0]; // from port 68 to 67, no checksum
        let length = (8 + m.len()) as u16;
        udp[4..6].copy_from_slice(&length.to_be_bytes());
        udp.extend_from_slice(&m);
        self.send_ip(BROADCAST_MAC, [0; 4], [255; 4], PROTO_UDP, &udp);
    }

    fn handle_dhcp(&mut self, m: &[u8]) {
        if m.len() < 240 || m[0] != 2 || m[4..8] != self.dhcp_id.to_be_bytes() || m[236..240] != [99, 130, 83, 99] {
            return;
        }
        let offered: Ip = m[16..20].try_into().unwrap();
        let (mut kind, mut server, mut mask, mut router, mut dns) = (0u8, [0u8; 4], [0u8; 4], [0u8; 4], [0u8; 4]);
        let mut i = 240;
        while i + 1 < m.len() && m[i] != 255 {
            if m[i] == 0 {
                i += 1; // padding
                continue;
            }
            let (option, len) = (m[i], m[i + 1] as usize);
            let Some(value) = m.get(i + 2..i + 2 + len) else { break };
            match (option, len) {
                (53, 1) => kind = value[0],
                (54, 4) => server.copy_from_slice(value),
                (1, 4) => mask.copy_from_slice(value),
                (3, n) if n >= 4 => router.copy_from_slice(&value[..4]),
                (6, n) if n >= 4 => dns.copy_from_slice(&value[..4]),
                _ => {}
            }
            i += 2 + len;
        }
        match (self.dhcp, kind) {
            // An offer: ask for exactly that address from that server.
            (Dhcp::Discovering, 2) => {
                self.dhcp = Dhcp::Requesting { server, offered };
                self.send_dhcp(3, Some((offered, server)));
                self.dhcp_next_try = arch::ticks() + ms(3000);
            }
            // The acknowledgement: the address is ours.
            (Dhcp::Requesting { .. }, 5) => {
                self.ip = offered;
                self.mask = mask;
                self.gateway = router;
                self.dns = dns;
                self.dhcp = Dhcp::Bound;
                serial::write_str(&format!("[net] address {} via DHCP\n", format_ip(offered)));
            }
            // Refused: start over.
            (Dhcp::Requesting { .. }, 6) => self.dhcp = Dhcp::Discovering,
            _ => {}
        }
    }

    // ------------------------------------------------------------ DNS

    /// Ask the DNS server for the IPv4 address ("A" record) of a name.
    fn send_dns_query(&mut self, id: u16, name: &str) {
        let mut q = Vec::new();
        q.extend_from_slice(&id.to_be_bytes());
        q.extend_from_slice(&[0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0]); // recursion wanted; 1 question
        for label in name.split('.').filter(|l| !l.is_empty()) {
            q.push(label.len().min(63) as u8);
            q.extend_from_slice(&label.as_bytes()[..label.len().min(63)]);
        }
        q.extend_from_slice(&[0, 0, 1, 0, 1]); // end of name; type A; class IN
        let server = self.dns;
        self.send_udp(server, DNS_PORT, 53, &q);
    }

    fn handle_dns(&mut self, m: &[u8]) {
        if m.len() < 12 {
            return;
        }
        let id = u16::from_be_bytes([m[0], m[1]]);
        let Some(query) = self.dns_queries.iter_mut().find(|q| q.id == id && q.result.is_none()) else { return };
        let (questions, answers) = (u16::from_be_bytes([m[4], m[5]]), u16::from_be_bytes([m[6], m[7]]));
        // Names are sequences of length-prefixed labels ending in 0, or a
        // two-byte pointer (top bits 11) to an earlier name.
        let skip_name = |mut i: usize| -> Option<usize> {
            loop {
                let len = *m.get(i)? as usize;
                if len & 0xC0 == 0xC0 {
                    return Some(i + 2);
                }
                if len == 0 {
                    return Some(i + 1);
                }
                i += 1 + len;
            }
        };
        let mut found = None;
        let mut i = 12;
        let parsed = (|| {
            for _ in 0..questions {
                i = skip_name(i)? + 4;
            }
            for _ in 0..answers {
                i = skip_name(i)?;
                let header = m.get(i..i + 10)?;
                let kind = u16::from_be_bytes([header[0], header[1]]);
                let len = u16::from_be_bytes([header[8], header[9]]) as usize;
                let data = m.get(i + 10..i + 10 + len)?;
                if kind == 1 && len == 4 && found.is_none() {
                    found = Some([data[0], data[1], data[2], data[3]]);
                }
                i += 10 + len;
            }
            Some(())
        })();
        let _ = parsed;
        query.result = Some(found);
    }

    fn lookup(&mut self, name: &str) -> Lookup {
        if let Some(ip) = parse_ip(name) {
            return Lookup::Found(ip);
        }
        if self.dhcp != Dhcp::Bound || self.dns == [0; 4] || name.is_empty() || name.len() > 253 {
            return Lookup::Failed;
        }
        let now = arch::ticks();
        // Forget answers after a minute and failures after a few seconds.
        self.dns_queries.retain(|q| match q.result {
            Some(Some(_)) => now < q.started + ms(60_000),
            Some(None) => now < q.started + ms(8_000),
            None => true,
        });
        if let Some(i) = self.dns_queries.iter().position(|q| q.name == name) {
            match self.dns_queries[i].result {
                Some(Some(ip)) => return Lookup::Found(ip),
                Some(None) => return Lookup::Failed,
                None => {}
            }
            if now > self.dns_queries[i].started + ms(4000) {
                self.dns_queries[i].result = Some(None); // the server never answered
                return Lookup::Failed;
            }
            if now >= self.dns_queries[i].last_sent + ms(250) {
                self.dns_queries[i].last_sent = now;
                let id = self.dns_queries[i].id;
                self.send_dns_query(id, name);
            }
            return Lookup::Pending;
        }
        if self.dns_queries.len() >= 16 {
            self.dns_queries.remove(0);
        }
        let id = (now as u16).wrapping_mul(31_337).wrapping_add(self.next_socket as u16) | 1;
        self.dns_queries.push(DnsQuery { name: String::from(name), id, started: now, last_sent: now, result: None });
        self.send_dns_query(id, name);
        Lookup::Pending
    }

    // ------------------------------------------------------------ receiving

    fn handle_udp(&mut self, source: Ip, segment: &[u8]) {
        if segment.len() < 8 {
            return;
        }
        let from_port = u16::from_be_bytes([segment[0], segment[1]]);
        let to_port = u16::from_be_bytes([segment[2], segment[3]]);
        let length = (u16::from_be_bytes([segment[4], segment[5]]) as usize).clamp(8, segment.len());
        let data = &segment[8..length];
        match to_port {
            68 => self.handle_dhcp(data),
            DNS_PORT => self.handle_dns(data),
            _ => {
                for socket in self.sockets.iter_mut() {
                    if let Kind::Udp { port, queue } = &mut socket.kind {
                        if *port == to_port && queue.len() < 64 {
                            queue.push_back((source, from_port, data.to_vec()));
                        }
                    }
                }
            }
        }
    }

    fn handle_tcp(&mut self, source: Ip, segment: &[u8], out: &mut Vec<Outgoing>) {
        if segment.len() < 20 {
            return;
        }
        let from_port = u16::from_be_bytes([segment[0], segment[1]]);
        let to_port = u16::from_be_bytes([segment[2], segment[3]]);
        let sequence = u32::from_be_bytes(segment[4..8].try_into().unwrap());
        let acknowledged = u32::from_be_bytes(segment[8..12].try_into().unwrap());
        let header = ((segment[12] >> 4) as usize) * 4;
        let flags = segment[13];
        let window = u16::from_be_bytes([segment[14], segment[15]]) as usize;
        if header < 20 || header > segment.len() {
            return;
        }
        let data = &segment[header..];
        // The options we care about: the peer's maximum segment size and
        // window scale (both only in SYNs).
        let mut mss = None;
        let mut scale = None;
        let mut i = 20;
        while i < header {
            match segment[i] {
                0 => break,
                1 => i += 1,
                _ if i + 1 >= header => break,
                2 if segment[i + 1] == 4 && i + 3 < header => {
                    mss = Some(u16::from_be_bytes([segment[i + 2], segment[i + 3]]) as usize);
                    i += 4;
                }
                3 if segment[i + 1] == 3 && i + 2 < header => {
                    scale = Some(segment[i + 2]);
                    i += 3;
                }
                _ => i += (segment[i + 1] as usize).max(2),
            }
        }
        if flags & TCP_SYN == 0 {
            scale = None;
        }

        let (local, now) = (self.ip, arch::ticks());
        let existing = self.sockets.iter().position(|s| match &s.kind {
            Kind::Tcp(t) => t.local_port == to_port && t.remote == source && t.remote_port == from_port,
            _ => false,
        });
        if let Some(index) = existing {
            let id = self.sockets[index].id;
            let Kind::Tcp(tcp) = &mut self.sockets[index].kind else { return };
            let established = tcp.input(local, now, sequence, acknowledged, flags, window, mss, scale, data, out);
            tcp.output(local, now, out);
            let listener = tcp.listener;
            if established {
                // Hand the finished connection to its listening socket.
                if let Some(Socket { kind: Kind::Listener { ready, .. }, .. }) =
                    self.sockets.iter_mut().find(|s| Some(s.id) == listener)
                {
                    ready.push_back(id);
                }
            }
            return;
        }

        // No such connection. A SYN to a listening port starts one.
        let listening = self.sockets.iter().find_map(|s| match &s.kind {
            Kind::Listener { port, ready } if *port == to_port && ready.len() < 16 => Some(s.id),
            _ => None,
        });
        if let (Some(listener), true) = (listening, flags & (TCP_SYN | TCP_ACK | TCP_RST) == TCP_SYN) {
            let iss = self.initial_sequence();
            let mut tcp = Tcp::new(TcpState::SynReceived, to_port, source, from_port, iss);
            tcp.rcv_nxt = sequence.wrapping_add(1);
            tcp.peer_mss = mss.unwrap_or(536);
            tcp.snd_wnd = window;
            tcp.offer_scale = scale.is_some();
            if let Some(theirs) = scale {
                tcp.snd_shift = theirs.min(14);
                tcp.rcv_shift = OUR_SHIFT;
            }
            tcp.listener = Some(listener);
            tcp.output(local, now, out); // sends the SYN+ACK
            let id = self.new_id();
            self.sockets.push(Socket { id, kind: Kind::Tcp(tcp) });
            return;
        }
        // Anything else gets a reset, so the sender knows nobody is here.
        if flags & TCP_RST == 0 {
            let mut ghost = Tcp::new(TcpState::Closed, to_port, source, from_port, 0);
            if flags & TCP_ACK != 0 {
                ghost.emit(local, TCP_RST, acknowledged, &[], out);
            } else {
                let consumed = data.len() as u32 + (flags & (TCP_SYN | TCP_FIN) != 0) as u32;
                ghost.rcv_nxt = sequence.wrapping_add(consumed);
                ghost.emit(local, TCP_RST | TCP_ACK, 0, &[], out);
            }
        }
    }

    fn handle_ip(&mut self, from: Mac, packet: &[u8], out: &mut Vec<Outgoing>) {
        if packet.len() < 20 || packet[0] >> 4 != 4 {
            return;
        }
        let header = ((packet[0] & 0x0F) as usize) * 4;
        let total = u16::from_be_bytes([packet[2], packet[3]]) as usize;
        if header < 20 || total < header || total > packet.len() {
            return;
        }
        let source: Ip = packet[12..16].try_into().unwrap();
        let destination: Ip = packet[16..20].try_into().unwrap();
        let payload = &packet[header..total];
        let for_us = destination == self.ip && self.ip != [0; 4];
        match packet[9] {
            PROTO_ICMP if for_us && payload.len() >= 8 => {
                self.learn(source, from);
                if payload[0] == 8 {
                    // Echo request: send the same bytes back as a reply.
                    let mut reply = payload.to_vec();
                    reply[0] = 0;
                    reply[2..4].fill(0);
                    let sum = checksum(&reply);
                    reply[2..4].copy_from_slice(&sum.to_be_bytes());
                    let ip = self.ip;
                    self.send_ip(from, ip, source, PROTO_ICMP, &reply);
                } else if payload[0] == 0 {
                    let sequence = u16::from_be_bytes([payload[6], payload[7]]);
                    if self.echo_replies.len() >= 64 {
                        self.echo_replies.remove(0);
                    }
                    self.echo_replies.push((sequence, arch::ticks()));
                }
            }
            // DHCP replies arrive before we have an address, so UDP is not
            // filtered on the destination here.
            PROTO_UDP => self.handle_udp(source, payload),
            PROTO_TCP if for_us => {
                self.learn(source, from);
                self.handle_tcp(source, payload, out);
            }
            _ => {}
        }
    }

    fn handle_frame(&mut self, frame: &[u8], out: &mut Vec<Outgoing>) {
        if frame.len() < 14 {
            return;
        }
        self.received += 1;
        let from: Mac = frame[6..12].try_into().unwrap();
        let payload = &frame[14..];
        match u16::from_be_bytes([frame[12], frame[13]]) {
            ETHERTYPE_ARP if payload.len() >= 28 => {
                let sender_mac: Mac = payload[8..14].try_into().unwrap();
                let sender_ip: Ip = payload[14..18].try_into().unwrap();
                let target_ip: Ip = payload[24..28].try_into().unwrap();
                self.learn(sender_ip, sender_mac);
                if u16::from_be_bytes([payload[6], payload[7]]) == 1 && target_ip == self.ip && self.ip != [0; 4] {
                    self.send_arp(2, sender_mac, sender_ip);
                }
            }
            ETHERTYPE_IP => self.handle_ip(from, payload, out),
            _ => {}
        }
    }

    // ------------------------------------------------------------ housekeeping

    fn new_id(&mut self) -> u32 {
        self.next_socket += 1;
        self.next_socket
    }

    /// A local port number nothing else is using.
    fn ephemeral_port(&mut self) -> u16 {
        loop {
            self.next_port = if self.next_port >= 65000 { 49152 } else { self.next_port + 1 };
            let candidate = self.next_port;
            let taken = self.sockets.iter().any(|s| match &s.kind {
                Kind::Udp { port, .. } | Kind::Listener { port, .. } => *port == candidate,
                Kind::Tcp(t) => t.local_port == candidate,
            });
            if !taken {
                return candidate;
            }
        }
    }

    fn initial_sequence(&mut self) -> u32 {
        (arch::ticks() as u32).wrapping_mul(2_654_435_761).wrapping_add(self.next_socket.wrapping_mul(40_503))
    }

    /// Let every connection send what it can, and free finished ones.
    fn run_tcp(&mut self, out: &mut Vec<Outgoing>) {
        let (local, now) = (self.ip, arch::ticks());
        for socket in self.sockets.iter_mut() {
            if let Kind::Tcp(tcp) = &mut socket.kind {
                tcp.output(local, now, out);
            }
        }
        // A connection whose descriptor is closed lingers until it has shut
        // down cleanly, or for ten seconds, whichever comes first.
        let mut resets = Vec::new();
        self.sockets.retain(|socket| {
            let Kind::Tcp(tcp) = &socket.kind else { return true };
            let Some(orphaned) = tcp.orphaned_at else { return true };
            if tcp.state == TcpState::Closed {
                return false;
            }
            if now > orphaned + ms(10_000) {
                resets.push((tcp.local_port, tcp.remote, tcp.remote_port, tcp.snd_nxt));
                return false;
            }
            true
        });
        for (local_port, remote, remote_port, sequence) in resets {
            Tcp::new(TcpState::Closed, local_port, remote, remote_port, 0).emit(local, TCP_RST, sequence, &[], out);
        }
    }

    fn flush(&mut self, out: Vec<Outgoing>) {
        for packet in out {
            self.deliver(packet);
        }
    }

    /// Take in what has arrived and keep everything moving. Called every
    /// tick by the network thread, and by socket calls that want promptness.
    fn poll(&mut self) {
        let mut out = Vec::new();
        let mut arrived = false;
        while let Some(frame) = self.nic.receive() {
            self.handle_frame(&frame, &mut out);
            arrived = true;
        }
        if arrived {
            // Whatever came in may be what a blocked program is waiting for.
            sched::wake(sched::Wait::Net);
        }
        let now = arch::ticks();
        if self.dhcp != Dhcp::Bound && now >= self.dhcp_next_try && self.nic.link_up() {
            self.dhcp = Dhcp::Discovering;
            self.dhcp_id = self.dhcp_id.wrapping_mul(1_103_515_245).wrapping_add(12_345 + now as u32);
            self.send_dhcp(1, None);
            self.dhcp_next_try = now + ms(3000);
        }
        self.run_tcp(&mut out);
        self.flush(out);
    }

    fn tcp_mut(&mut self, id: u32) -> Option<&mut Tcp> {
        self.sockets.iter_mut().find(|s| s.id == id).and_then(|s| match &mut s.kind {
            Kind::Tcp(tcp) => Some(tcp),
            _ => None,
        })
    }
}

fn task() {
    loop {
        if let Some(net) = NET.lock().as_mut() {
            net.poll();
        }
        sched::sleep_ms(10);
    }
}

/// Look for a network adapter and, if there is one, start the stack.
/// Returns a description of what was found.
pub fn start() -> Option<String> {
    let nic = Nic::probe()?;
    let mac = nic.mac;
    let description = format!(
        "Intel Ethernet (device {:04x}), address {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
        nic.model, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
    );
    *NET.lock() = Some(Net {
        nic,
        ip: [0; 4],
        mask: [0; 4],
        gateway: [0; 4],
        dns: [0; 4],
        dhcp: Dhcp::Discovering,
        dhcp_id: u32::from_le_bytes([mac[2], mac[3], mac[4], mac[5]]) ^ 0x5244_5800,
        dhcp_next_try: 0,
        arp: Vec::new(),
        arp_asked: Vec::new(),
        echo_replies: Vec::new(),
        sockets: Vec::new(),
        next_socket: 0,
        next_port: 49152 + (mac[5] as u16) * 16,
        dns_queries: Vec::new(),
        sent: 0,
        received: 0,
    });
    sched::spawn("net", task);
    Some(description)
}

/// Lines describing the network for the shell's `net` command.
pub fn status() -> Vec<String> {
    let guard = NET.lock();
    let Some(net) = guard.as_ref() else {
        return vec![String::from("No supported network adapter was found.")];
    };
    let m = net.nic.mac;
    let mut lines = vec![
        format!(
            "adapter:  Intel Ethernet (device {:04x}), link {}",
            net.nic.model,
            if net.nic.link_up() { "up" } else { "down" }
        ),
        format!("hardware: {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", m[0], m[1], m[2], m[3], m[4], m[5]),
    ];
    if net.dhcp == Dhcp::Bound {
        lines.push(format!("address:  {} (mask {})", format_ip(net.ip), format_ip(net.mask)));
        lines.push(format!("gateway:  {}    DNS: {}", format_ip(net.gateway), format_ip(net.dns)));
    } else {
        lines.push(String::from("address:  none yet (waiting for a DHCP server)"));
    }
    lines.push(format!("packets:  {} sent, {} received", net.sent, net.received));
    for socket in &net.sockets {
        lines.push(match &socket.kind {
            Kind::Udp { port, queue } => format!("socket:   UDP port {} ({} datagram(s) waiting)", port, queue.len()),
            Kind::Listener { port, ready } => {
                format!("socket:   TCP listening on port {} ({} connection(s) waiting)", port, ready.len())
            }
            Kind::Tcp(t) => format!(
                "socket:   TCP port {} <-> {}:{} ({})",
                t.local_port,
                format_ip(t.remote),
                t.remote_port,
                match t.state {
                    TcpState::SynSent | TcpState::SynReceived => "connecting",
                    TcpState::Established => "connected",
                    TcpState::Closed => "closed",
                }
            ),
        });
    }
    lines
}

/// Address, mask, gateway and DNS server, once DHCP has provided them.
pub fn config() -> Option<[Ip; 4]> {
    let guard = NET.lock();
    let net = guard.as_ref()?;
    (net.dhcp == Dhcp::Bound).then_some([net.ip, net.mask, net.gateway, net.dns])
}

// ---------------------------------------------------------------- ping

pub enum PingError {
    NoAdapter,
    NoAddress,
    /// Nobody answered the ARP request for the next hop.
    Unreachable,
}

/// Try to send one ICMP echo request. Ok(false) means "not yet": the next
/// hop's hardware address is still being looked up, so try again shortly.
/// Use `ping_reply` to see whether (and when) the answer came back.
pub fn ping_try(destination: Ip, sequence: u16) -> Result<bool, PingError> {
    let mut guard = NET.lock();
    let net = guard.as_mut().ok_or(PingError::NoAdapter)?;
    if net.dhcp != Dhcp::Bound {
        return Err(PingError::NoAddress);
    }
    let mut echo = vec![8, 0, 0, 0, 0x52, 0x44]; // type 8, id "RD"
    echo.extend_from_slice(&sequence.to_be_bytes());
    echo.extend_from_slice(b"RDX ping payload, 32 bytes long!");
    let sum = checksum(&echo);
    echo[2..4].copy_from_slice(&sum.to_be_bytes());
    Ok(net.deliver(Outgoing { to: destination, protocol: PROTO_ICMP, payload: echo }))
}

/// Send one ICMP echo request, waiting up to a second for the next hop to
/// be found. For kernel threads (it sleeps).
pub fn ping(destination: Ip, sequence: u16) -> Result<(), PingError> {
    let started = arch::ticks();
    loop {
        if ping_try(destination, sequence)? {
            return Ok(());
        }
        if arch::ticks() > started + ms(1000) {
            return Err(PingError::Unreachable);
        }
        sched::sleep_ms(10);
    }
}

/// The tick at which the reply to echo request `sequence` arrived, if it has.
pub fn ping_reply(sequence: u16) -> Option<u64> {
    NET.lock().as_ref()?.echo_replies.iter().find(|(s, _)| *s == sequence).map(|&(_, tick)| tick)
}

// ---------------------------------------------------------------- socket API
//
// Used by the system call layer. Each call takes the stack's lock, does its
// work and returns at once.

/// Run `f` on the stack, then process anything that produced or that has
/// arrived in the meantime, so callers see results without waiting a tick.
fn with_net<R>(f: impl FnOnce(&mut Net) -> R) -> Option<R> {
    let mut guard = NET.lock();
    let net = guard.as_mut()?;
    let result = f(net);
    net.poll();
    Some(result)
}

/// Turn a host name (or dotted address) into an address.
pub fn lookup(name: &str) -> Lookup {
    with_net(|net| net.lookup(name)).unwrap_or(Lookup::Failed)
}

/// Start connecting to `ip:port`. The handshake proceeds in the background;
/// poll `tcp_connected` for the outcome.
pub fn tcp_open(ip: Ip, port: u16) -> Option<SocketHandle> {
    with_net(|net| {
        if net.dhcp != Dhcp::Bound || net.sockets.len() >= 256 {
            return None;
        }
        let (local_port, iss) = (net.ephemeral_port(), net.initial_sequence());
        let id = net.new_id();
        net.sockets.push(Socket { id, kind: Kind::Tcp(Tcp::new(TcpState::SynSent, local_port, ip, port, iss)) });
        Some(SocketHandle(id))
    })?
}

pub fn tcp_connected(id: u32) -> Connect {
    with_net(|net| match net.tcp_mut(id) {
        Some(tcp) if tcp.failed => Connect::Failed,
        Some(tcp) if tcp.state == TcpState::Established => Connect::Connected,
        // Connected and already shut down again still counts as success.
        Some(tcp) if tcp.state == TcpState::Closed => Connect::Connected,
        Some(_) => Connect::Connecting,
        None => Connect::Failed,
    })
    .unwrap_or(Connect::Failed)
}

/// Start accepting TCP connections on a port.
pub fn tcp_listen(port: u16) -> Option<SocketHandle> {
    with_net(|net| {
        let taken = net.sockets.iter().any(|s| matches!(&s.kind, Kind::Listener { port: p, .. } if *p == port));
        if taken || port == 0 || net.sockets.len() >= 256 {
            return None;
        }
        let id = net.new_id();
        net.sockets.push(Socket { id, kind: Kind::Listener { port, ready: VecDeque::new() } });
        Some(SocketHandle(id))
    })?
}

/// The next established incoming connection, if one is waiting.
/// `Err(())` means the descriptor is not a listening socket.
pub fn tcp_accept(listener: u32) -> Result<Option<SocketHandle>, ()> {
    with_net(|net| {
        let socket = net.sockets.iter_mut().find(|s| s.id == listener).ok_or(())?;
        let Kind::Listener { ready, .. } = &mut socket.kind else { return Err(()) };
        let Some(id) = ready.pop_front() else { return Ok(None) };
        if let Some(tcp) = net.tcp_mut(id) {
            tcp.listener = None;
        }
        Ok(Some(SocketHandle(id)))
    })
    .unwrap_or(Err(()))
}

/// Finish sending on a connection (the peer sees end-of-input) while still
/// being able to receive.
pub fn tcp_shutdown(id: u32) -> bool {
    with_net(|net| match net.tcp_mut(id) {
        Some(tcp) => {
            tcp.fin_wanted = true;
            true
        }
        None => false,
    })
    .unwrap_or(false)
}

/// Open a UDP port (0 = pick one).
pub fn udp_open(port: u16) -> Option<SocketHandle> {
    with_net(|net| {
        let port = if port == 0 { net.ephemeral_port() } else { port };
        let taken = net.sockets.iter().any(|s| matches!(&s.kind, Kind::Udp { port: p, .. } if *p == port));
        if taken || port == 68 || port == DNS_PORT || net.sockets.len() >= 256 {
            return None;
        }
        let id = net.new_id();
        net.sockets.push(Socket { id, kind: Kind::Udp { port, queue: VecDeque::new() } });
        Some(SocketHandle(id))
    })?
}

/// Send one datagram. `WouldBlock` means the destination's hardware address
/// is still being looked up: try again in a moment.
pub fn udp_send(id: u32, to: Ip, to_port: u16, data: &[u8]) -> Io {
    if data.len() > 1472 {
        return Io::Closed; // would not fit in one packet
    }
    with_net(|net| {
        let Some(port) = net.sockets.iter().find(|s| s.id == id).and_then(|s| match &s.kind {
            Kind::Udp { port, .. } => Some(*port),
            _ => None,
        }) else {
            return Io::Closed;
        };
        if net.dhcp != Dhcp::Bound {
            return Io::Closed;
        }
        if net.send_udp(to, port, to_port, data) { Io::Done(data.len()) } else { Io::WouldBlock }
    })
    .unwrap_or(Io::Closed)
}

/// The next datagram that arrived on a UDP socket: (from, port, data).
pub fn udp_receive(id: u32) -> Result<Option<(Ip, u16, Vec<u8>)>, ()> {
    with_net(|net| match net.sockets.iter_mut().find(|s| s.id == id).map(|s| &mut s.kind) {
        Some(Kind::Udp { queue, .. }) => Ok(queue.pop_front()),
        _ => Err(()),
    })
    .unwrap_or(Err(()))
}

/// (readable, writable, finished) for a socket, for poll.
pub fn readiness(id: u32) -> (bool, bool, bool) {
    let guard = NET.lock();
    let Some(net) = guard.as_ref() else { return (true, false, true) };
    match net.sockets.iter().find(|s| s.id == id).map(|s| &s.kind) {
        Some(Kind::Tcp(tcp)) => {
            let over = tcp.failed || tcp.state == TcpState::Closed;
            let finished = over || tcp.peer_fin;
            let can_send = tcp.state == TcpState::Established && !tcp.fin_wanted && tcp.send_buf.len() < SEND_CAPACITY;
            (!tcp.recv_buf.is_empty() || finished, can_send || over, finished)
        }
        Some(Kind::Listener { ready, .. }) => (!ready.is_empty(), false, false),
        Some(Kind::Udp { queue, .. }) => (!queue.is_empty(), true, false),
        None => (true, false, true),
    }
}

/// Read from a socket: bytes of a TCP stream, or one whole UDP datagram.
pub fn read(id: u32, buf: &mut [u8]) -> Io {
    with_net(|net| match net.sockets.iter_mut().find(|s| s.id == id).map(|s| &mut s.kind) {
        Some(Kind::Tcp(tcp)) => {
            if tcp.recv_buf.is_empty() {
                let finished = tcp.peer_fin || tcp.failed || tcp.state == TcpState::Closed;
                return if finished { Io::Closed } else { Io::WouldBlock };
            }
            let n = buf.len().min(tcp.recv_buf.len());
            for (slot, byte) in buf.iter_mut().zip(tcp.recv_buf.drain(..n)) {
                *slot = byte;
            }
            Io::Done(n)
        }
        Some(Kind::Udp { queue, .. }) => match queue.pop_front() {
            Some((_, _, data)) => {
                let n = buf.len().min(data.len());
                buf[..n].copy_from_slice(&data[..n]);
                Io::Done(n)
            }
            None => Io::WouldBlock,
        },
        _ => Io::Closed,
    })
    .unwrap_or(Io::Closed)
}

/// Queue bytes for sending on a TCP connection. May take only part of them.
pub fn write(id: u32, data: &[u8]) -> Io {
    with_net(|net| match net.tcp_mut(id) {
        Some(tcp) => {
            if tcp.failed || tcp.fin_wanted || tcp.state == TcpState::Closed {
                return Io::Closed;
            }
            if tcp.state != TcpState::Established {
                return Io::WouldBlock;
            }
            let n = data.len().min(SEND_CAPACITY - tcp.send_buf.len());
            if n == 0 && !data.is_empty() {
                return Io::WouldBlock;
            }
            tcp.send_buf.extend(&data[..n]);
            Io::Done(n)
        }
        None => Io::Closed,
    })
    .unwrap_or(Io::Closed)
}

/// The descriptor is gone. TCP connections finish sending and shut down in
/// the background; everything else is removed at once.
fn close(id: u32) {
    with_net(|net| {
        let now = arch::ticks();
        let Some(index) = net.sockets.iter().position(|s| s.id == id) else { return };
        match &mut net.sockets[index].kind {
            Kind::Tcp(tcp) => {
                tcp.orphaned_at = Some(now);
                if tcp.state == TcpState::Established {
                    tcp.fin_wanted = true;
                } else {
                    tcp.state = TcpState::Closed;
                }
            }
            Kind::Listener { ready, .. } => {
                // Connections nobody accepted go with it.
                let abandoned: Vec<u32> = ready.drain(..).collect();
                net.sockets.remove(index);
                for socket in net.sockets.iter_mut() {
                    if let Kind::Tcp(tcp) = &mut socket.kind {
                        if abandoned.contains(&socket.id) || tcp.listener == Some(id) {
                            tcp.orphaned_at = Some(now);
                            tcp.fin_wanted = true;
                        }
                    }
                }
            }
            Kind::Udp { .. } => {
                net.sockets.remove(index);
            }
        }
    });
}
