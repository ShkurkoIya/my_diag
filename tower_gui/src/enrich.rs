//! OpenCelliD enrichment — lookup by primary LTE identity + field-level diff.

use crate::model::{FlatTower, Rat};
use serde::Deserialize;
use std::collections::{HashMap, HashSet, VecDeque};
use std::sync::mpsc::{self, Receiver, Sender};
use std::thread;
use std::time::{Duration, Instant};

/// Primary identity used for external match (LTE CGI).
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct PrimaryId {
    pub rat: String,
    pub mcc: u32,
    pub mnc: u32,
    pub tac: u32,
    pub cid: u64,
}

impl PrimaryId {
    pub fn cache_key(&self) -> String {
        format!(
            "{}|{}|{}|{}|{}",
            self.rat, self.mcc, self.mnc, self.tac, self.cid
        )
    }

    pub fn from_tower(ft: &FlatTower) -> Option<Self> {
        if ft.rat != Rat::Lte {
            return None;
        }
        let t = &ft.tower;
        let mcc: u32 = t.get_id("mcc").parse().ok()?;
        let mnc_raw = t.get_id("mnc");
        let mnc: u32 = mnc_raw.parse().ok()?;
        let tac: u32 = t.lac_or_tac().parse().ok()?;
        let cid: u64 = t.get_id("cid").parse().ok()?;
        if mcc == 0 || tac == 0 || cid == 0 {
            return None;
        }
        Some(Self {
            rat: "LTE".into(),
            mcc,
            mnc,
            tac,
            cid,
        })
    }
}

#[derive(Clone, Debug, Default)]
pub struct ExtCell {
    pub radio: String,
    pub mcc: String,
    pub mnc: String,
    pub lac: String,
    pub cellid: String,
    pub lat: String,
    pub lon: String,
    pub range_m: String,
    pub samples: String,
    pub average_signal: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DiffSide {
    /// Values equal after normalize.
    Match,
    /// Present on both sides, different.
    Mismatch,
    /// Only in our feed (complements the merge).
    OursOnly,
    /// Only in external DB (complements the merge).
    ExtOnly,
}

#[derive(Clone, Debug)]
pub struct FieldDiff {
    pub label: &'static str,
    pub ours: String,
    pub ext: String,
    pub side: DiffSide,
}

impl FieldDiff {
    /// Effective merged value for the union passport.
    pub fn merged_value(&self) -> &str {
        match self.side {
            DiffSide::Match | DiffSide::OursOnly => self.ours.as_str(),
            DiffSide::ExtOnly => self.ext.as_str(),
            DiffSide::Mismatch => self.ours.as_str(), // prefer ours; UI shows both
        }
    }

}

#[derive(Clone, Debug)]
pub enum ExtStatus {
    /// No API key configured.
    NoKey,
    /// Incomplete identity — cannot look up.
    Skipped,
    Pending,
    /// Primary identity not in OpenCelliD → unmatched (red).
    /// `diffs` still carries the full field union (OCI side empty).
    NotInDb { diffs: Vec<FieldDiff> },
    /// Found; `diffs` is the honest field comparison / merge plan.
    Found { ext: ExtCell, diffs: Vec<FieldDiff> },
    Error(String),
}

impl ExtStatus {
    /// True when primary identity failed to resolve in external DB.
    pub fn is_unmatched(&self) -> bool {
        matches!(self, ExtStatus::NotInDb { .. })
    }

    /// Soft warning: found but some comparable fields disagree.
    pub fn has_field_mismatch(&self) -> bool {
        match self {
            ExtStatus::Found { diffs, .. } => diffs.iter().any(|d| d.side == DiffSide::Mismatch),
            _ => false,
        }
    }

    pub fn diffs(&self) -> Option<&[FieldDiff]> {
        match self {
            ExtStatus::Found { diffs, .. } | ExtStatus::NotInDb { diffs } => Some(diffs),
            _ => None,
        }
    }
}

#[derive(Deserialize)]
struct OciJson {
    #[serde(default)]
    lat: Option<f64>,
    #[serde(default)]
    lon: Option<f64>,
    #[serde(default)]
    range: Option<f64>,
    #[serde(default)]
    samples: Option<u64>,
    #[serde(default, rename = "averageSignal")]
    average_signal: Option<i64>,
    #[serde(default)]
    radio: Option<String>,
    #[serde(default)]
    mcc: Option<u32>,
    #[serde(default)]
    mnc: Option<u32>,
    #[serde(default)]
    lac: Option<u32>,
    #[serde(default)]
    cellid: Option<u64>,
    #[serde(default)]
    error: Option<String>,
    #[serde(default)]
    code: Option<i32>,
}

fn fmt_opt_f64(v: Option<f64>, prec: usize) -> String {
    v.map(|x| format!("{x:.prec$}"))
        .unwrap_or_default()
}

fn fmt_opt_u(v: Option<u64>) -> String {
    v.map(|x| x.to_string()).unwrap_or_default()
}

fn fmt_opt_i(v: Option<i64>) -> String {
    v.map(|x| x.to_string()).unwrap_or_default()
}

fn norm_num(s: &str) -> String {
    let t = s.trim();
    if t.is_empty() {
        return String::new();
    }
    // Strip leading zeros for MNC-style compares ("02" == "2").
    if let Ok(n) = t.parse::<u64>() {
        return n.to_string();
    }
    t.to_lowercase()
}

fn push_diff(
    out: &mut Vec<FieldDiff>,
    label: &'static str,
    ours: impl Into<String>,
    ext: impl Into<String>,
) {
    let ours = ours.into();
    let ext = ext.into();
    let side = match (ours.is_empty(), ext.is_empty()) {
        (true, true) => return,
        (false, false) => {
            if norm_num(&ours) == norm_num(&ext) {
                DiffSide::Match
            } else {
                DiffSide::Mismatch
            }
        }
        (false, true) => DiffSide::OursOnly,
        (true, false) => DiffSide::ExtOnly,
    };
    out.push(FieldDiff {
        label,
        ours,
        ext,
        side,
    });
}

/// Full union of our RF passport + OpenCelliD row (OCI may be empty → NotInDb view).
pub fn build_diffs(ft: &FlatTower, ext: &ExtCell) -> Vec<FieldDiff> {
    let t = &ft.tower;
    let mut d = Vec::new();

    // ── Shared / interchangeable identity ──
    push_diff(&mut d, "Radio", ft.rat.as_str(), &ext.radio);
    push_diff(&mut d, "MCC", t.get_id("mcc"), &ext.mcc);
    push_diff(&mut d, "MNC", t.get_id("mnc"), &ext.mnc);
    push_diff(&mut d, "TAC / LAC", t.lac_or_tac(), &ext.lac);
    push_diff(&mut d, "CID", t.get_id("cid"), &ext.cellid);

    // ── Ours (modem survey) — complements OCI ──
    push_diff(&mut d, "eNB", t.get_id("enb_id"), "");
    push_diff(&mut d, "Sector (ncell)", t.get_id("ncell_id"), "");
    push_diff(&mut d, "EARFCN", t.channel(), "");
    push_diff(&mut d, "PCI", t.cell_code(), "");
    push_diff(&mut d, "Band", t.band(), "");
    push_diff(&mut d, "Duplex", t.get_radio("duplex_type"), "");
    push_diff(&mut d, "DL MHz", t.get_radio("dl_freq"), "");
    push_diff(&mut d, "UL MHz", t.get_radio("ul_freq"), "");
    push_diff(&mut d, "Bandwidth MHz", t.get_radio("bandwidth"), "");
    push_diff(&mut d, "RSRP", t.rxl(), "");
    push_diff(&mut d, "RSRQ", t.get_sig("rsrq"), "");
    push_diff(&mut d, "RSSI", t.get_sig("rssi"), "");

    // ── OCI crowd geo / stats — complements ours ──
    push_diff(&mut d, "Latitude", "", &ext.lat);
    push_diff(&mut d, "Longitude", "", &ext.lon);
    push_diff(&mut d, "Range m", "", &ext.range_m);
    push_diff(&mut d, "Samples", "", &ext.samples);
    push_diff(&mut d, "Avg signal", "", &ext.average_signal);

    d
}

pub fn empty_ext() -> ExtCell {
    ExtCell::default()
}

struct Job {
    id: PrimaryId,
}

struct JobResult {
    key: String,
    status: ExtStatus,
}

/// Shared enrichment store + background OpenCelliD worker.
pub struct Enrichment {
    api_key: Option<String>,
    cache: HashMap<String, ExtStatus>,
    in_flight: HashSet<String>,
    queue_tx: Sender<Job>,
    result_rx: Receiver<JobResult>,
    /// Rate-limit UI enqueue storms.
    last_enqueue: Instant,
    stats_ok: u32,
    stats_miss: u32,
    stats_err: u32,
}

impl Enrichment {
    pub fn new() -> Self {
        let api_key = std::env::var("OPENCELLID_API_KEY")
            .ok()
            .map(|s| s.trim().to_string())
            .filter(|s| !s.is_empty());

        let (queue_tx, queue_rx) = mpsc::channel::<Job>();
        let (result_tx, result_rx) = mpsc::channel::<JobResult>();

        let key_for_worker = api_key.clone();
        thread::Builder::new()
            .name("opencellid".into())
            .spawn(move || worker_loop(key_for_worker, queue_rx, result_tx))
            .ok();

        Self {
            api_key,
            cache: HashMap::new(),
            in_flight: HashSet::new(),
            queue_tx,
            result_rx,
            last_enqueue: Instant::now() - Duration::from_secs(10),
            stats_ok: 0,
            stats_miss: 0,
            stats_err: 0,
        }
    }

    pub fn has_key(&self) -> bool {
        self.api_key.is_some()
    }

    /// True while background lookups are in flight (not merely "not cached yet").
    pub fn has_pending(&self) -> bool {
        !self.in_flight.is_empty()
    }

    pub fn poll_results(&mut self) {
        while let Ok(r) = self.result_rx.try_recv() {
            self.in_flight.remove(&r.key);
            match &r.status {
                ExtStatus::Found { .. } => self.stats_ok += 1,
                ExtStatus::NotInDb { .. } => self.stats_miss += 1,
                ExtStatus::Error(_) => self.stats_err += 1,
                _ => {}
            }
            self.cache.insert(r.key, r.status);
        }
    }

    pub fn status_line(&self) -> String {
        if self.api_key.is_none() {
            return "OCI off · set OPENCELLID_API_KEY".into();
        }
        format!(
            "OCI ok={} miss={} err={} pending={}",
            self.stats_ok,
            self.stats_miss,
            self.stats_err,
            self.in_flight.len()
        )
    }

    pub fn lookup(&mut self, ft: &FlatTower) -> ExtStatus {
        let Some(id) = PrimaryId::from_tower(ft) else {
            return ExtStatus::Skipped;
        };
        if self.api_key.is_none() {
            return ExtStatus::NoKey;
        }
        let key = id.cache_key();
        if let Some(s) = self.cache.get(&key) {
            return s.clone();
        }
        if self.in_flight.contains(&key) {
            return ExtStatus::Pending;
        }
        self.in_flight.insert(key.clone());
        self.cache.insert(key.clone(), ExtStatus::Pending);
        let _ = self.queue_tx.send(Job { id });
        ExtStatus::Pending
    }

    /// Ensure FULL LTE rows are queued (throttled).
    pub fn enqueue_flat(&mut self, flat: &[FlatTower]) {
        if self.api_key.is_none() {
            return;
        }
        if self.last_enqueue.elapsed() < Duration::from_millis(800) {
            return;
        }
        self.last_enqueue = Instant::now();
        let mut n = 0;
        for ft in flat {
            if n >= 40 {
                break;
            }
            let Some(id) = PrimaryId::from_tower(ft) else {
                continue;
            };
            let key = id.cache_key();
            if self.cache.contains_key(&key) || self.in_flight.contains(&key) {
                continue;
            }
            self.in_flight.insert(key.clone());
            self.cache.insert(key, ExtStatus::Pending);
            if self.queue_tx.send(Job { id }).is_err() {
                break;
            }
            n += 1;
        }
    }

    pub fn status_for_flat(&self, ft: &FlatTower) -> ExtStatus {
        let Some(id) = PrimaryId::from_tower(ft) else {
            return ExtStatus::Skipped;
        };
        if self.api_key.is_none() {
            return ExtStatus::NoKey;
        }
        let key = id.cache_key();
        if let Some(s) = self.cache.get(&key) {
            return s.clone();
        }
        if self.in_flight.contains(&key) {
            return ExtStatus::Pending;
        }
        // Not queued yet — don't paint Pending badges / force continuous repaint.
        ExtStatus::Skipped
    }
}

fn worker_loop(api_key: Option<String>, rx: Receiver<Job>, tx: Sender<JobResult>) {
    let Some(key) = api_key else {
        return;
    };
    // Simple local queue to pace requests (~1.2s) — OpenCelliD is rate-sensitive.
    let mut backlog: VecDeque<PrimaryId> = VecDeque::new();
    let pace = Duration::from_millis(1200);
    let mut last = Instant::now() - pace;

    loop {
        // Drain channel into backlog.
        match rx.recv_timeout(Duration::from_millis(200)) {
            Ok(job) => backlog.push_back(job.id),
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
        }
        while let Ok(job) = rx.try_recv() {
            backlog.push_back(job.id);
        }

        if backlog.is_empty() {
            continue;
        }
        let wait = pace.saturating_sub(last.elapsed());
        if !wait.is_zero() {
            thread::sleep(wait);
        }
        let Some(id) = backlog.pop_front() else {
            continue;
        };
        last = Instant::now();
        let status = fetch_cell(&key, &id);
        let key_s = id.cache_key();
        if tx
            .send(JobResult {
                key: key_s,
                status,
            })
            .is_err()
        {
            break;
        }
    }
}

fn fetch_cell(api_key: &str, id: &PrimaryId) -> ExtStatus {
    let url = format!(
        "https://opencellid.org/cell/get?key={}&radio=LTE&mcc={}&mnc={}&lac={}&cellid={}&format=json",
        urlencoding_minimal(api_key),
        id.mcc,
        id.mnc,
        id.tac,
        id.cid
    );

    let agent = ureq::AgentBuilder::new()
        .timeout_read(Duration::from_secs(12))
        .timeout_connect(Duration::from_secs(8))
        .build();

    let resp = match agent.get(&url).call() {
        Ok(r) => r,
        Err(ureq::Error::Status(code, resp)) => {
            let body = resp.into_string().unwrap_or_default();
            if let Ok(j) = serde_json::from_str::<OciJson>(&body) {
                if j.code == Some(1)
                    || j.error
                        .as_deref()
                        .map(|e| e.to_ascii_lowercase().contains("not found"))
                        .unwrap_or(false)
                {
                    return ExtStatus::NotInDb {
                        diffs: Vec::new(),
                    };
                }
            }
            return ExtStatus::Error(format!("HTTP {code}"));
        }
        Err(e) => return ExtStatus::Error(e.to_string()),
    };

    let body = match resp.into_string() {
        Ok(b) => b,
        Err(e) => return ExtStatus::Error(e.to_string()),
    };

    let j: OciJson = match serde_json::from_str(&body) {
        Ok(j) => j,
        Err(e) => return ExtStatus::Error(format!("json: {e}")),
    };

    if j.code == Some(1)
        || j.error
            .as_deref()
            .map(|e| e.to_ascii_lowercase().contains("not found"))
            .unwrap_or(false)
    {
        return ExtStatus::NotInDb {
            diffs: Vec::new(),
        };
    }
    if j.lat.is_none() && j.lon.is_none() && j.cellid.is_none() {
        if j.error.is_some() {
            return ExtStatus::Error(j.error.unwrap_or_else(|| "unknown".into()));
        }
        return ExtStatus::NotInDb {
            diffs: Vec::new(),
        };
    }

    let ext = ExtCell {
        radio: j.radio.unwrap_or_else(|| "LTE".into()),
        mcc: j.mcc.map(|v| v.to_string()).unwrap_or_default(),
        mnc: j.mnc.map(|v| v.to_string()).unwrap_or_default(),
        lac: j.lac.map(|v| v.to_string()).unwrap_or_default(),
        cellid: j.cellid.map(|v| v.to_string()).unwrap_or_default(),
        lat: fmt_opt_f64(j.lat, 6),
        lon: fmt_opt_f64(j.lon, 6),
        range_m: fmt_opt_f64(j.range, 0),
        samples: fmt_opt_u(j.samples),
        average_signal: fmt_opt_i(j.average_signal),
    };

    // diffs rebuilt on UI side with the live tower.
    ExtStatus::Found {
        ext,
        diffs: Vec::new(),
    }
}

fn urlencoding_minimal(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for b in s.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                out.push(b as char)
            }
            _ => out.push_str(&format!("%{b:02X}")),
        }
    }
    out
}

/// Rebuild field union / diffs against current tower data.
pub fn with_fresh_diffs(status: ExtStatus, ft: &FlatTower) -> ExtStatus {
    match status {
        ExtStatus::Found { ext, .. } => {
            let diffs = build_diffs(ft, &ext);
            ExtStatus::Found { ext, diffs }
        }
        ExtStatus::NotInDb { .. } => ExtStatus::NotInDb {
            diffs: build_diffs(ft, &empty_ext()),
        },
        other => other,
    }
}
