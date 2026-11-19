//! Local connections: how a program finds a server on the same machine and
//! talks to it, like a Unix-domain socket.
//!
//! A server calls local_listen("desktop"); clients call
//! local_connect("desktop"), and the server picks each one up with
//! local_accept. Each connection is a two-way stream of bytes: a pair of
//! pipes, one for each direction, behind one descriptor at each end. So
//! everything that works on a pipe - read, write, poll, being told when the
//! other end has gone - works on a connection.
//!
//! A name belongs to whoever listens on it until that descriptor is closed
//! (or its program exits).

use alloc::collections::{BTreeMap, VecDeque};
use alloc::string::String;
use alloc::sync::Arc;

use crate::pipe::{Fd, Pipe};
use crate::sched::{self, Wait};
use crate::sync::IrqLock;

/// Connections a server has not accepted yet, beyond which more are refused.
const BACKLOG: usize = 32;

pub struct Listener {
    name: String,
    pending: IrqLock<VecDeque<Fd>>,
}

impl Listener {
    pub fn channel(&self) -> Wait {
        Wait::Pipe(self as *const Listener as usize)
    }

    /// The next waiting connection, if any.
    pub fn accept(&self) -> Option<Fd> {
        self.pending.lock().pop_front()
    }

    pub fn has_pending(&self) -> bool {
        !self.pending.lock().is_empty()
    }
}

impl Drop for Listener {
    fn drop(&mut self) {
        // Give up the name, unless someone else has taken it since.
        let mut names = NAMES.lock();
        if names.get(&self.name).map_or(false, |l| l.as_ptr() == self as *const Listener) {
            names.remove(&self.name);
        }
    }
}

/// Weak references, so that a name lives exactly as long as the server's
/// descriptor for it.
static NAMES: IrqLock<BTreeMap<String, alloc::sync::Weak<Listener>>> = IrqLock::new(BTreeMap::new());

/// Start listening on `name`. Fails if a live server already has it.
pub fn listen(name: &str) -> Result<Arc<Listener>, &'static str> {
    if name.is_empty() || name.len() > 63 {
        return Err("a connection name must be 1 to 63 characters");
    }
    let mut names = NAMES.lock();
    if names.get(name).map_or(false, |weak| weak.strong_count() > 0) {
        return Err("another program is already listening on that name");
    }
    let listener = Arc::new(Listener { name: String::from(name), pending: IrqLock::new(VecDeque::new()) });
    names.insert(String::from(name), Arc::downgrade(&listener));
    Ok(listener)
}

/// Connect to the server listening on `name`. Returns the client's end.
pub fn connect(name: &str) -> Result<Fd, &'static str> {
    let listener = NAMES.lock().get(name).and_then(|weak| weak.upgrade()).ok_or("nothing is listening on that name")?;
    // One pipe each way: the server reads what the client writes, and the
    // other way round.
    let upstream = Pipe::bare();
    let downstream = Pipe::bare();
    let server_end = Fd::stream(upstream.clone(), downstream.clone());
    let client_end = Fd::stream(downstream, upstream);
    {
        let mut pending = listener.pending.lock();
        if pending.len() >= BACKLOG {
            return Err("the server is too busy to take another connection");
        }
        pending.push_back(server_end);
    }
    sched::wake(listener.channel());
    Ok(client_end)
}
