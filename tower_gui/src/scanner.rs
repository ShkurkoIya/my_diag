//! Spawn / stop `live_scanner` from the Live Scan UI (LTE / WCDMA / IRAT).
//!
//! Important: only one writer may own `/tmp/qcom_live_towers.json`. Start always
//! kills any prior `live_scanner` targeting the same live-json path (including
//! root/pkexec orphans from a previous GUI session).
//!
//! Start/stop never block the egui thread on `pkexec` (that froze/crashed the UI).

use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStderr, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

const PID_PATH: &str = "/tmp/qcom_live_scanner.pid";
const CMD_PATH: &str = "/tmp/qcom_live_scanner.cmd";
const DEFAULT_LIVE_JSON: &str = "/tmp/qcom_live_towers.json";
const DEFAULT_SCANNER_LOG: &str = "/tmp/qcom_live_scanner.log";

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum SurveyMode {
    #[default]
    Irat,
    Lte,
    Wcdma,
}

impl SurveyMode {
    pub fn label(self) -> &'static str {
        match self {
            Self::Lte => "LTE 4G",
            Self::Wcdma => "WCDMA 3G",
            Self::Irat => "IRAT 4G→3G",
        }
    }

    pub fn cli(self) -> &'static str {
        match self {
            Self::Lte => "lte",
            Self::Wcdma => "wcdma",
            Self::Irat => "irat",
        }
    }

    pub fn hint(self) -> &'static str {
        match self {
            Self::Lte => "CCELLCFG hop on LTE only (no 3G walk)",
            Self::Wcdma => "CNMP=14 WCDMA camp walk (SIB6/0x4005/CPSI)",
            Self::Irat => "LTE full-walk, then WCDMA IRAT, back to LTE",
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum ScannerPhase {
    #[default]
    Idle,
    Starting,
    Running,
    Stopping,
}

struct SharedState {
    status: Mutex<String>,
    last_err: Mutex<String>,
    phase: Mutex<ScannerPhase>,
    child: Mutex<Option<Child>>,
    stderr_lines: Mutex<Vec<String>>,
    busy: AtomicBool,
}

pub struct ScannerControl {
    pub mode: SurveyMode,
    pub binary: PathBuf,
    pub live_json: PathBuf,
    pub scanner_log: PathBuf,
    pub use_pkexec: bool,
    /// Mirrored for UI; refreshed in [`Self::tick`].
    pub status: String,
    pub last_err: String,
    pub phase: ScannerPhase,
    shared: Arc<SharedState>,
}

impl Default for ScannerControl {
    fn default() -> Self {
        Self::new()
    }
}

impl ScannerControl {
    pub fn new() -> Self {
        let shared = Arc::new(SharedState {
            status: Mutex::new("idle".into()),
            last_err: Mutex::new(String::new()),
            phase: Mutex::new(ScannerPhase::Idle),
            child: Mutex::new(None),
            stderr_lines: Mutex::new(Vec::new()),
            busy: AtomicBool::new(false),
        });
        let (live_json, scanner_log) = preferred_live_paths();
        Self {
            mode: SurveyMode::Irat,
            binary: resolve_live_scanner(),
            live_json,
            scanner_log,
            // dialout is enough for DIAG/AT. QMI (/dev/cdc-wdm*) is attempted as-is;
            // pkexec helps when the node is root:root 0600.
            use_pkexec: false,
            status: "idle".into(),
            last_err: String::new(),
            phase: ScannerPhase::Idle,
            shared,
        }
    }

    /// Make sure live-json / lock / log are writable (root leftovers from old pkexec runs).
    /// May redirect `live_json` / `scanner_log` to per-user paths.
    pub fn ensure_writable_paths(&mut self) -> Option<String> {
        if live_bundle_usable(&self.live_json, &self.scanner_log) {
            return None;
        }
        let (json, log) = user_live_paths();
        let note = format!(
            "default /tmp paths owned by root — using {}",
            json.display()
        );
        self.live_json = json;
        self.scanner_log = log;
        Some(note)
    }

    fn sync_ui(&mut self) {
        if let Ok(s) = self.shared.status.lock() {
            self.status = s.clone();
        }
        if let Ok(e) = self.shared.last_err.lock() {
            self.last_err = e.clone();
        }
        if let Ok(p) = self.shared.phase.lock() {
            self.phase = *p;
        }
    }

    fn set_shared(shared: &SharedState, phase: ScannerPhase, status: impl Into<String>, err: impl Into<String>) {
        if let Ok(mut p) = shared.phase.lock() {
            *p = phase;
        }
        if let Ok(mut s) = shared.status.lock() {
            *s = status.into();
        }
        if let Ok(mut e) = shared.last_err.lock() {
            *e = err.into();
        }
    }

    /// True when the scanner process is up, or start/stop is in flight.
    pub fn is_running(&mut self) -> bool {
        self.tick();
        matches!(
            self.phase,
            ScannerPhase::Starting | ScannerPhase::Running | ScannerPhase::Stopping
        )
    }

    pub fn can_start(&mut self) -> bool {
        self.tick();
        self.phase == ScannerPhase::Idle && !self.shared.busy.load(Ordering::Relaxed)
    }

    pub fn can_stop(&mut self) -> bool {
        self.tick();
        matches!(self.phase, ScannerPhase::Running | ScannerPhase::Starting)
    }

    /// Last stderr lines from live_scanner (boot / QMI open / tty errors).
    pub fn boot_notes(&self) -> Vec<String> {
        self.shared
            .stderr_lines
            .lock()
            .map(|g| g.clone())
            .unwrap_or_default()
    }

    pub fn start(&mut self) {
        let path_note = self.ensure_writable_paths();
        // Always re-resolve: cargo-run cwd vs IDE cwd vs first construction.
        self.binary = resolve_live_scanner();
        if !self.binary.is_file() {
            Self::set_shared(
                &self.shared,
                ScannerPhase::Idle,
                "missing binary",
                format!("live_scanner not found: {}", self.binary.display()),
            );
            self.sync_ui();
            return;
        }
        if self
            .shared
            .busy
            .compare_exchange(false, true, Ordering::SeqCst, Ordering::Relaxed)
            .is_err()
        {
            return;
        }

        let start_msg = path_note.clone().unwrap_or_else(|| "starting…".into());
        Self::set_shared(
            &self.shared,
            ScannerPhase::Starting,
            if path_note.is_some() {
                start_msg
            } else {
                "starting…".into()
            },
            "",
        );
        self.sync_ui();

        let shared = Arc::clone(&self.shared);
        let binary = self.binary.clone();
        let live_json = self.live_json.clone();
        let scanner_log = self.scanner_log.clone();
        let use_pkexec = self.use_pkexec;
        let mode = self.mode;
        let args = self.build_args();
        // QMI is on by default. Scanner degrades to DIAG+AT if /dev/cdc-wdm*
        // is not writable (chmod a+rw / pkexec). Do not pass --no-qmi.

        thread::spawn(move || {
            // Never pkexec here — password dialogs from a bg thread were crashing the GUI.
            soft_stop_user_scanners(&shared, &live_json);
            thread::sleep(Duration::from_millis(250));

            // Only remove files we can actually touch (skip root leftovers).
            remove_if_owned(&live_json);
            remove_if_owned(Path::new(&format!("{}.tmp", live_json.display())));
            remove_if_owned(Path::new(&format!("{}.lock", live_json.display())));
            remove_if_owned(Path::new(PID_PATH));
            remove_if_owned(&scanner_log);

            if let Ok(mut f) = std::fs::File::create(CMD_PATH) {
                let line = format!(
                    "{}{} {}",
                    if use_pkexec { "pkexec " } else { "" },
                    binary.display(),
                    args.iter()
                        .map(|s| shell_quote(s))
                        .collect::<Vec<_>>()
                        .join(" ")
                );
                let _ = writeln!(f, "{line}");
            }

            if let Ok(mut lines) = shared.stderr_lines.lock() {
                lines.clear();
            }

            Self::set_shared(
                &shared,
                ScannerPhase::Starting,
                if use_pkexec {
                    "starting… (polkit password)"
                } else {
                    "starting…"
                },
                "",
            );

            let mut qmi_note = String::new();
            if !use_pkexec {
                let blocked: Vec<PathBuf> = cdc_wdm_paths()
                    .into_iter()
                    .filter(|p| !device_rw(p))
                    .collect();
                if !blocked.is_empty() {
                    Self::set_shared(
                        &shared,
                        ScannerPhase::Starting,
                        "starting… (QMI access / polkit)",
                        "",
                    );
                    if !pkexec_chmod_rw(&blocked) {
                        qmi_note = format!(
                            "QMI {} still 0600 — neighbours from DIAG; sudo chmod a+rw {}",
                            blocked[0].display(),
                            blocked[0].display()
                        );
                        Self::set_shared(
                            &shared,
                            ScannerPhase::Starting,
                            "starting… (DIAG+AT, QMI locked)",
                            qmi_note.clone(),
                        );
                    }
                }
            }

            let spawn_result = spawn_scanner(&binary, &args, use_pkexec, /*skip_qmi_pkexec=*/ !use_pkexec);

            match spawn_result {
                Ok(mut child) => {
                    let wrapper_pid = child.id() as i32;
                    if let Some(pipe) = child.stderr.take() {
                        drain_stderr(pipe, Arc::clone(&shared));
                    }
                    if let Ok(mut guard) = shared.child.lock() {
                        *guard = Some(child);
                    }

                    // pkexec waits for password — poll up to 90s, do NOT drop Child early
                    // (that used to dismiss the auth dialog / look like a crash).
                    // Direct spawn: wait through boot() so "No modem" is not flashed as Running.
                    let wait_for = if use_pkexec {
                        Duration::from_secs(90)
                    } else {
                        Duration::from_secs(6)
                    };
                    let deadline = Instant::now() + wait_for;
                    let boot_ok_at = Instant::now() + Duration::from_millis(if use_pkexec {
                        0
                    } else {
                        2200
                    });
                    let mut real_pid: Option<i32> = None;
                    while Instant::now() < deadline {
                        let child_dead: Option<bool> = {
                            let mut dead = None;
                            if let Ok(mut guard) = shared.child.lock() {
                                if let Some(c) = guard.as_mut() {
                                    match c.try_wait() {
                                        Ok(Some(s)) => {
                                            *guard = None;
                                            dead = Some(s.success());
                                        }
                                        Ok(None) => {}
                                        Err(_) => {
                                            *guard = None;
                                            dead = Some(false);
                                        }
                                    }
                                }
                            }
                            dead
                        };
                        if let Some(success) = child_dead {
                            // Drain thread may still be finishing the last line.
                            thread::sleep(Duration::from_millis(80));
                            let hint = stderr_hint(&shared);
                            let hint = if !hint.is_empty() {
                                hint
                            } else if use_pkexec {
                                "pkexec cancelled or denied".into()
                            } else if !success {
                                "live_scanner exited (failed)".into()
                            } else {
                                "live_scanner exited immediately".into()
                            };
                            Self::set_shared(&shared, ScannerPhase::Idle, "spawn failed", hint);
                            shared.busy.store(false, Ordering::SeqCst);
                            return;
                        }

                        if let Some(pid) = find_live_scanner_pids(&live_json).into_iter().next() {
                            if use_pkexec || Instant::now() >= boot_ok_at {
                                real_pid = Some(pid);
                                break;
                            }
                        }
                        if !use_pkexec
                            && process_alive(wrapper_pid)
                            && Instant::now() >= boot_ok_at
                        {
                            real_pid = Some(wrapper_pid);
                            break;
                        }
                        thread::sleep(Duration::from_millis(100));
                    }

                    if let Some(real) = real_pid {
                        if let Ok(mut f) = std::fs::File::create(PID_PATH) {
                            let _ = writeln!(f, "{real}");
                        }
                        Self::set_shared(
                            &shared,
                            ScannerPhase::Running,
                            format!("running ({})", mode.label()),
                            qmi_note.clone(),
                        );
                    } else {
                        if let Ok(mut guard) = shared.child.lock() {
                            if let Some(mut c) = guard.take() {
                                let _ = c.kill();
                                let _ = c.try_wait();
                            }
                        }
                        thread::sleep(Duration::from_millis(80));
                        let hint = stderr_hint(&shared);
                        Self::set_shared(
                            &shared,
                            ScannerPhase::Idle,
                            "spawn failed",
                            if use_pkexec {
                                "pkexec timeout — password not entered?".into()
                            } else if hint.is_empty() {
                                "live_scanner did not stay up".into()
                            } else {
                                hint
                            },
                        );
                    }
                }
                Err(e) => {
                    Self::set_shared(
                        &shared,
                        ScannerPhase::Idle,
                        "spawn failed",
                        format!("spawn failed: {e}"),
                    );
                }
            }
            shared.busy.store(false, Ordering::SeqCst);
        });
    }

    pub fn stop(&mut self) {
        if self
            .shared
            .busy
            .compare_exchange(false, true, Ordering::SeqCst, Ordering::Relaxed)
            .is_err()
        {
            // Already starting/stopping — nudge another TERM from UI is fine as fire-and-forget.
            let json = self.live_json.clone();
            thread::spawn(move || {
                for pid in find_live_scanner_pids(&json) {
                    let _ = signal_term(pid);
                }
            });
            return;
        }

        Self::set_shared(&self.shared, ScannerPhase::Stopping, "stopping…", "");
        self.sync_ui();

        let shared = Arc::clone(&self.shared);
        let live_json = self.live_json.clone();
        thread::spawn(move || {
            stop_all_impl(&shared, &live_json);
            let left = find_live_scanner_pids(&live_json);
            if left.is_empty() {
                Self::set_shared(&shared, ScannerPhase::Idle, "stopped", "");
            } else {
                Self::set_shared(
                    &shared,
                    ScannerPhase::Idle,
                    "stop incomplete",
                    format!(
                        "still running pid {} — approve pkexec or kill manually",
                        left[0]
                    ),
                );
            }
            shared.busy.store(false, Ordering::SeqCst);
        });
    }

    pub fn tick(&mut self) {
        self.sync_ui();

        if matches!(self.phase, ScannerPhase::Idle) {
            let resolved = resolve_live_scanner();
            if resolved.is_file() || !self.binary.is_file() {
                self.binary = resolved;
            }
        }

        // Refresh Running/Idle from process table when not busy.
        if self.shared.busy.load(Ordering::Relaxed) {
            return;
        }
        let alive = any_live_scanner_for_json(&self.live_json);
        if alive {
            if self.phase != ScannerPhase::Running {
                Self::set_shared(
                    &self.shared,
                    ScannerPhase::Running,
                    format!("running ({})", self.mode.label()),
                    "",
                );
                self.sync_ui();
            }
            if let Ok(mut guard) = self.shared.child.lock() {
                if let Some(child) = guard.as_mut() {
                    let _ = child.try_wait();
                }
            }
        } else if matches!(self.phase, ScannerPhase::Running) {
            // Reap wrapper if needed.
            if let Ok(mut guard) = self.shared.child.lock() {
                if let Some(mut child) = guard.take() {
                    let _ = child.try_wait();
                }
            }
            let hint = stderr_hint(&self.shared);
            Self::set_shared(
                &self.shared,
                ScannerPhase::Idle,
                "exited",
                if hint.is_empty() {
                    "scanner exited".into()
                } else {
                    hint
                },
            );
            self.sync_ui();
        }
    }

    fn build_args(&self) -> Vec<String> {
        let mut a = vec![
            "--survey-mode".into(),
            self.mode.cli().into(),
            "--search-period".into(),
            "90".into(),
            "--hop-dwell".into(),
            "40".into(),
            "--hop-band-clip".into(),
            "--live-json".into(),
            self.live_json.to_string_lossy().into_owned(),
            "--scanner-log".into(),
            self.scanner_log.to_string_lossy().into_owned(),
        ];
        if matches!(self.mode, SurveyMode::Wcdma) {
            a.push("--wcdma-dwell".into());
            a.push("40".into());
        }
        a
    }
}

fn cdc_wdm_paths() -> Vec<PathBuf> {
    let mut out = Vec::new();
    let Ok(rd) = std::fs::read_dir("/dev") else {
        return out;
    };
    for e in rd.flatten() {
        let name = e.file_name();
        if name.to_string_lossy().starts_with("cdc-wdm") {
            out.push(e.path());
        }
    }
    out.sort();
    out
}

fn device_rw(path: &Path) -> bool {
    std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open(path)
        .is_ok()
}

/// One polkit prompt: `chmod a+rw` on qmi_wwan nodes (root:root 0600 after plug).
fn pkexec_chmod_rw(paths: &[PathBuf]) -> bool {
    if paths.is_empty() {
        return true;
    }
    let mut cmd = Command::new("pkexec");
    cmd.arg("chmod").arg("a+rw");
    for p in paths {
        cmd.arg(p);
    }
    cmd.stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    matches!(cmd.status(), Ok(s) if s.success()) && paths.iter().all(|p| device_rw(p))
}

fn spawn_scanner(
    binary: &Path,
    args: &[String],
    use_pkexec: bool,
    skip_qmi_pkexec: bool,
) -> std::io::Result<Child> {
    let mut cmd = if use_pkexec {
        let mut c = Command::new("pkexec");
        c.arg(binary.as_os_str());
        c
    } else {
        Command::new(binary)
    };
    for a in args {
        cmd.arg(a);
    }
    if skip_qmi_pkexec {
        cmd.env("OBSERVER_SKIP_QMI_PKEXEC", "1");
    }
    // stdout is the TTY dashboard (nulled under GUI). stderr is drained so boot
    // errors reach the status line and the pipe cannot fill and freeze the scanner.
    cmd.stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
}

fn drain_stderr(stderr: ChildStderr, shared: Arc<SharedState>) {
    thread::spawn(move || {
        let reader = BufReader::new(stderr);
        for line in reader.lines() {
            let Ok(line) = line else {
                break;
            };
            let t = line.trim();
            if t.is_empty() {
                continue;
            }
            if let Ok(mut lines) = shared.stderr_lines.lock() {
                lines.push(t.to_string());
                if lines.len() > 16 {
                    let extra = lines.len() - 16;
                    lines.drain(..extra);
                }
            }
        }
    });
}

fn stderr_hint(shared: &SharedState) -> String {
    let lines = shared
        .stderr_lines
        .lock()
        .map(|g| g.clone())
        .unwrap_or_default();
    pick_stderr_hint(&lines)
}

fn pick_stderr_hint(lines: &[String]) -> String {
    let trimmed: Vec<&str> = lines
        .iter()
        .map(|s| s.trim())
        .filter(|s| !s.is_empty())
        .collect();
    let chosen = trimmed
        .iter()
        .rev()
        .find(|l| !l.starts_with("note:") && !l.starts_with("warning:"))
        .copied()
        .or_else(|| trimmed.last().copied())
        .unwrap_or("");
    chosen.chars().take(220).collect()
}

fn preferred_live_paths() -> (PathBuf, PathBuf) {
    let json = PathBuf::from(DEFAULT_LIVE_JSON);
    let log = PathBuf::from(DEFAULT_SCANNER_LOG);
    if live_bundle_usable(&json, &log) {
        (json, log)
    } else {
        user_live_paths()
    }
}

fn user_live_paths() -> (PathBuf, PathBuf) {
    let user = std::env::var("USER").unwrap_or_else(|_| "user".into());
    (
        PathBuf::from(format!("/tmp/qcom_live_towers_{user}.json")),
        PathBuf::from(format!("/tmp/qcom_live_scanner_{user}.log")),
    )
}

fn live_bundle_usable(json: &Path, log: &Path) -> bool {
    let lock = PathBuf::from(format!("{}.lock", json.display()));
    path_writable_or_creatable(json)
        && path_writable_or_creatable(&lock)
        && path_writable_or_creatable(log)
}

fn path_writable_or_creatable(path: &Path) -> bool {
    if path.exists() {
        std::fs::OpenOptions::new()
            .write(true)
            .open(path)
            .map(|_| true)
            .unwrap_or(false)
    } else {
        match std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(path)
        {
            Ok(_) => {
                let _ = std::fs::remove_file(path);
                true
            }
            Err(_) => false,
        }
    }
}

fn remove_if_owned(path: &Path) {
    if path_writable_or_creatable(path) || !path.exists() {
        let _ = std::fs::remove_file(path);
    }
}

/// Kill only what we can without polkit (used on Start — no password dialogs).
fn soft_stop_user_scanners(shared: &SharedState, live_json: &Path) {
    if let Ok(mut guard) = shared.child.lock() {
        if let Some(mut child) = guard.take() {
            let _ = child.kill();
            let deadline = Instant::now() + Duration::from_millis(400);
            while Instant::now() < deadline {
                match child.try_wait() {
                    Ok(Some(_)) => break,
                    Ok(None) => thread::sleep(Duration::from_millis(40)),
                    Err(_) => break,
                }
            }
        }
    }
    for pid in find_live_scanner_pids(live_json) {
        let _ = signal_term(pid);
    }
    thread::sleep(Duration::from_millis(200));
    for pid in find_live_scanner_pids(live_json) {
        let _ = signal_kill(pid);
    }
}

fn stop_all_impl(shared: &SharedState, live_json: &Path) {
    // Drop our Child handle (usually pkexec wrapper) without blocking forever.
    if let Ok(mut guard) = shared.child.lock() {
        if let Some(mut child) = guard.take() {
            let _ = child.kill();
            let deadline = Instant::now() + Duration::from_millis(400);
            while Instant::now() < deadline {
                match child.try_wait() {
                    Ok(Some(_)) => break,
                    Ok(None) => thread::sleep(Duration::from_millis(40)),
                    Err(_) => break,
                }
            }
        }
    }

    let mut pids = find_live_scanner_pids(live_json);
    if let Ok(s) = std::fs::read_to_string(PID_PATH) {
        if let Ok(pid) = s.trim().parse::<i32>() {
            pids.push(pid);
        }
    }
    let lock_path = format!("{}.lock", live_json.display());
    if let Ok(s) = std::fs::read_to_string(&lock_path) {
        if let Ok(pid) = s.trim().parse::<i32>() {
            pids.push(pid);
        }
    }
    pids.sort_unstable();
    pids.dedup();
    pids.retain(|&pid| pid > 1 && process_alive(pid));

    // User-owned TERM first (no prompt).
    let mut need_elevated = Vec::new();
    for pid in &pids {
        if !signal_term(*pid) {
            need_elevated.push(*pid);
        }
    }

    // Give graceful shutdown a moment (live_scanner aborts AT on SIGTERM).
    thread::sleep(Duration::from_millis(600));
    let mut leftover: Vec<i32> = find_live_scanner_pids(live_json)
        .into_iter()
        .filter(|&p| process_alive(p))
        .collect();
    for pid in &need_elevated {
        if process_alive(*pid) && !leftover.contains(pid) {
            leftover.push(*pid);
        }
    }

    if !leftover.is_empty() {
        // One polkit prompt max — never call pkexec per-PID.
        kill_pids_elevated(&leftover);
        thread::sleep(Duration::from_millis(350));
    }

    // Last resort user KILL (works if somehow non-root).
    for pid in find_live_scanner_pids(live_json) {
        if process_alive(pid) {
            let _ = signal_kill(pid);
        }
    }

    let _ = std::fs::remove_file(PID_PATH);
}

fn any_live_scanner_for_json(json: &Path) -> bool {
    !find_live_scanner_pids(json).is_empty()
}

fn process_alive(pid: i32) -> bool {
    Path::new(&format!("/proc/{pid}")).exists()
}

/// PIDs of `live_scanner` whose cmdline contains our live-json path.
fn find_live_scanner_pids(json: &Path) -> Vec<i32> {
    let needle = json.to_string_lossy();
    let Ok(entries) = std::fs::read_dir("/proc") else {
        return Vec::new();
    };
    let mut out = Vec::new();
    for ent in entries.flatten() {
        let name = ent.file_name();
        let name = name.to_string_lossy();
        let Ok(pid) = name.parse::<i32>() else {
            continue;
        };
        let cmdline = std::fs::read(format!("/proc/{pid}/cmdline")).unwrap_or_default();
        if cmdline.is_empty() {
            continue;
        }
        let joined = cmdline
            .split(|b| *b == 0)
            .filter(|s| !s.is_empty())
            .map(|s| String::from_utf8_lossy(s).into_owned())
            .collect::<Vec<_>>()
            .join(" ");
        if !joined.contains("live_scanner") {
            continue;
        }
        // Avoid matching this GUI binary if it ever appears with the path in args.
        if joined.contains("tower_gui") {
            continue;
        }
        if joined.contains(needle.as_ref()) || needle.is_empty() {
            out.push(pid);
        }
    }
    out.sort_unstable();
    out.dedup();
    out
}

/// User-level TERM only — never pkexec (that must stay off the hot path / UI).
fn signal_term(pid: i32) -> bool {
    Command::new("kill")
        .args(["-TERM", &pid.to_string()])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

fn signal_kill(pid: i32) -> bool {
    Command::new("kill")
        .args(["-KILL", &pid.to_string()])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

/// Kill a batch of root scanners with a single pkexec (avoids N password prompts).
fn kill_pids_elevated(pids: &[i32]) {
    if pids.is_empty() {
        return;
    }
    let list = pids
        .iter()
        .map(|p| p.to_string())
        .collect::<Vec<_>>()
        .join(" ");
    // Graceful then hard — live_scanner should exit on TERM within ~1s after AT abort.
    let script = format!(
        "/bin/kill -TERM {list} 2>/dev/null; sleep 0.8; /bin/kill -KILL {list} 2>/dev/null; true"
    );
    let _ = Command::new("pkexec")
        .args(["/bin/sh", "-c", &script])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status();
}

fn resolve_live_scanner() -> PathBuf {
    let mut candidates = vec![
        PathBuf::from("./build/live_scanner"),
        PathBuf::from("../build/live_scanner"),
        PathBuf::from("build/live_scanner"),
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../build/live_scanner"),
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../build/live_scanner"),
    ];
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            candidates.push(dir.join("live_scanner"));
            candidates.push(dir.join("../../build/live_scanner"));
            candidates.push(dir.join("../../../build/live_scanner"));
        }
    }
    for c in candidates {
        if c.is_file() {
            if let Ok(canon) = c.canonicalize() {
                return canon;
            }
            return c;
        }
    }
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../build/live_scanner")
}

fn shell_quote(s: &str) -> String {
    if s.chars()
        .all(|c| c.is_ascii_alphanumeric() || "-_./:@+".contains(c))
    {
        s.to_string()
    } else {
        format!("'{}'", s.replace('\'', "'\\''"))
    }
}

#[allow(dead_code)]
pub fn binary_exists(path: &Path) -> bool {
    path.is_file()
}

#[cfg(test)]
mod spawn_hint_tests {
    use super::{pick_stderr_hint, ScannerControl, SurveyMode};
    use std::path::PathBuf;

    #[test]
    fn prefers_hard_error_over_cli_notes() {
        let lines = vec![
            "note: --survey-mode irat → survey walk".into(),
            "No modem with Diag port found. Pass --device /dev/ttyUSBx".into(),
        ];
        assert!(pick_stderr_hint(&lines).contains("No modem"));
    }

    #[test]
    fn falls_back_to_last_note() {
        let lines = vec!["note: survey → --duration 0 (until Ctrl+C)".into()];
        assert!(pick_stderr_hint(&lines).starts_with("note:"));
    }

    #[test]
    fn start_args_skip_recover_cfun_and_clip_bands() {
        let mut s = ScannerControl::new();
        s.mode = SurveyMode::Lte;
        s.live_json = PathBuf::from("/tmp/qcom_live_towers.json");
        s.scanner_log = PathBuf::from("/tmp/qcom_live_scanner.log");
        let args = s.build_args();
        let joined = args.join(" ");
        assert!(joined.contains("--survey-mode lte"));
        assert!(joined.contains("--hop-band-clip"));
        assert!(!joined.contains("--recover-cfun"));
        assert!(!joined.contains("--no-qmi"));
    }

    #[test]
    fn empty_chmod_list_is_ok() {
        assert!(super::pkexec_chmod_rw(&[]));
    }

    #[test]
    fn device_rw_on_owned_tempfile() {
        let p = std::env::temp_dir().join("observer_device_rw_test");
        std::fs::write(&p, b"x").unwrap();
        assert!(super::device_rw(&p));
        let _ = std::fs::remove_file(&p);
    }
}
