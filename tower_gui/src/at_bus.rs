//! Background AT tty — same dialect as `AtSession` / `tools/at_probe.py`.
//! Never blocks the egui thread (COPS=? can sit for minutes).

use std::io;
use std::os::unix::io::RawFd;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, Sender, TryRecvError};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

#[derive(Clone, Debug)]
pub struct AtStep {
    pub cmd: String,
    pub timeout_ms: u32,
}

#[derive(Debug, Clone)]
pub enum AtEvent {
    Opened(String),
    Closed,
    Busy(bool),
    Tx { cmd: String },
    Rx {
        cmd: String,
        text: String,
        ok: bool,
        ms: u32,
    },
    Progress { cmd: String, elapsed_ms: u32, hint: String },
    Err(String),
    Note(String),
}

enum Req {
    Open(String),
    Close,
    Run { job: String, steps: Vec<AtStep> },
    Cancel,
    Quit,
}

pub struct AtBus {
    tx: Sender<Req>,
    rx: Receiver<AtEvent>,
    cancel: Arc<AtomicBool>,
    _thread: Option<JoinHandle<()>>,
    pub connected: bool,
    pub port: String,
    pub busy: bool,
}

impl AtBus {
    pub fn new() -> Self {
        let (req_tx, req_rx) = mpsc::channel();
        let (ev_tx, ev_rx) = mpsc::channel();
        let cancel = Arc::new(AtomicBool::new(false));
        let cancel_w = cancel.clone();
        let thread = thread::Builder::new()
            .name("observer-at".into())
            .spawn(move || worker(req_rx, ev_tx, cancel_w))
            .expect("spawn AT worker");
        Self {
            tx: req_tx,
            rx: ev_rx,
            cancel,
            _thread: Some(thread),
            connected: false,
            port: default_at_port(),
            busy: false,
        }
    }

    pub fn poll(&mut self) -> Vec<AtEvent> {
        let mut out = Vec::new();
        loop {
            match self.rx.try_recv() {
                Ok(ev) => {
                    match &ev {
                        AtEvent::Opened(p) => {
                            self.connected = true;
                            self.port = p.clone();
                        }
                        AtEvent::Closed => self.connected = false,
                        AtEvent::Busy(b) => self.busy = *b,
                        _ => {}
                    }
                    out.push(ev);
                }
                Err(TryRecvError::Empty) => break,
                Err(TryRecvError::Disconnected) => {
                    self.connected = false;
                    self.busy = false;
                    break;
                }
            }
        }
        out
    }

    pub fn open(&self, port: &str) {
        let _ = self.tx.send(Req::Open(port.to_string()));
    }

    pub fn close(&self) {
        let _ = self.tx.send(Req::Close);
    }

    pub fn cancel(&self) {
        self.cancel.store(true, Ordering::Relaxed);
        let _ = self.tx.send(Req::Cancel);
    }

    pub fn run(&self, job: impl Into<String>, steps: Vec<AtStep>) {
        if steps.is_empty() {
            return;
        }
        self.cancel.store(false, Ordering::Relaxed);
        let _ = self.tx.send(Req::Run {
            job: job.into(),
            steps,
        });
    }
}

impl Drop for AtBus {
    fn drop(&mut self) {
        self.cancel.store(true, Ordering::Relaxed);
        let _ = self.tx.send(Req::Quit);
    }
}

pub fn default_at_port() -> String {
    let ports = list_at_ports();
    if ports.iter().any(|p| p == "/dev/ttyUSB2") {
        "/dev/ttyUSB2".into()
    } else {
        ports.into_iter().next().unwrap_or_else(|| "/dev/ttyUSB2".into())
    }
}

pub fn list_at_ports() -> Vec<String> {
    let mut out = Vec::new();
    if let Ok(rd) = std::fs::read_dir("/dev") {
        for e in rd.flatten() {
            let name = e.file_name();
            let s = name.to_string_lossy();
            if s.starts_with("ttyUSB") || s.starts_with("ttyACM") {
                out.push(format!("/dev/{s}"));
            }
        }
    }
    out.sort();
    if out.is_empty() {
        out.push("/dev/ttyUSB2".into());
    }
    out
}

fn worker(rx: Receiver<Req>, tx: Sender<AtEvent>, cancel: Arc<AtomicBool>) {
    let mut sess: Option<Tty> = None;
    loop {
        let req = match rx.recv_timeout(Duration::from_millis(80)) {
            Ok(r) => r,
            Err(mpsc::RecvTimeoutError::Timeout) => continue,
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
        };
        match req {
            Req::Quit => {
                sess.take();
                let _ = tx.send(AtEvent::Closed);
                break;
            }
            Req::Open(path) => {
                match Tty::open(&path) {
                    Ok(t) => {
                        sess = Some(t);
                        let _ = tx.send(AtEvent::Opened(path.clone()));
                        let _ = tx.send(AtEvent::Note(format!("ATE0 on {path}")));
                    }
                    Err(e) => {
                        sess = None;
                        let _ = tx.send(AtEvent::Err(format!("{path}: {e}")));
                        let _ = tx.send(AtEvent::Closed);
                    }
                }
            }
            Req::Close => {
                sess.take();
                let _ = tx.send(AtEvent::Closed);
            }
            Req::Cancel => {
                cancel.store(true, Ordering::Relaxed);
            }
            Req::Run { job, steps } => {
                cancel.store(false, Ordering::Relaxed);
                let _ = tx.send(AtEvent::Busy(true));
                if sess.is_none() {
                    let _ = tx.send(AtEvent::Err("AT port is not connected".into()));
                    let _ = tx.send(AtEvent::Busy(false));
                    continue;
                }
                for step in steps {
                    if cancel.load(Ordering::Relaxed) {
                        let _ = tx.send(AtEvent::Note("cancelled".into()));
                        break;
                    }
                    let Some(tty) = sess.as_mut() else { break };
                    let _ = tx.send(AtEvent::Tx {
                        cmd: step.cmd.clone(),
                    });
                    match tty.transact(&step.cmd, step.timeout_ms, &cancel, |elapsed, partial| {
                        let hint = last_nonempty_line(partial);
                        let _ = tx.send(AtEvent::Progress {
                            cmd: step.cmd.clone(),
                            elapsed_ms: elapsed,
                            hint,
                        });
                    }) {
                        Ok((text, ms)) => {
                            let ok = reply_ok(&text);
                            let _ = tx.send(AtEvent::Rx {
                                cmd: step.cmd.clone(),
                                text,
                                ok,
                                ms,
                            });
                            if !ok && job == "camp" {
                                // Keep going on camp? No — dual-lock order matters; stop on first fail.
                                break;
                            }
                        }
                        Err(e) => {
                            let _ = tx.send(AtEvent::Err(format!("{}: {e}", step.cmd)));
                            // Tty may be dead.
                            if e.kind() == io::ErrorKind::BrokenPipe
                                || e.raw_os_error() == Some(libc::EIO)
                            {
                                sess.take();
                                let _ = tx.send(AtEvent::Closed);
                            }
                            break;
                        }
                    }
                }
                let _ = tx.send(AtEvent::Busy(false));
            }
        }
    }
}

struct Tty {
    fd: RawFd,
}

impl Tty {
    fn open(path: &str) -> io::Result<Self> {
        let cpath = std::ffi::CString::new(path)
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "port path"))?;
        let fd = unsafe {
            libc::open(cpath.as_ptr(), libc::O_RDWR | libc::O_NOCTTY | libc::O_NONBLOCK)
        };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        unsafe {
            let mut tio: libc::termios = std::mem::zeroed();
            if libc::tcgetattr(fd, &mut tio) == 0 {
                libc::cfmakeraw(&mut tio);
                libc::cfsetispeed(&mut tio, libc::B115200);
                libc::cfsetospeed(&mut tio, libc::B115200);
                tio.c_cflag |= libc::CLOCAL | libc::CREAD;
                tio.c_cc[libc::VMIN] = 0;
                tio.c_cc[libc::VTIME] = 0;
                libc::tcsetattr(fd, libc::TCSANOW, &tio);
            }
        }
        let mut t = Self { fd };
        t.drain();
        // ATE0 so parsers see clean OK / +CPSI:
        let _ = t.transact("ATE0", 400, &AtomicBool::new(false), |_, _| {});
        t.drain();
        Ok(t)
    }

    fn drain(&mut self) {
        let mut buf = [0u8; 512];
        for _ in 0..64 {
            let n = unsafe { libc::read(self.fd, buf.as_mut_ptr() as *mut _, buf.len()) };
            if n <= 0 {
                break;
            }
        }
    }

    fn transact(
        &mut self,
        cmd: &str,
        timeout_ms: u32,
        cancel: &AtomicBool,
        mut tick: impl FnMut(u32, &str),
    ) -> io::Result<(String, u32)> {
        self.drain();
        let wire = at_wire(cmd);
        let n = unsafe { libc::write(self.fd, wire.as_ptr() as *const _, wire.len()) };
        if n != wire.len() as isize {
            return Err(io::Error::last_os_error());
        }
        unsafe { libc::tcdrain(self.fd) };

        let start = Instant::now();
        let deadline = start + Duration::from_millis(timeout_ms.max(200) as u64);
        let mut out = String::new();
        let mut last_tick = -1i32;
        while Instant::now() < deadline {
            if cancel.load(Ordering::Relaxed) {
                break;
            }
            let mut pfd = libc::pollfd {
                fd: self.fd,
                events: libc::POLLIN,
                revents: 0,
            };
            let pr = unsafe { libc::poll(&mut pfd, 1, 100) };
            if pr < 0 {
                let e = io::Error::last_os_error();
                if e.kind() == io::ErrorKind::Interrupted {
                    continue;
                }
                return Err(e);
            }
            if pr > 0 && (pfd.revents & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL)) != 0 {
                return Err(io::Error::new(io::ErrorKind::BrokenPipe, "tty hung up"));
            }
            if pr > 0 && (pfd.revents & libc::POLLIN) != 0 {
                let mut buf = [0u8; 1024];
                loop {
                    let n = unsafe { libc::read(self.fd, buf.as_mut_ptr() as *mut _, buf.len()) };
                    if n > 0 {
                        out.push_str(&String::from_utf8_lossy(&buf[..n as usize]));
                        continue;
                    }
                    break;
                }
                if at_has_final(&out) {
                    let ms = start.elapsed().as_millis() as u32;
                    return Ok((out, ms));
                }
            }
            let elapsed = start.elapsed().as_millis() as u32;
            let bucket = (elapsed / 1000) as i32;
            if bucket != last_tick {
                last_tick = bucket;
                tick(elapsed, &out);
            }
        }
        let ms = start.elapsed().as_millis() as u32;
        if out.is_empty() {
            Err(io::Error::new(
                io::ErrorKind::TimedOut,
                format!("timeout {timeout_ms} ms"),
            ))
        } else {
            Ok((out, ms))
        }
    }
}

impl Drop for Tty {
    fn drop(&mut self) {
        if self.fd >= 0 {
            unsafe { libc::close(self.fd) };
            self.fd = -1;
        }
    }
}

fn at_wire(cmd: &str) -> Vec<u8> {
    let mut s = cmd.trim().to_string();
    if !s.ends_with('\n') {
        s.push_str("\r\n");
    } else if !s.ends_with("\r\n") {
        s.insert(s.len() - 1, '\r');
    }
    s.into_bytes()
}

fn at_has_final(out: &str) -> bool {
    for line in out.split(['\r', '\n']) {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        if line == "OK"
            || line == "ERROR"
            || line.starts_with("+CME ERROR")
            || line.starts_with("+CMS ERROR")
            || line.starts_with("NOT IN")
        {
            return true;
        }
    }
    false
}

pub fn reply_ok(resp: &str) -> bool {
    if resp.contains("ERROR") || resp.contains("NOT IN") {
        return false;
    }
    resp.split(['\r', '\n']).any(|l| l.trim() == "OK") || resp.trim().ends_with("OK")
}

fn last_nonempty_line(s: &str) -> String {
    s.split(['\r', '\n'])
        .rev()
        .find(|l| !l.trim().is_empty())
        .unwrap_or("")
        .trim()
        .chars()
        .take(80)
        .collect()
}
