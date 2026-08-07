//! Live Scan — poll live_scanner survey JSON and render Operator → eNB → cell tree.

use crate::enrich::{self, DiffSide, Enrichment, ExtStatus};
use crate::icons::{self, expand_toggle, nest_toggle};
use crate::model::{Document, FlatTower, Rat, Tower};
use crate::theme::{brand_color, SurfaceState, Theme};
use eframe::egui::{
    self, Align, Color32, CornerRadius, Frame, Key, Layout, Margin, Pos2, Rect, RichText,
    ScrollArea, Sense, Stroke, Ui, Vec2,
};
use egui_extras::{Column, TableBuilder};
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::f32::consts::TAU;
use std::path::PathBuf;
use std::time::{Duration, Instant, SystemTime};

#[derive(Clone, Copy, PartialEq, Eq, Default)]
pub enum LiveView {
    /// Flat table of every RF row (FULL + Heard).
    #[default]
    Table,
    /// Operator → eNB → cell hierarchy.
    Tree,
    Graph,
}

impl LiveView {
    fn cycle(self) -> Self {
        match self {
            Self::Table => Self::Tree,
            Self::Tree => Self::Graph,
            Self::Graph => Self::Table,
        }
    }

    fn label(self) -> &'static str {
        match self {
            Self::Table => "Table",
            Self::Tree => "List",
            Self::Graph => "Graph",
        }
    }

    fn icon(self) -> &'static str {
        match self {
            Self::Table => icons::TABLE,
            Self::Tree => icons::LIST_BULLETS,
            Self::Graph => icons::SHARE_NETWORK,
        }
    }

    fn hint(self) -> &'static str {
        match self {
            Self::Table => "All RF cells in one table",
            Self::Tree => "Operators · sites · carriers",
            Self::Graph => "Force-directed site graph",
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum GraphKind {
    Operator,
    Enb,
    Cell,
    Heard,
}

#[derive(Clone)]
struct GraphNode {
    id: String,
    kind: GraphKind,
    label: String,
    sub: String,
    color: Color32,
    flat_ix: Option<usize>,
    pos: Vec2,
    vel: Vec2,
    /// Soft home for cluster separation (operators / eNB anchors).
    home: Option<Vec2>,
}

#[derive(Clone, Copy)]
enum EdgeKind {
    Hierarchy,
    Neighbor,
}

#[derive(Clone)]
struct GraphEdge {
    a: usize,
    b: usize,
    kind: EdgeKind,
}

/// Expand key: `op:{plmn}`
fn op_key(plmn: &str) -> String {
    format!("op:{plmn}")
}

/// Expand key: `enb:{plmn}:{enb}`
fn enb_key(plmn: &str, enb: &str) -> String {
    format!("enb:{plmn}:{enb}")
}

/// Expand key for incomplete bucket under operator.
fn heard_key(plmn: &str) -> String {
    format!("heard:{plmn}")
}


/// Expand key for SIB/meas neighbor hints under a cell row.
fn cell_nb_key(tower_key: &str) -> String {
    format!("nb:{tower_key}")
}

fn normalize_plmn(raw: &str) -> String {
    let digits: String = raw.chars().filter(|c| c.is_ascii_digit()).collect();
    if digits.len() >= 5 {
        format!("{}-{}", &digits[..3], &digits[3..])
    } else if !raw.is_empty() {
        raw.to_string()
    } else {
        String::new()
    }
}

/// Russian PLMN brand name (color via [`brand_color`]).
fn operator_brand(plmn: &str) -> (&'static str, Color32) {
    let name = match plmn {
        "250-01" => "MTS",
        "250-02" => "MegaFon",
        "250-20" => "t2",
        "250-99" => "Beeline",
        "250-11" => "Yota",
        "250-32" => "Win Mobile",
        "250-35" => "Motiv",
        "250-50" => "SberMobile",
        _ => "",
    };
    (name, brand_color(plmn))
}

fn operator_label(plmn: &str) -> String {
    let (name, _) = operator_brand(plmn);
    if name.is_empty() {
        if plmn.is_empty() {
            "Unknown PLMN".into()
        } else {
            plmn.to_string()
        }
    } else {
        format!("{name}  ·  {plmn}")
    }
}

fn operator_short_label(plmn: &str) -> String {
    let (name, _) = operator_brand(plmn);
    if name.is_empty() {
        plmn.to_string()
    } else {
        name.to_string()
    }
}

fn cell_graph_label(t: &Tower) -> String {
    let cid = t.get_id("cid");
    if cid.is_empty() {
        format!("{}/{}", dash(t.channel()), dash(t.cell_code()))
    } else {
        format!("CID {cid}")
    }
}

fn cell_graph_sub(t: &Tower) -> String {
    format!(
        "{}/{}  {}",
        dash(t.channel()),
        dash(t.cell_code()),
        if t.band().is_empty() { "-" } else { t.band() }
    )
}

/// Graph cell fill — calm slate/brand; status is ring/badge, not neon fill.
fn cell_graph_color(t: &Tower, brand: Color32) -> Color32 {
    if t.is_serving() {
        Color32::from_rgb(72, 120, 100)
    } else if t.was_identity_camped() {
        Color32::from_rgb(120, 110, 72)
    } else {
        // Muted brand so hubs stay readable without rainbow leaves.
        Color32::from_rgb(
            ((brand.r() as u16 + 90) / 2) as u8,
            ((brand.g() as u16 + 100) / 2) as u8,
            ((brand.b() as u16 + 110) / 2) as u8,
        )
    }
}

fn site_graph_color(flat: &[FlatTower], site: &SiteNode) -> Color32 {
    let mut camped = false;
    for &ix in &site.cells {
        let Some(ft) = flat.get(ix) else { continue };
        if ft.tower.is_serving() {
            return Color32::from_rgb(72, 120, 100);
        }
        if ft.tower.was_identity_camped() {
            camped = true;
        }
    }
    if camped {
        Color32::from_rgb(120, 110, 72)
    } else {
        Color32::from_rgb(90, 100, 118)
    }
}

fn site_sub_label(site: &SiteNode) -> String {
    if site.tac.is_empty() {
        format!("{} full", site.cells.len())
    } else {
        format!("TAC {} · {} full", site.tac, site.cells.len())
    }
}

#[derive(Clone)]
pub struct SiteNode {
    pub key: String,
    pub enb: String,
    pub tac: String,
    pub best_rsrp: f32,
    /// FULL cells only (have CID under this eNB).
    pub cells: Vec<usize>,
}

#[derive(Clone)]
pub struct OperatorNode {
    pub key: String,
    pub plmn: String,
    pub color: Color32,
    pub sites: Vec<SiteNode>,
    /// Heard RF: PCI seen, no CID yet. Not claimed under any eNB.
    pub incomplete: Vec<usize>,
    pub best_rsrp: f32,
}

pub struct LiveState {
    pub path: PathBuf,
    pub watching: bool,
    pub last_err: String,
    pub last_mtime: Option<SystemTime>,
    pub last_load: Option<Instant>,
    pub doc: Option<Document>,
    pub flat: Vec<FlatTower>,
    pub operators: Vec<OperatorNode>,
    pub unknown_lte: Vec<usize>,
    pub other_rat: Vec<usize>,
    pub expanded: BTreeSet<String>,
    pub selected: Option<usize>,
    pub view: LiveView,
    /// Free-text search (CID, eNB, EARFCN, PCI, PLMN, brand, key…).
    pub search_query: String,
    /// Index into current match list for Next/Prev.
    search_cursor: usize,
    /// Request ScrollArea to bring the selected tree row into view.
    scroll_to_selected: bool,
    /// Request graph camera to center on the selected cell/site.
    focus_graph_selected: bool,
    /// One-shot: focus the search TextEdit (Ctrl+F / `/`).
    focus_search: bool,
    graph_nodes: Vec<GraphNode>,
    graph_edges: Vec<GraphEdge>,
    /// Accumulated neighbor links by stable node id (only grows within GUI session).
    graph_neighbor_pairs: BTreeSet<(String, String)>,
    graph_pan: Vec2,
    graph_zoom: f32,
    graph_fp: String,
    graph_drag: Option<usize>,
    /// Physics frozen after settle; wakes on drag / topology change.
    graph_settled: bool,
    /// OpenCelliD primary-identity lookup + field diffs.
    enrich: Enrichment,
}

impl LiveState {
    pub fn new() -> Self {
        Self {
            path: PathBuf::from("/tmp/qcom_live_towers.json"),
            watching: true,
            last_err: String::new(),
            last_mtime: None,
            last_load: None,
            doc: None,
            flat: Vec::new(),
            operators: Vec::new(),
            unknown_lte: Vec::new(),
            other_rat: Vec::new(),
            expanded: BTreeSet::new(),
            selected: None,
            view: LiveView::Tree,
            search_query: String::new(),
            search_cursor: 0,
            scroll_to_selected: false,
            focus_graph_selected: false,
            focus_search: false,
            graph_nodes: Vec::new(),
            graph_edges: Vec::new(),
            graph_neighbor_pairs: BTreeSet::new(),
            graph_pan: Vec2::ZERO,
            graph_zoom: 1.0,
            graph_fp: String::new(),
            graph_drag: None,
            graph_settled: false,
            enrich: Enrichment::new(),
        }
    }

    fn ext_status(&mut self, ix: usize) -> ExtStatus {
        let Some(ft) = self.flat.get(ix) else {
            return ExtStatus::Skipped;
        };
        let st = self.enrich.lookup(ft);
        enrich::with_fresh_diffs(st, ft)
    }

    fn ext_status_ref(&self, ix: usize) -> ExtStatus {
        let Some(ft) = self.flat.get(ix) else {
            return ExtStatus::Skipped;
        };
        enrich::with_fresh_diffs(self.enrich.status_for_flat(ft), ft)
    }

    fn site_unmatched(&self, site: &SiteNode) -> bool {
        site.cells
            .iter()
            .any(|&ix| self.ext_status_ref(ix).is_unmatched())
    }

    fn search_active(&self) -> bool {
        !self.search_query.trim().is_empty()
    }

    /// Flat indices matching the current query (order = feed order).
    fn search_matches(&self) -> Vec<usize> {
        let q = self.search_query.trim().to_lowercase();
        if q.is_empty() {
            return Vec::new();
        }
        let tokens: Vec<&str> = q.split_whitespace().collect();
        self.flat
            .iter()
            .enumerate()
            .filter(|(_, ft)| {
                let hay = tower_search_haystack(ft);
                tokens.iter().all(|tok| hay.contains(tok))
            })
            .map(|(i, _)| i)
            .collect()
    }

    fn match_set(&self) -> BTreeSet<usize> {
        self.search_matches().into_iter().collect()
    }

    /// Expand tree path + select + scroll (stays on / switches to List).
    pub fn reveal_in_tree(&mut self, ix: usize) {
        if ix >= self.flat.len() {
            return;
        }
        self.selected = Some(ix);
        self.view = LiveView::Tree;
        if let Some(loc) = self.locate_in_tree(ix) {
            match loc {
                TreeLoc::Site { op_key, site_key } => {
                    self.expanded.insert(op_key);
                    self.expanded.insert(site_key);
                    if let Some(ft) = self.flat.get(ix) {
                        if ft.tower.neighbor_count() > 0 {
                            self.expanded.insert(cell_nb_key(&ft.tower.key));
                        }
                    }
                }
                TreeLoc::Incomplete { op_key, heard_key } => {
                    self.expanded.insert(op_key);
                    self.expanded.insert(heard_key);
                }
                TreeLoc::UnknownLte | TreeLoc::OtherRat => {}
            }
        }
        self.scroll_to_selected = true;
        self.sync_search_cursor(ix);
    }

    /// Select + switch to flat table + scroll to row.
    pub fn reveal_in_table(&mut self, ix: usize) {
        if ix >= self.flat.len() {
            return;
        }
        self.selected = Some(ix);
        self.view = LiveView::Table;
        self.scroll_to_selected = true;
        self.sync_search_cursor(ix);
    }

    /// Select + switch to Graph + pan camera onto the cell (or its eNB).
    pub fn reveal_in_graph(&mut self, ix: usize) {
        if ix >= self.flat.len() {
            return;
        }
        self.selected = Some(ix);
        self.view = LiveView::Graph;
        self.rebuild_graph_if_needed();
        self.focus_graph_selected = true;
        self.sync_search_cursor(ix);
    }

    fn sync_search_cursor(&mut self, ix: usize) {
        let matches = self.search_matches();
        if let Some(pos) = matches.iter().position(|&m| m == ix) {
            self.search_cursor = pos;
        }
    }

    fn reveal_in_current_view(&mut self, ix: usize) {
        match self.view {
            LiveView::Table => self.reveal_in_table(ix),
            LiveView::Tree => self.reveal_in_tree(ix),
            LiveView::Graph => self.reveal_in_graph(ix),
        }
    }

    fn locate_in_tree(&self, ix: usize) -> Option<TreeLoc> {
        for op in &self.operators {
            for site in &op.sites {
                if site.cells.contains(&ix) {
                    return Some(TreeLoc::Site {
                        op_key: op.key.clone(),
                        site_key: site.key.clone(),
                    });
                }
            }
            if op.incomplete.contains(&ix) {
                return Some(TreeLoc::Incomplete {
                    op_key: op.key.clone(),
                    heard_key: heard_key(&op.plmn),
                });
            }
        }
        if self.unknown_lte.contains(&ix) {
            return Some(TreeLoc::UnknownLte);
        }
        if self.other_rat.contains(&ix) {
            return Some(TreeLoc::OtherRat);
        }
        None
    }

    fn graph_focus_node_ix(&self) -> Option<usize> {
        let sel = self.selected?;
        let key = self.flat.get(sel)?.tower.key.as_str();
        let cell_id = format!("cell:{key}");
        if let Some(i) = self.graph_nodes.iter().position(|n| n.id == cell_id) {
            return Some(i);
        }
        // FULL cell without graph leaf → parent eNB.
        for op in &self.operators {
            for site in &op.sites {
                if site.cells.contains(&sel) {
                    return self.graph_nodes.iter().position(|n| n.id == site.key);
                }
            }
        }
        None
    }

    fn apply_graph_focus(&mut self, viewport: Rect) {
        if !self.focus_graph_selected {
            return;
        }
        self.focus_graph_selected = false;
        let Some(ni) = self.graph_focus_node_ix() else {
            return;
        };
        let Some(n) = self.graph_nodes.get(ni) else {
            return;
        };
        // Prefer a readable zoom when jumping from search / inspector.
        if self.graph_zoom < 0.9 {
            self.graph_zoom = 1.15;
        }
        // screen = center + pan + pos * zoom  →  center node: pan = -pos * zoom
        let _ = viewport;
        self.graph_pan = -n.pos * self.graph_zoom;
    }

    /// Jump along matches and reveal in the active view.
    /// If the current selection is already a match, move by `delta`; otherwise land on cursor.
    fn jump_search_cursor(&mut self, delta: isize) {
        let matches = self.search_matches();
        if matches.is_empty() {
            return;
        }
        let n = matches.len();
        if let Some(pos) = self
            .selected
            .and_then(|s| matches.iter().position(|&m| m == s))
        {
            self.search_cursor = ((pos as isize + delta).rem_euclid(n as isize)) as usize;
        } else if delta < 0 {
            self.search_cursor = n - 1;
        } else {
            self.search_cursor = self.search_cursor.min(n - 1);
        }
        let ix = matches[self.search_cursor];
        self.reveal_in_current_view(ix);
    }
}

enum TreeLoc {
    Site { op_key: String, site_key: String },
    Incomplete { op_key: String, heard_key: String },
    UnknownLte,
    OtherRat,
}

fn tower_search_haystack(ft: &FlatTower) -> String {
    let t = &ft.tower;
    let plmn = normalize_plmn(t.plmn());
    let (brand, _) = operator_brand(&plmn);
    format!(
        "{} {} {} {} {} {} {} {} {} {} {} {} {} {} cid{} enb{} tac{} earfcn{} pci{}",
        t.key,
        ft.rat.as_str(),
        plmn,
        brand.to_lowercase(),
        t.get_id("cid"),
        t.get_id("enb_id"),
        t.get_id("rnc_id"),
        t.get_id("ncell_id"),
        t.lac_or_tac(),
        t.channel(),
        t.cell_code(),
        t.band(),
        t.get_radio("dl_freq"),
        t.get_radio("ul_freq"),
        t.get_id("cid"),
        t.get_id("enb_id"),
        t.lac_or_tac(),
        t.channel(),
        t.cell_code(),
    )
    .to_lowercase()
}

impl LiveState {
    /// True while Graph view wants continuous frames (force layout settling / drag).
    pub fn wants_continuous_repaint(&self) -> bool {
        let graph_busy = self.view == LiveView::Graph
            && !self.graph_nodes.is_empty()
            && (!self.graph_settled || self.graph_drag.is_some());
        // Only while real OCI jobs are in flight — not for every uncached row.
        graph_busy || self.enrich.has_pending()
    }

    pub fn tick(&mut self, dt: f32) {
        self.enrich.poll_results();
        if !self.flat.is_empty() {
            self.enrich.enqueue_flat(&self.flat);
        }
        if self.view == LiveView::Graph && !self.graph_nodes.is_empty() && !self.graph_settled
        {
            self.step_graph_physics(dt);
        }
        if !self.watching {
            return;
        }
        let due = self
            .last_load
            .map(|t| t.elapsed() >= Duration::from_millis(500))
            .unwrap_or(true);
        if due {
            self.poll();
        }
    }

    pub fn poll(&mut self) {
        self.last_load = Some(Instant::now());
        let meta = match std::fs::metadata(&self.path) {
            Ok(m) => m,
            Err(e) => {
                self.last_err = format!("waiting for {} ({})", self.path.display(), e);
                return;
            }
        };
        let mtime = meta.modified().ok();
        if mtime == self.last_mtime && self.doc.is_some() {
            self.last_err.clear();
            return;
        }
        match Document::load(&self.path) {
            Ok(doc) => {
                self.last_mtime = mtime;
                self.last_err.clear();
                // Selection is a flat index — remapping by stable tower key across reloads.
                let selected_key = self
                    .selected
                    .and_then(|ix| self.flat.get(ix))
                    .map(|ft| ft.tower.key.clone());
                self.flat = doc.flatten();
                self.selected = selected_key.and_then(|k| {
                    self.flat.iter().position(|ft| ft.tower.key == k)
                });
                self.rebuild_tree();
                self.doc = Some(doc);
                // Do NOT touch expanded — user controls open/closed state across reloads.
            }
            Err(e) => {
                self.last_err = e;
            }
        }
    }

    fn rebuild_tree(&mut self) {
        // plmn -> (enb -> SiteNode builder), incompletes, best
        struct OpAcc {
            sites: BTreeMap<String, SiteNode>,
            incomplete: Vec<usize>,
            best_rsrp: f32,
        }

        let mut ops: BTreeMap<String, OpAcc> = BTreeMap::new();
        let mut unknown_lte = Vec::new();
        let mut other_rat = Vec::new();
        let mut pending_radio: Vec<(String, usize)> = Vec::new(); // (plmn, flat ix)

        for (i, ft) in self.flat.iter().enumerate() {
            if ft.rat != Rat::Lte {
                other_rat.push(i);
                continue;
            }
            let plmn = normalize_plmn(ft.tower.plmn());
            let enb = ft.tower.get_id("enb_id");
            let cid = ft.tower.get_id("cid");
            let r = parse_f32(ft.tower.rxl());

            if plmn.is_empty() {
                // No PLMN: never nest under an eNB/operator.
                unknown_lte.push(i);
                continue;
            }

            let acc = ops.entry(plmn.clone()).or_insert_with(|| OpAcc {
                sites: BTreeMap::new(),
                incomplete: Vec::new(),
                best_rsrp: -999.0,
            });
            if r > acc.best_rsrp {
                acc.best_rsrp = r;
            }

            // Only FULL identity cells seed an eNB node.
            if !enb.is_empty() && !cid.is_empty() {
                let sk = enb_key(&plmn, enb);
                let node = acc.sites.entry(enb.to_string()).or_insert_with(|| SiteNode {
                    key: sk,
                    enb: enb.to_string(),
                    tac: ft.tower.lac_or_tac().to_string(),
                    best_rsrp: -999.0,
                    cells: Vec::new(),
                });
                if node.tac.is_empty() {
                    node.tac = ft.tower.lac_or_tac().to_string();
                }
                if r > node.best_rsrp {
                    node.best_rsrp = r;
                }
                node.cells.push(i);
            } else {
                pending_radio.push((plmn, i));
            }
        }

        // Heard RF stays under the operator — never claimed as child of an eNB.
        for (plmn, ix) in pending_radio {
            let Some(acc) = ops.get_mut(&plmn) else {
                unknown_lte.push(ix);
                continue;
            };
            acc.incomplete.push(ix);
        }

        let sort_ix = |flat: &[FlatTower], a: usize, b: usize| {
            let sa = flat[a].tower.is_serving();
            let sb = flat[b].tower.is_serving();
            let ca = flat[a].tower.was_identity_camped();
            let cb = flat[b].tower.was_identity_camped();
            sb.cmp(&sa)
                .then_with(|| cb.cmp(&ca))
                .then_with(|| {
                    parse_f32(flat[b].tower.rxl())
                        .partial_cmp(&parse_f32(flat[a].tower.rxl()))
                        .unwrap_or(std::cmp::Ordering::Equal)
                })
        };

        let mut operators: Vec<OperatorNode> = ops
            .into_iter()
            .map(|(plmn, mut acc)| {
                for s in acc.sites.values_mut() {
                    s.cells.sort_by(|&a, &b| sort_ix(&self.flat, a, b));
                }
                acc.incomplete
                    .sort_by(|&a, &b| sort_ix(&self.flat, a, b));
                let mut sites: Vec<SiteNode> = acc.sites.into_values().collect();
                sites.sort_by(|a, b| {
                    b.best_rsrp
                        .partial_cmp(&a.best_rsrp)
                        .unwrap_or(std::cmp::Ordering::Equal)
                });
                let (_, color) = operator_brand(&plmn);
                OperatorNode {
                    key: op_key(&plmn),
                    plmn,
                    color,
                    sites,
                    incomplete: acc.incomplete,
                    best_rsrp: acc.best_rsrp,
                }
            })
            .collect();

        operators.sort_by(|a, b| {
            b.best_rsrp
                .partial_cmp(&a.best_rsrp)
                .unwrap_or(std::cmp::Ordering::Equal)
                .then_with(|| a.plmn.cmp(&b.plmn))
        });

        unknown_lte.sort_by(|&a, &b| sort_ix(&self.flat, a, b));
        other_rat.sort_by(|&a, &b| sort_ix(&self.flat, a, b));

        // Drop selected index if out of range after reload.
        if let Some(ix) = self.selected {
            if ix >= self.flat.len() {
                self.selected = None;
            }
        }

        self.operators = operators;
        self.unknown_lte = unknown_lte;
        self.other_rat = other_rat;
        self.rebuild_graph_if_needed();
    }

    fn graph_fingerprint(&self) -> String {
        // Stable topology id — no flat index (order in JSON must not reshuffle layout).
        let mut keys: Vec<String> = Vec::new();
        for op in &self.operators {
            keys.push(format!("O:{}", op.key));
            for site in &op.sites {
                keys.push(format!("E:{}", site.key));
                for &ix in &site.cells {
                    if let Some(ft) = self.flat.get(ix) {
                        keys.push(format!("C:{}", ft.tower.key));
                    }
                }
            }
        }
        keys.sort();
        let mut s = String::from("v3-stable;");
        for k in keys {
            s.push_str(&k);
            s.push(';');
        }
        s
    }

    fn rebuild_graph_if_needed(&mut self) {
        let fp = self.graph_fingerprint();
        let structure_changed = fp != self.graph_fp || self.graph_nodes.is_empty();
        if structure_changed {
            self.graph_fp = fp;
            self.layout_graph();
            self.graph_settled = false;
        } else {
            // Remap cell flat indices + refresh appearance without moving nodes.
            // eNB/Operator must NEVER read flat_ix for labels — that index is only a
            // cell pointer and goes stale when flat order / site.cells sort changes.
            self.refresh_graph_appearance();
        }
        // Always absorb newly observed neighbor pairs into the session set.
        let before = self.graph_neighbor_pairs.len();
        self.accumulate_neighbor_pairs();
        if self.graph_neighbor_pairs.len() != before || structure_changed {
            self.rebuild_graph_edges_from_accum();
        }
    }

    /// Update labels/colors/flat_ix from the current operator tree without relayout.
    fn refresh_graph_appearance(&mut self) {
        let key_to_ix: HashMap<String, usize> = self
            .flat
            .iter()
            .enumerate()
            .map(|(i, ft)| (ft.tower.key.clone(), i))
            .collect();
        // Owned snapshots — avoid borrow conflicts with &mut graph_nodes.
        let op_look: HashMap<String, (String, String, Color32)> = self
            .operators
            .iter()
            .map(|op| {
                (
                    op.key.clone(),
                    (operator_short_label(&op.plmn), op.plmn.clone(), op.color),
                )
            })
            .collect();
        let site_look: HashMap<String, (String, String, Color32)> = self
            .operators
            .iter()
            .flat_map(|op| {
                op.sites.iter().map(|site| {
                    (
                        site.key.clone(),
                        (
                            format!("eNB {}", site.enb),
                            site_sub_label(site),
                            site_graph_color(&self.flat, site),
                        ),
                    )
                })
            })
            .collect();
        let cell_look: HashMap<String, (String, String, Color32)> = self
            .flat
            .iter()
            .map(|ft| {
                let t = &ft.tower;
                let plmn = normalize_plmn(t.plmn());
                let (_, brand) = operator_brand(&plmn);
                (
                    t.key.clone(),
                    (
                        cell_graph_label(t),
                        cell_graph_sub(t),
                        cell_graph_color(t, brand),
                    ),
                )
            })
            .collect();

        for n in &mut self.graph_nodes {
            match n.kind {
                GraphKind::Operator => {
                    n.flat_ix = None;
                    if let Some((label, sub, color)) = op_look.get(&n.id) {
                        n.label = label.clone();
                        n.sub = sub.clone();
                        n.color = *color;
                    }
                }
                GraphKind::Enb => {
                    // Identity is site.key / site.enb — never a cell CID.
                    n.flat_ix = None;
                    if let Some((label, sub, color)) = site_look.get(&n.id) {
                        n.label = label.clone();
                        n.sub = sub.clone();
                        n.color = *color;
                    }
                }
                GraphKind::Cell => {
                    if let Some(rest) = n.id.strip_prefix("cell:") {
                        n.flat_ix = key_to_ix.get(rest).copied();
                        if let Some((label, sub, color)) = cell_look.get(rest) {
                            n.label = label.clone();
                            n.sub = sub.clone();
                            n.color = *color;
                        }
                    } else {
                        n.flat_ix = None;
                    }
                }
                GraphKind::Heard => {
                    n.flat_ix = None;
                }
            }
        }
    }

    /// First FULL cell under an eNB site (for click → detail panel).
    fn site_primary_flat_ix(&self, site_key: &str) -> Option<usize> {
        for op in &self.operators {
            for site in &op.sites {
                if site.key == site_key {
                    return site.cells.first().copied();
                }
            }
        }
        None
    }

    fn accumulate_neighbor_pairs(&mut self) {
        let mut rf_to_id: HashMap<(String, String), String> = HashMap::new();
        for n in &self.graph_nodes {
            if n.kind != GraphKind::Cell {
                continue;
            }
            let Some(ix) = n.flat_ix else { continue };
            let Some(ft) = self.flat.get(ix) else { continue };
            let ch = ft.tower.channel().to_string();
            let code = ft.tower.cell_code().to_string();
            if !ch.is_empty() && !code.is_empty() {
                rf_to_id.insert((ch, code), n.id.clone());
            }
        }
        for n in &self.graph_nodes {
            if n.kind != GraphKind::Cell {
                continue;
            }
            let Some(ix) = n.flat_ix else { continue };
            let Some(ft) = self.flat.get(ix) else { continue };
            for nb in &ft.tower.neighbors.nb_lte {
                let ch = nb.radio.get("earfcn").map(|s| s.as_str()).unwrap_or("");
                let pci = nb.radio.get("pci").map(|s| s.as_str()).unwrap_or("");
                if ch.is_empty() || pci.is_empty() {
                    continue;
                }
                if let Some(other) = rf_to_id.get(&(ch.to_string(), pci.to_string())) {
                    if other == &n.id {
                        continue;
                    }
                    let pair = if n.id < *other {
                        (n.id.clone(), other.clone())
                    } else {
                        (other.clone(), n.id.clone())
                    };
                    self.graph_neighbor_pairs.insert(pair);
                }
            }
        }
    }

    fn rebuild_graph_edges_from_accum(&mut self) {
        let id_ix: HashMap<String, usize> = self
            .graph_nodes
            .iter()
            .enumerate()
            .map(|(i, n)| (n.id.clone(), i))
            .collect();
        let mut edges: Vec<GraphEdge> = Vec::new();
        // Hierarchy from current parent links encoded in node homes / rebuild via structure.
        // Recreate hierarchy from operators tree.
        for op in &self.operators {
            let Some(&op_i) = id_ix.get(&op.key) else { continue };
            for site in &op.sites {
                let Some(&site_i) = id_ix.get(&site.key) else { continue };
                edges.push(GraphEdge {
                    a: op_i,
                    b: site_i,
                    kind: EdgeKind::Hierarchy,
                });
                for &flat_i in &site.cells {
                    let Some(ft) = self.flat.get(flat_i) else { continue };
                    let cid = format!("cell:{}", ft.tower.key);
                    let Some(&cell_i) = id_ix.get(&cid) else { continue };
                    edges.push(GraphEdge {
                        a: site_i,
                        b: cell_i,
                        kind: EdgeKind::Hierarchy,
                    });
                }
            }
        }
        for (a, b) in &self.graph_neighbor_pairs {
            let (Some(&ai), Some(&bi)) = (id_ix.get(a), id_ix.get(b)) else {
                continue;
            };
            edges.push(GraphEdge {
                a: ai,
                b: bi,
                kind: EdgeKind::Neighbor,
            });
        }
        self.graph_edges = edges;
    }

    fn layout_graph(&mut self) {
        let mut nodes: Vec<GraphNode> = Vec::new();
        let mut id_ix: HashMap<String, usize> = HashMap::new();

        let push = |nodes: &mut Vec<GraphNode>,
                    id_ix: &mut HashMap<String, usize>,
                    n: GraphNode|
         -> usize {
            let id = n.id.clone();
            if let Some(&i) = id_ix.get(&id) {
                return i;
            }
            let i = nodes.len();
            id_ix.insert(id, i);
            nodes.push(n);
            i
        };

        let n_ops = self.operators.len().max(1) as f32;
        let op_r = 320.0 + 40.0 * n_ops.sqrt();
        for (oi, op) in self.operators.iter().enumerate() {
            let ang = TAU * (oi as f32) / n_ops - std::f32::consts::FRAC_PI_2;
            let op_pos = Vec2::new(ang.cos() * op_r, ang.sin() * op_r);
            let op_i = push(
                &mut nodes,
                &mut id_ix,
                GraphNode {
                    id: op.key.clone(),
                    kind: GraphKind::Operator,
                    label: operator_short_label(&op.plmn),
                    sub: op.plmn.clone(),
                    color: op.color,
                    flat_ix: None,
                    pos: op_pos,
                    vel: Vec2::ZERO,
                    home: Some(op_pos),
                },
            );
            let _ = op_i;

            let n_sites = op.sites.len().max(1) as f32;
            for (si, site) in op.sites.iter().enumerate() {
                let sang = ang + (si as f32 - (n_sites - 1.0) * 0.5) * 0.72;
                let site_home = op_pos + Vec2::new(sang.cos() * 170.0, sang.sin() * 170.0);
                push(
                    &mut nodes,
                    &mut id_ix,
                    GraphNode {
                        id: site.key.clone(),
                        kind: GraphKind::Enb,
                        label: format!("eNB {}", site.enb),
                        sub: site_sub_label(site),
                        color: site_graph_color(&self.flat, site),
                        // eNB is not a cell — never bind flat_ix (stale index → wrong CID label).
                        flat_ix: None,
                        pos: site_home,
                        vel: Vec2::ZERO,
                        home: Some(site_home),
                    },
                );

                let n_cells = site.cells.len().max(1) as f32;
                for (ci, &flat_i) in site.cells.iter().enumerate() {
                    let t = &self.flat[flat_i].tower;
                    let cang = sang + (ci as f32 - (n_cells - 1.0) * 0.5) * 0.55;
                    let cell_pos = site_home + Vec2::new(cang.cos() * 95.0, cang.sin() * 95.0);
                    push(
                        &mut nodes,
                        &mut id_ix,
                        GraphNode {
                            id: format!("cell:{}", t.key),
                            kind: GraphKind::Cell,
                            label: cell_graph_label(t),
                            sub: cell_graph_sub(t),
                            color: cell_graph_color(t, op.color),
                            flat_ix: Some(flat_i),
                            pos: cell_pos,
                            vel: Vec2::ZERO,
                            home: None,
                        },
                    );
                }
            }
        }

        // Preserve positions + velocity for nodes that already existed.
        let old: HashMap<String, (Vec2, Vec2)> = self
            .graph_nodes
            .iter()
            .map(|n| (n.id.clone(), (n.pos, n.vel)))
            .collect();
        for n in &mut nodes {
            if let Some((p, v)) = old.get(&n.id) {
                n.pos = *p;
                n.vel = *v;
            }
        }

        self.graph_nodes = nodes;
        // Drop neighbor pairs whose endpoints vanished (keep the rest — session memory).
        let alive: BTreeSet<String> = self.graph_nodes.iter().map(|n| n.id.clone()).collect();
        self.graph_neighbor_pairs
            .retain(|(a, b)| alive.contains(a) && alive.contains(b));
        self.accumulate_neighbor_pairs();
        self.rebuild_graph_edges_from_accum();
    }

    /// Force-directed step: repulsion + hierarchy springs + soft home anchors.
    fn step_graph_physics(&mut self, dt: f32) {
        let n = self.graph_nodes.len();
        if n == 0 || self.graph_settled {
            return;
        }
        let dt = dt.clamp(0.0, 0.033);
        let drag = self.graph_drag;
        let mut force = vec![Vec2::ZERO; n];

        for i in 0..n {
            for j in (i + 1)..n {
                let delta = self.graph_nodes[j].pos - self.graph_nodes[i].pos;
                let dist2 = delta.length_sq().max(36.0);
                let dist = dist2.sqrt();
                let charge = match (self.graph_nodes[i].kind, self.graph_nodes[j].kind) {
                    (GraphKind::Operator, _) | (_, GraphKind::Operator) => 14000.0,
                    (GraphKind::Enb, GraphKind::Enb) => 9000.0,
                    _ => 5500.0,
                };
                let f = delta / dist * (charge / dist2);
                force[i] -= f;
                force[j] += f;
            }
        }

        // Hierarchy springs only — neighbor edges are visual, not physical (stops edge thrash).
        for e in &self.graph_edges {
            if !matches!(e.kind, EdgeKind::Hierarchy) {
                continue;
            }
            let a = e.a;
            let b = e.b;
            if a >= n || b >= n {
                continue;
            }
            let delta = self.graph_nodes[b].pos - self.graph_nodes[a].pos;
            let dist = delta.length().max(1.0);
            let ideal = match (self.graph_nodes[a].kind, self.graph_nodes[b].kind) {
                (GraphKind::Operator, GraphKind::Enb) | (GraphKind::Enb, GraphKind::Operator) => {
                    160.0
                }
                (GraphKind::Enb, GraphKind::Cell) | (GraphKind::Cell, GraphKind::Enb) => 88.0,
                _ => 110.0,
            };
            let f = delta / dist * ((dist - ideal) * 0.085);
            force[a] += f;
            force[b] -= f;
        }

        for (i, node) in self.graph_nodes.iter().enumerate() {
            if let Some(home) = node.home {
                let k = match node.kind {
                    GraphKind::Operator => 0.04,
                    GraphKind::Enb => 0.025,
                    _ => 0.0,
                };
                if k > 0.0 {
                    force[i] += (home - node.pos) * k;
                }
            }
        }

        let mut max_speed = 0.0f32;
        for i in 0..n {
            if Some(i) == drag {
                self.graph_nodes[i].vel = Vec2::ZERO;
                continue;
            }
            let mut v = self.graph_nodes[i].vel + force[i] * dt * 60.0;
            v *= 0.78;
            let speed = v.length();
            if speed > 420.0 {
                v *= 420.0 / speed;
            }
            if speed < 0.35 && force[i].length() < 1.5 {
                v = Vec2::ZERO;
            }
            max_speed = max_speed.max(v.length());
            self.graph_nodes[i].vel = v;
            self.graph_nodes[i].pos += v * dt;
        }

        // Freeze when calm (unless user is dragging a node).
        if drag.is_none() && max_speed < 0.8 {
            for n in &mut self.graph_nodes {
                n.vel = Vec2::ZERO;
            }
            self.graph_settled = true;
        }
    }
}


fn parse_f32(s: &str) -> f32 {
    s.parse().unwrap_or(-999.0)
}

fn fill_label(th: &Theme, t: &Tower) -> (&'static str, Color32) {
    let has_cid = !t.get_id("cid").is_empty();
    let has_tac = !t.lac_or_tac().is_empty();
    let has_plmn = !t.plmn().is_empty();
    let has_rf = !t.channel().is_empty() && !t.cell_code().is_empty();
    if has_rf && has_plmn && has_cid && has_tac {
        ("FULL", th.ok)
    } else if has_rf && has_plmn {
        ("PLMN", th.warning)
    } else if has_rf {
        ("RADIO", th.muted)
    } else {
        ("WEAK", th.wash(th.muted, 160))
    }
}

fn rsrp_color(th: &Theme, rsrp: f32) -> Color32 {
    if rsrp > -85.0 {
        th.serving
    } else if rsrp > -105.0 {
        th.accent
    } else if rsrp > -120.0 {
        th.accent2
    } else {
        th.muted
    }
}

fn signal_meter(ui: &mut Ui, th: &Theme, rsrp: f32, width: f32, height: f32) {
    let norm = ((rsrp + 140.0) / 80.0).clamp(0.0, 1.0);
    let (rect, _) = ui.allocate_exact_size(Vec2::new(width, height), Sense::hover());
    ui.painter()
        .rect_filled(rect, CornerRadius::same(height as u8 / 2), Color32::from_rgb(30, 36, 46));
    if rsrp > -200.0 {
        let fill = egui::Rect::from_min_size(rect.min, Vec2::new(rect.width() * norm, rect.height()));
        ui.painter()
            .rect_filled(fill, CornerRadius::same(height as u8 / 2), rsrp_color(th, rsrp));
    }
}

fn badge(ui: &mut Ui, label: &str, fg: Color32) {
    badge_icon(ui, "", label, fg);
}

fn badge_icon(ui: &mut Ui, icon: &str, label: &str, fg: Color32) {
    let bg = Color32::from_rgba_unmultiplied(fg.r(), fg.g(), fg.b(), 28);
    let text = if icon.is_empty() {
        label.to_string()
    } else {
        format!("{icon}  {label}")
    };
    Frame::new()
        .fill(bg)
        .stroke(Stroke::new(1.0, Color32::from_rgba_unmultiplied(fg.r(), fg.g(), fg.b(), 80)))
        .corner_radius(CornerRadius::same(6))
        .inner_margin(Margin::symmetric(8, 3))
        .show(ui, |ui| {
            ui.label(RichText::new(text).strong().size(11.5).color(fg));
        });
}

fn operator_dot(ui: &mut Ui, color: Color32, radius: f32) {
    let (rect, _) = ui.allocate_exact_size(Vec2::splat(radius * 2.0 + 2.0), Sense::hover());
    let center = rect.center();
    // Soft halo + solid brand circle.
    ui.painter().circle_filled(
        center,
        radius + 2.0,
        Color32::from_rgba_unmultiplied(color.r(), color.g(), color.b(), 50),
    );
    ui.painter().circle_filled(center, radius, color);
    ui.painter().circle_stroke(
        center,
        radius,
        Stroke::new(1.0, Color32::from_rgba_unmultiplied(255, 255, 255, 40)),
    );
}

fn metric_card(ui: &mut Ui, th: &Theme, label: &str, value: &str, accent: Color32) {
    Frame::new()
        .fill(th.panel)
        .stroke(Stroke::new(1.0, th.stroke))
        .corner_radius(CornerRadius::same(12))
        .inner_margin(Margin::symmetric(16, 14))
        .show(ui, |ui| {
            ui.set_min_width(108.0);
            ui.vertical(|ui| {
                ui.label(RichText::new(label).size(12.0).color(th.muted));
                ui.add_space(4.0);
                ui.label(RichText::new(value).strong().size(28.0).color(accent));
            });
        });
}

fn section_label(ui: &mut Ui, th: &Theme, text: &str) {
    ui.add_space(10.0);
    ui.label(RichText::new(text).strong().size(13.0).color(th.muted));
    ui.add_space(8.0);
}

fn toggle_expand(expanded: &mut BTreeSet<String>, key: &str) {
    if expanded.contains(key) {
        expanded.remove(key);
    } else {
        expanded.insert(key.to_string());
    }
}

pub fn render_live(ui: &mut Ui, th: &Theme, live: &mut LiveState) {
    // Global shortcuts (skip while typing in a text field).
    if !ui.ctx().wants_keyboard_input() {
        let (tab, find, next, prev) = ui.input(|i| {
            (
                i.key_pressed(Key::Tab),
                (i.key_pressed(Key::F) && i.modifiers.command) || i.key_pressed(Key::Slash),
                i.key_pressed(Key::F3) && !i.modifiers.shift,
                (i.key_pressed(Key::F3) && i.modifiers.shift)
                    || (i.key_pressed(Key::G) && i.modifiers.command && i.modifiers.shift),
            )
        });
        if tab {
            live.view = live.view.cycle();
            if live.view == LiveView::Graph {
                live.rebuild_graph_if_needed();
                if live.selected.is_some() {
                    live.focus_graph_selected = true;
                }
            }
        }
        if find {
            live.focus_search = true;
        }
        if next {
            live.jump_search_cursor(1);
        }
        if prev {
            live.jump_search_cursor(-1);
        }
    }

    // ── Header ──
    ui.horizontal(|ui| {
        ui.vertical(|ui| {
            ui.horizontal(|ui| {
                ui.label(
                    RichText::new(icons::BROADCAST)
                        .size(22.0)
                        .color(th.accent),
                );
                ui.add_space(8.0);
                ui.label(RichText::new("Live Scan").strong().size(28.0).color(th.text));
            });
            ui.label(
                RichText::new(format!(
                    "{}  ·  Ctrl+F search  ·  Tab cycles views",
                    live.view.hint()
                ))
                .size(14.0)
                .color(th.muted),
            );
        });
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            let live_col = if live.watching && live.last_err.is_empty() {
                th.serving
            } else if live.watching {
                th.accent2
            } else {
                th.muted
            };
            let (live_icon, live_txt) = if !live.watching {
                (icons::PAUSE, "Paused")
            } else if live.last_err.is_empty() {
                (icons::PLAY, "Live")
            } else {
                (icons::CIRCLE, "Waiting")
            };
            if ui
                .add(
                    egui::Button::new(
                        RichText::new(format!("{live_icon}  {live_txt}"))
                            .strong()
                            .size(14.0)
                            .color(live_col),
                    )
                    .fill(th.wash(live_col, 28))
                    .corner_radius(CornerRadius::same(20))
                    .min_size(Vec2::new(110.0, 36.0)),
                )
                .on_hover_text("Toggle polling")
                .clicked()
            {
                live.watching = !live.watching;
            }
            ui.add_space(8.0);
            if ui
                .add(
                    egui::Button::new(
                        RichText::new(format!("{}  Reload", icons::ARROW_RIGHT))
                            .size(14.0)
                            .color(th.text),
                    )
                    .fill(th.panel2)
                    .corner_radius(CornerRadius::same(8))
                    .min_size(Vec2::new(96.0, 36.0)),
                )
                .clicked()
            {
                live.last_mtime = None;
                live.poll();
            }
            if ui
                .add(
                    egui::Button::new(
                        RichText::new(format!("{}  Pick file", icons::FOLDER_OPEN))
                            .size(14.0)
                            .color(th.text),
                    )
                    .fill(th.panel2)
                    .corner_radius(CornerRadius::same(8))
                    .min_size(Vec2::new(120.0, 36.0)),
                )
                .clicked()
            {
                if let Some(p) = rfd::FileDialog::new()
                    .add_filter("json", &["json"])
                    .pick_file()
                {
                    live.path = p;
                    live.last_mtime = None;
                    live.poll();
                }
            }
        });
    });

    ui.add_space(6.0);
    ui.horizontal(|ui| {
        ui.label(
            RichText::new(icons::FILE_TEXT)
                .size(14.0)
                .color(th.muted),
        );
        ui.label(RichText::new("Feed").size(13.0).color(th.muted));
        let mut path_str = live.path.to_string_lossy().to_string();
        let te = egui::TextEdit::singleline(&mut path_str)
            .desired_width(420.0)
            .font(egui::TextStyle::Monospace)
            .margin(Margin::symmetric(10, 8));
        ui.add(te);
        if path_str != live.path.to_string_lossy() {
            live.path = PathBuf::from(path_str);
            live.last_mtime = None;
        }
    });

    ui.add_space(14.0);

    // ── Metrics strip ──
    if let Some(doc) = &live.doc {
        let m = &doc.meta;
        let ops_n = live.operators.len().to_string();
        let sites_n = live
            .operators
            .iter()
            .map(|o| o.sites.len())
            .sum::<usize>()
            .to_string();
        let rf_n = live.flat.len().to_string();
        let full = live
            .flat
            .iter()
            .filter(|ft| ft.tower.has_cid())
            .count()
            .to_string();
        let radio_n = live
            .flat
            .iter()
            .filter(|ft| !ft.tower.has_cid() && ft.rat == Rat::Lte)
            .count()
            .to_string();
        let camped = live
            .flat
            .iter()
            .filter(|ft| ft.tower.was_identity_camped())
            .count()
            .to_string();
        let serving = live
            .flat
            .iter()
            .filter(|ft| ft.tower.is_serving())
            .count()
            .to_string();
        let hf = if m.hop_fulls.is_empty() {
            "-".into()
        } else {
            m.hop_fulls.clone()
        };
        let hc = if m.hop_cops.is_empty() {
            "-".into()
        } else {
            m.hop_cops.clone()
        };

        ui.horizontal(|ui| {
            metric_card(ui, th, "Operators", &ops_n, th.accent);
            ui.add_space(10.0);
            metric_card(ui, th, "Sites", &sites_n, th.lte);
            ui.add_space(10.0);
            metric_card(ui, th, "RF cells", &rf_n, th.text);
            ui.add_space(10.0);
            metric_card(ui, th, "FULL", &full, th.serving);
            ui.add_space(10.0);
            metric_card(ui, th, "Heard RF", &radio_n, th.muted);
            ui.add_space(10.0);
            metric_card(ui, th, "Serving", &serving, th.serving);
            ui.add_space(10.0);
            metric_card(ui, th, "Camped", &camped, th.warning);
            ui.add_space(10.0);
            metric_card(ui, th, "Hop FULLs", &hf, th.wcdma);
            ui.add_space(10.0);
            metric_card(ui, th, "COPS", &hc, th.accent2);
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                if !m.situation_as_of.is_empty() {
                    ui.label(
                        RichText::new(format!("Updated  {}", m.situation_as_of))
                            .size(13.0)
                            .color(th.muted),
                    );
                }
            });
        });
        let qmi_bits: Vec<String> = [
            (!m.qmi_reg.is_empty()).then(|| format!("reg {}", m.qmi_reg)),
            (!m.qmi_ps.is_empty()).then(|| format!("ps {}", m.qmi_ps)),
            (!m.qmi_radio.is_empty()).then(|| format!("rat {}", m.qmi_radio)),
            (!m.qmi_plmn.is_empty()).then(|| {
                if m.qmi_plmn_name.is_empty() {
                    format!("plmn {}", m.qmi_plmn)
                } else {
                    format!("plmn {} ({})", m.qmi_plmn, m.qmi_plmn_name)
                }
            }),
            (!m.qmi_rsrp.is_empty()).then(|| format!("rsrp {} dBm", m.qmi_rsrp)),
            (!m.qmi_rsrq.is_empty()).then(|| format!("rsrq {} dB", m.qmi_rsrq)),
            (!m.qmi_snr.is_empty()).then(|| format!("snr {} dB", m.qmi_snr)),
            (!m.qmi_hop_snaps.is_empty()).then(|| format!("hop-qmi {}", m.qmi_hop_snaps)),
        ]
        .into_iter()
        .flatten()
        .collect();
        if !qmi_bits.is_empty() {
            ui.add_space(6.0);
            ui.label(
                RichText::new(format!("QMI  {}", qmi_bits.join(" · ")))
                    .size(12.5)
                    .color(th.muted),
            );
        }
        ui.add_space(12.0);
    } else {
        ui.add_space(8.0);
    }

    if !live.last_err.is_empty() {
        Frame::new()
            .fill(Color32::from_rgb(42, 32, 22))
            .stroke(Stroke::new(1.0, Color32::from_rgb(180, 120, 60)))
            .corner_radius(CornerRadius::same(12))
            .inner_margin(Margin::symmetric(20, 16))
            .show(ui, |ui| {
                ui.label(
                    RichText::new("Waiting for scanner feed")
                        .strong()
                        .size(18.0)
                        .color(th.accent2),
                );
                ui.add_space(6.0);
                ui.label(RichText::new(&live.last_err).size(14.0).color(th.muted));
                ui.add_space(10.0);
                ui.label(
                    RichText::new("./build/live_scanner --earfcn-hop --duration 300")
                        .size(14.0)
                        .monospace()
                        .color(th.text),
                );
            });
        ui.add_space(12.0);
    }

    // ── Workspace: shared tools + view body + inspector ──
    let avail = ui.available_size();
    let detail_w = 380.0_f32.clamp(avail.x * 0.30, avail.x * 0.42);

    ui.horizontal(|ui| {
        ui.allocate_ui_with_layout(
            Vec2::new((avail.x - detail_w - 12.0).max(320.0), avail.y),
            Layout::top_down(Align::Min),
            |ui| {
                Frame::new()
                    .fill(th.panel)
                    .stroke(Stroke::new(1.0, th.stroke))
                    .corner_radius(CornerRadius::same(14))
                    .inner_margin(Margin::symmetric(12, 12))
                    .show(ui, |ui| {
                        ui.set_min_height(ui.available_height());
                        render_workspace_toolbar(ui, th, live);
                        ui.add_space(10.0);
                        Frame::new()
                            .fill(th.bg)
                            .stroke(Stroke::new(1.0, th.stroke))
                            .corner_radius(CornerRadius::same(10))
                            .inner_margin(Margin::symmetric(8, 8))
                            .show(ui, |ui| {
                                ui.set_min_height((ui.available_height() - 4.0).max(200.0));
                                match live.view {
                                    LiveView::Table => render_table_view(ui, th, live),
                                    LiveView::Graph => render_graph_view(ui, th, live),
                                    LiveView::Tree => render_tree_view(ui, th, live),
                                }
                            });
                    });
            },
        );

        ui.add_space(8.0);

        ui.allocate_ui_with_layout(
            Vec2::new(detail_w, avail.y),
            Layout::top_down(Align::Min),
            |ui| {
                Frame::new()
                    .fill(th.panel)
                    .stroke(Stroke::new(1.0, th.stroke))
                    .corner_radius(CornerRadius::same(14))
                    .inner_margin(Margin::symmetric(20, 18))
                    .show(ui, |ui| {
                        ui.set_min_height(ui.available_height());
                        ui.label(
                            RichText::new("Inspector")
                                .strong()
                                .size(12.0)
                                .color(th.muted),
                        );
                        ui.add_space(8.0);
                        if let Some(ix) = live.selected {
                            ui.horizontal(|ui| {
                                for (lab, tip, go) in [
                                    (
                                        "Table",
                                        "Show this row in the flat RF table",
                                        LiveView::Table,
                                    ),
                                    (
                                        "List",
                                        "Expand tree path and scroll to this carrier",
                                        LiveView::Tree,
                                    ),
                                    (
                                        "Graph",
                                        "Open Graph and center on this cell / eNB",
                                        LiveView::Graph,
                                    ),
                                ] {
                                    let active = live.view == go;
                                    let btn = ui
                                        .add(
                                            egui::Button::new(
                                                RichText::new(lab)
                                                    .size(12.5)
                                                    .color(if active { th.accent } else { th.text }),
                                            )
                                            .fill(if active {
                                                th.wash(th.accent, 32)
                                            } else {
                                                th.panel2
                                            })
                                            .corner_radius(CornerRadius::same(8))
                                            .min_size(Vec2::new(0.0, 30.0)),
                                        )
                                        .on_hover_text(tip);
                                    if btn.clicked() {
                                        match go {
                                            LiveView::Table => live.reveal_in_table(ix),
                                            LiveView::Tree => live.reveal_in_tree(ix),
                                            LiveView::Graph => live.reveal_in_graph(ix),
                                        }
                                    }
                                    ui.add_space(4.0);
                                }
                            });
                            ui.add_space(8.0);
                            ui.separator();
                            ui.add_space(4.0);
                            ScrollArea::vertical()
                                .id_salt("live_inspector_scroll")
                                .auto_shrink([false, false])
                                .show(ui, |ui| {
                                    if let Some(ft) = live.flat.get(ix).cloned() {
                                        let ext = live.ext_status(ix);
                                        render_detail(ui, th, &ft, &ext);
                                    }
                                    ui.add_space(24.0);
                                });
                        } else {
                            ui.add_space(40.0);
                            ui.label(
                                RichText::new("Select a carrier\nor neighbor")
                                    .size(16.0)
                                    .color(th.muted),
                            );
                            ui.add_space(8.0);
                            ui.label(
                                RichText::new(match live.view {
                                    LiveView::Table => {
                                        "Click any RF row in the table — all towers stay visible here."
                                    }
                                    LiveView::Tree => {
                                        "Click any row in the list to inspect identity, signal, and neighbor hints."
                                    }
                                    LiveView::Graph => {
                                        "Drag the canvas · scroll to zoom · click a node. Tab cycles views."
                                    }
                                })
                                .size(13.0)
                                .color(Color32::from_rgb(100, 112, 128)),
                            );
                        }
                    });
            },
        );
    });
}

fn select_graph_node(live: &mut LiveState, node_i: usize) {
    let Some(n) = live.graph_nodes.get(node_i) else {
        return;
    };
    match n.kind {
        GraphKind::Cell => {
            if let Some(ix) = n.flat_ix {
                live.selected = Some(ix);
            }
        }
        GraphKind::Enb => {
            let id = n.id.clone();
            live.selected = live.site_primary_flat_ix(&id);
        }
        GraphKind::Operator | GraphKind::Heard => {}
    }
}

fn render_workspace_toolbar(ui: &mut Ui, th: &Theme, live: &mut LiveState) {
    ui.horizontal(|ui| {
        render_view_switcher(ui, th, live);
        ui.add_space(12.0);
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            ui.label(
                RichText::new(live.enrich.status_line())
                    .size(11.5)
                    .color(if live.enrich.has_key() {
                        th.muted
                    } else {
                        th.wash(th.muted, 160)
                    }),
            );
        });
    });
    ui.add_space(8.0);
    render_search_field(ui, th, live);
}

fn render_view_switcher(ui: &mut Ui, th: &Theme, live: &mut LiveState) {
    let views = [LiveView::Table, LiveView::Tree, LiveView::Graph];
    Frame::new()
        .fill(th.bg)
        .stroke(Stroke::new(1.0, th.stroke))
        .corner_radius(CornerRadius::same(10))
        .inner_margin(Margin::same(3))
        .show(ui, |ui| {
            ui.horizontal(|ui| {
                ui.spacing_mut().item_spacing.x = 2.0;
                for v in views {
                    let active = live.view == v;
                    let fill = if active {
                        th.wash(th.accent, 40)
                    } else {
                        Color32::TRANSPARENT
                    };
                    let col = if active { th.accent } else { th.muted };
                    let btn = ui
                        .add(
                            egui::Button::new(
                                RichText::new(format!("{}  {}", v.icon(), v.label()))
                                    .strong()
                                    .size(13.0)
                                    .color(col),
                            )
                            .fill(fill)
                            .stroke(if active {
                                Stroke::new(1.0, th.wash(th.accent, 100))
                            } else {
                                Stroke::NONE
                            })
                            .corner_radius(CornerRadius::same(8))
                            .min_size(Vec2::new(92.0, 32.0)),
                        )
                        .on_hover_text(format!("{} (Tab)", v.hint()));
                    if btn.clicked() && live.view != v {
                        live.view = v;
                        if v == LiveView::Graph {
                            live.rebuild_graph_if_needed();
                            if live.selected.is_some() {
                                live.focus_graph_selected = true;
                            }
                        }
                    }
                }
            });
        });
}

/// Compact search pill — icon + field + match chip + prev/next.
fn render_search_field(ui: &mut Ui, th: &Theme, live: &mut LiveState) {
    let matches = live.search_matches();
    if live.search_cursor >= matches.len() && !matches.is_empty() {
        live.search_cursor = 0;
    }

    Frame::new()
        .fill(th.bg)
        .stroke(Stroke::new(
            1.0,
            if live.search_active() {
                th.wash(th.accent, 120)
            } else {
                th.stroke
            },
        ))
        .corner_radius(CornerRadius::same(10))
        .inner_margin(Margin::symmetric(10, 6))
        .show(ui, |ui| {
            ui.horizontal(|ui| {
                ui.label(
                    RichText::new(icons::MAGNIFYING_GLASS)
                        .size(16.0)
                        .color(if live.search_active() {
                            th.accent
                        } else {
                            th.muted
                        }),
                );
                ui.add_space(6.0);
                let avail = (ui.available_width() - 168.0).clamp(180.0, 520.0);
                let search_id = egui::Id::new("live_tower_search");
                let te = egui::TextEdit::singleline(&mut live.search_query)
                    .id(search_id)
                    .desired_width(avail)
                    .frame(false)
                    .hint_text("CID · eNB · EARFCN · PCI · MegaFon · 250-02…")
                    .font(egui::TextStyle::Monospace);
                let resp = ui.add(te);
                if live.focus_search {
                    resp.request_focus();
                    live.focus_search = false;
                }
                if resp.changed() {
                    live.search_cursor = 0;
                }
                if resp.has_focus() && ui.input(|i| i.key_pressed(Key::Enter)) {
                    live.jump_search_cursor(1);
                }

                if live.search_active() {
                    if ui
                        .add(
                            egui::Button::new(
                                RichText::new(icons::X_CIRCLE).size(14.0).color(th.muted),
                            )
                            .fill(Color32::TRANSPARENT)
                            .min_size(Vec2::splat(26.0)),
                        )
                        .on_hover_text("Clear search")
                        .clicked()
                    {
                        live.search_query.clear();
                        live.search_cursor = 0;
                    }
                }

                let chip = if !live.search_active() {
                    format!("{} RF", live.flat.len())
                } else if matches.is_empty() {
                    "0".into()
                } else {
                    format!(
                        "{}/{}",
                        live.search_cursor.min(matches.len() - 1) + 1,
                        matches.len()
                    )
                };
                Frame::new()
                    .fill(th.wash(th.accent, if live.search_active() { 36 } else { 18 }))
                    .corner_radius(CornerRadius::same(6))
                    .inner_margin(Margin::symmetric(8, 4))
                    .show(ui, |ui| {
                        ui.label(
                            RichText::new(chip)
                                .size(12.0)
                                .strong()
                                .color(if live.search_active() {
                                    th.accent
                                } else {
                                    th.muted
                                }),
                        );
                    });

                ui.add_space(4.0);
                let prev = ui
                    .add_enabled(
                        !matches.is_empty(),
                        egui::Button::new(
                            RichText::new(icons::ARROW_LEFT)
                                .size(14.0)
                                .color(th.text),
                        )
                        .fill(th.panel2)
                        .corner_radius(CornerRadius::same(6))
                        .min_size(Vec2::new(30.0, 28.0)),
                    )
                    .on_hover_text("Previous match (Shift+F3)");
                if prev.clicked() {
                    live.jump_search_cursor(-1);
                }
                let next = ui
                    .add_enabled(
                        !matches.is_empty(),
                        egui::Button::new(
                            RichText::new(icons::ARROW_RIGHT)
                                .size(14.0)
                                .color(th.text),
                        )
                        .fill(th.panel2)
                        .corner_radius(CornerRadius::same(6))
                        .min_size(Vec2::new(30.0, 28.0)),
                    )
                    .on_hover_text("Next match (F3 / Enter)");
                if next.clicked() {
                    live.jump_search_cursor(1);
                }
            });
        });
}

fn render_tree_view(ui: &mut Ui, th: &Theme, live: &mut LiveState) {
    ScrollArea::vertical()
        .id_salt("live_tree_scroll")
        .auto_shrink([false, false])
        .drag_to_scroll(false)
        .show(ui, |ui| {
            if live.operators.is_empty()
                && live.unknown_lte.is_empty()
                && live.other_rat.is_empty()
                && live.last_err.is_empty()
            {
                empty_state(ui, th);
                return;
            }

            let hits = live.match_set();
            let filtering = live.search_active();

            for op in live.operators.clone() {
                if filtering && !op_has_match(&op, &hits) {
                    continue;
                }
                render_operator(ui, th, live, &op, &hits, filtering);
                ui.add_space(12.0);
            }

            let unk: Vec<usize> = live
                .unknown_lte
                .iter()
                .copied()
                .filter(|ix| !filtering || hits.contains(ix))
                .collect();
            if !unk.is_empty() {
                section_label(ui, th, "LTE - NO PLMN YET");
                for ix in unk {
                    render_cell_row(ui, th, live, ix, 0, CellKind::Heard);
                    ui.add_space(6.0);
                }
            }

            let other: Vec<usize> = live
                .other_rat
                .iter()
                .copied()
                .filter(|ix| !filtering || hits.contains(ix))
                .collect();
            if !other.is_empty() {
                section_label(ui, th, "OTHER RAT");
                for ix in other {
                    render_cell_row(ui, th, live, ix, 0, CellKind::Heard);
                    ui.add_space(6.0);
                }
            }

            if filtering && hits.is_empty() {
                ui.add_space(24.0);
                ui.label(
                    RichText::new("No towers match this search")
                        .size(16.0)
                        .color(th.muted),
                );
            }
            ui.add_space(24.0);
        });
}

fn render_table_view(ui: &mut Ui, th: &Theme, live: &mut LiveState) {
    let filtering = live.search_active();
    let hits = live.match_set();
    let rows: Vec<usize> = if filtering {
        hits.iter().copied().collect()
    } else {
        (0..live.flat.len()).collect()
    };

    ui.horizontal(|ui| {
        ui.label(
            RichText::new(format!(
                "{}  {} towers",
                icons::TABLE,
                rows.len()
            ))
            .strong()
            .size(13.0)
            .color(th.text),
        );
        ui.label(
            RichText::new(if filtering {
                "filtered · every RF row when search is clear"
            } else {
                "FULL + Heard RF · feed order"
            })
            .size(12.0)
            .color(th.muted),
        );
    });
    ui.add_space(6.0);

    if rows.is_empty() {
        ui.add_space(40.0);
        ui.vertical_centered(|ui| {
            ui.label(
                RichText::new(if filtering {
                    "No towers match this search"
                } else {
                    "No RF cells in feed yet"
                })
                .size(16.0)
                .color(th.muted),
            );
        });
        return;
    }

    let height = (ui.available_height() - 4.0).max(120.0);
    let mut clicked: Option<usize> = None;
    let scroll_row = if live.scroll_to_selected {
        live.selected
            .and_then(|sel| rows.iter().position(|&ix| ix == sel))
    } else {
        None
    };

    let mut table = TableBuilder::new(ui)
        .striped(true)
        .sense(Sense::click())
        .cell_layout(Layout::left_to_right(Align::Center))
        .min_scrolled_height(height)
        .max_scroll_height(height)
        .column(Column::exact(72.0))  // status
        .column(Column::exact(52.0))  // RAT
        .column(Column::exact(110.0)) // operator
        .column(Column::exact(120.0)) // EARFCN/PCI
        .column(Column::remainder().at_least(140.0)) // identity
        .column(Column::exact(72.0))  // band
        .column(Column::exact(118.0)) // signal
        .column(Column::exact(56.0)); // OCI

    if let Some(r) = scroll_row {
        table = table.scroll_to_row(r, Some(Align::Center));
        live.scroll_to_selected = false;
    }

    table
        .header(26.0, |mut h| {
            for lab in [
                "STATUS",
                "RAT",
                "OPERATOR",
                "EARFCN / PCI",
                "IDENTITY",
                "BAND",
                "SIGNAL",
                "OCI",
            ] {
                h.col(|ui| {
                    ui.label(RichText::new(lab).strong().size(10.5).color(th.muted));
                });
            }
        })
        .body(|body| {
            body.rows(38.0, rows.len(), |mut row| {
                let ix = rows[row.index()];
                let Some(ft) = live.flat.get(ix) else {
                    return;
                };
                let t = &ft.tower;
                let selected = live.selected == Some(ix);
                if selected {
                    row.set_selected(true);
                }
                let (fill, fill_c) = fill_label(th, t);
                let rsrp = parse_f32(t.rxl());
                let serving = t.is_serving();
                let camped = t.was_identity_camped() && !serving;
                let plmn = normalize_plmn(t.plmn());
                let (brand, brand_c) = operator_brand(&plmn);
                let ext = live.ext_status_ref(ix);

                row.col(|ui| {
                    table_status_cell(ui, th, serving, camped, fill, fill_c);
                });
                row.col(|ui| {
                    Frame::new()
                        .fill(th.wash(th.rat(ft.rat), 28))
                        .corner_radius(CornerRadius::same(5))
                        .inner_margin(Margin::symmetric(6, 3))
                        .show(ui, |ui| {
                            ui.label(
                                RichText::new(ft.rat.as_str())
                                    .strong()
                                    .size(11.5)
                                    .color(th.rat(ft.rat)),
                            );
                        });
                });
                row.col(|ui| {
                    ui.horizontal(|ui| {
                        if !plmn.is_empty() {
                            operator_dot(ui, brand_c, 5.0);
                            ui.add_space(4.0);
                        }
                        ui.label(
                            RichText::new(if brand.is_empty() {
                                if plmn.is_empty() {
                                    "—"
                                } else {
                                    plmn.as_str()
                                }
                            } else {
                                brand
                            })
                            .size(12.5)
                            .color(th.text),
                        );
                    });
                });
                row.col(|ui| {
                    ui.vertical(|ui| {
                        ui.spacing_mut().item_spacing.y = 0.0;
                        ui.label(
                            RichText::new(dash(t.channel()))
                                .strong()
                                .size(13.0)
                                .monospace()
                                .color(th.text),
                        );
                        ui.label(
                            RichText::new(format!("PCI {}", dash(t.cell_code())))
                                .size(11.0)
                                .color(th.muted),
                        );
                    });
                });
                row.col(|ui| {
                    if t.get_id("cid").is_empty() {
                        ui.label(
                            RichText::new("PCI only · no CID")
                                .size(12.5)
                                .color(th.muted),
                        );
                    } else {
                        ui.vertical(|ui| {
                            ui.spacing_mut().item_spacing.y = 0.0;
                            ui.label(
                                RichText::new(format!("CID {}", t.get_id("cid")))
                                    .strong()
                                    .size(13.0)
                                    .color(th.text),
                            );
                            let enb = t.get_id("enb_id");
                            if !enb.is_empty() {
                                ui.label(
                                    RichText::new(format!("eNB {enb}"))
                                        .size(11.0)
                                        .color(th.muted),
                                );
                            }
                        });
                    }
                });
                row.col(|ui| {
                    ui.label(
                        RichText::new(if t.band().is_empty() {
                            "—"
                        } else {
                            t.band()
                        })
                        .size(12.5)
                        .color(th.text),
                    );
                });
                row.col(|ui| {
                    ui.horizontal(|ui| {
                        if rsrp > -200.0 {
                            ui.label(
                                RichText::new(format!("{rsrp:.0}"))
                                    .strong()
                                    .size(13.0)
                                    .color(rsrp_color(th, rsrp)),
                            );
                            ui.add_space(4.0);
                            signal_meter(ui, th, rsrp, 56.0, 7.0);
                        } else {
                            ui.label(RichText::new("—").size(13.0).color(th.muted));
                        }
                    });
                });
                row.col(|ui| {
                    table_oci_cell(ui, th, &ext);
                });

                if row.response().clicked() {
                    clicked = Some(ix);
                }
            });
        });

    if let Some(ix) = clicked {
        live.selected = Some(ix);
        live.sync_search_cursor(ix);
    }
}

fn table_status_cell(
    ui: &mut Ui,
    th: &Theme,
    serving: bool,
    camped: bool,
    fill: &str,
    fill_c: Color32,
) {
    ui.horizontal(|ui| {
        let (icon, c) = if serving {
            (icons::CELL_SIGNAL_FULL, th.serving)
        } else if camped {
            (icons::MAP_PIN, th.warning)
        } else {
            (icons::CIRCLE, th.muted)
        };
        ui.label(RichText::new(icon).size(13.0).color(c));
        badge(ui, fill, fill_c);
    });
}

fn table_oci_cell(ui: &mut Ui, th: &Theme, ext: &ExtStatus) {
    let (glyph, c) = match ext {
        ExtStatus::NotInDb { .. } => (icons::X_CIRCLE, th.danger),
        ExtStatus::Found { .. } if ext.has_field_mismatch() => (icons::WARNING, th.warning),
        ExtStatus::Found { .. } => (icons::CHECK_CIRCLE, th.ok),
        ExtStatus::Pending => (icons::CIRCLE, th.muted),
        ExtStatus::Error(_) => (icons::WARNING_CIRCLE, th.warning),
        ExtStatus::NoKey | ExtStatus::Skipped => ("", th.muted),
    };
    if glyph.is_empty() {
        ui.label(RichText::new("·").size(12.0).color(th.muted));
    } else {
        ui.label(RichText::new(glyph).size(14.0).color(c));
    }
}

fn op_has_match(op: &OperatorNode, hits: &BTreeSet<usize>) -> bool {
    op.sites.iter().any(|s| site_has_match(s, hits))
        || op.incomplete.iter().any(|ix| hits.contains(ix))
}

fn site_has_match(site: &SiteNode, hits: &BTreeSet<usize>) -> bool {
    site.cells.iter().any(|ix| hits.contains(ix))
}

/// Soft hint: which known eNBs already have FULL cells on this EARFCN (not ownership).
fn earfcn_peer_enbs(live: &LiveState, op: &OperatorNode, ix: usize) -> Vec<String> {
    let Some(ft) = live.flat.get(ix) else {
        return Vec::new();
    };
    let earfcn = ft.tower.channel();
    if earfcn.is_empty() {
        return Vec::new();
    }
    let mut out = Vec::new();
    for site in &op.sites {
        let owns = site.cells.iter().any(|&ci| {
            live.flat
                .get(ci)
                .map(|f| f.tower.channel() == earfcn)
                .unwrap_or(false)
        });
        if owns {
            out.push(site.enb.clone());
        }
    }
    out
}

fn graph_node_matches(live: &LiveState, n: &GraphNode, hits: &BTreeSet<usize>) -> bool {
    if hits.is_empty() {
        return false;
    }
    match n.kind {
        GraphKind::Cell => n.flat_ix.map(|ix| hits.contains(&ix)).unwrap_or(false),
        GraphKind::Enb => live
            .operators
            .iter()
            .flat_map(|op| op.sites.iter())
            .find(|s| s.key == n.id)
            .map(|s| site_has_match(s, hits))
            .unwrap_or(false),
        GraphKind::Operator => live
            .operators
            .iter()
            .find(|op| op.key == n.id)
            .map(|op| op_has_match(op, hits))
            .unwrap_or(false),
        GraphKind::Heard => false,
    }
}

fn graph_node_oci_unmatched(live: &LiveState, n: &GraphNode) -> bool {
    match n.kind {
        GraphKind::Cell => n
            .flat_ix
            .map(|ix| live.ext_status_ref(ix).is_unmatched())
            .unwrap_or(false),
        GraphKind::Enb => live
            .operators
            .iter()
            .flat_map(|op| op.sites.iter())
            .find(|s| s.key == n.id)
            .map(|s| live.site_unmatched(s))
            .unwrap_or(false),
        GraphKind::Operator => live
            .operators
            .iter()
            .find(|op| op.key == n.id)
            .map(|op| op.sites.iter().any(|s| live.site_unmatched(s)))
            .unwrap_or(false),
        GraphKind::Heard => false,
    }
}

fn graph_node_oci_warn(live: &LiveState, n: &GraphNode) -> bool {
    match n.kind {
        GraphKind::Cell => n
            .flat_ix
            .map(|ix| live.ext_status_ref(ix).has_field_mismatch())
            .unwrap_or(false),
        GraphKind::Enb => live
            .operators
            .iter()
            .flat_map(|op| op.sites.iter())
            .find(|s| s.key == n.id)
            .map(|s| {
                !live.site_unmatched(s)
                    && s.cells
                        .iter()
                        .any(|&ix| live.ext_status_ref(ix).has_field_mismatch())
            })
            .unwrap_or(false),
        _ => false,
    }
}

fn render_graph_view(ui: &mut Ui, th: &Theme, live: &mut LiveState) {
    live.rebuild_graph_if_needed();

    Frame::new()
        .fill(Color32::from_rgb(14, 17, 23))
        .stroke(Stroke::new(1.0, th.stroke))
        .corner_radius(CornerRadius::same(14))
        .inner_margin(Margin::same(0))
        .show(ui, |ui| {
            let (resp, painter) =
                ui.allocate_painter(ui.available_size(), Sense::click_and_drag());
            let rect = resp.rect;
            live.apply_graph_focus(rect);
            let origin = rect.center() + live.graph_pan;

            // Soft vignette / grid
            painter.rect_filled(rect, CornerRadius::same(14), Color32::from_rgb(14, 17, 23));
            for i in -8..=8 {
                let x = origin.x + i as f32 * 80.0 * live.graph_zoom;
                let y = origin.y + i as f32 * 80.0 * live.graph_zoom;
                if rect.x_range().contains(x) {
                    painter.line_segment(
                        [Pos2::new(x, rect.top()), Pos2::new(x, rect.bottom())],
                        Stroke::new(1.0, Color32::from_rgba_unmultiplied(255, 255, 255, 8)),
                    );
                }
                if rect.y_range().contains(y) {
                    painter.line_segment(
                        [Pos2::new(rect.left(), y), Pos2::new(rect.right(), y)],
                        Stroke::new(1.0, Color32::from_rgba_unmultiplied(255, 255, 255, 8)),
                    );
                }
            }

            // Zoom (scroll) + pan (drag background)
            if resp.hovered() {
                let scroll = ui.input(|i| i.smooth_scroll_delta.y);
                if scroll.abs() > 0.1 {
                    let before = live.graph_zoom;
                    live.graph_zoom = (live.graph_zoom * (1.0 + scroll * 0.0015)).clamp(0.35, 2.8);
                    // zoom toward pointer
                    if let Some(p) = resp.hover_pos() {
                        let world = (p - origin) / before;
                        live.graph_pan += world * (before - live.graph_zoom);
                    }
                }
            }
            if resp.dragged() && live.graph_drag.is_none() {
                live.graph_pan += resp.drag_delta();
            }

            let to_screen = |p: Vec2, pan: Vec2, zoom: f32, center: Pos2| -> Pos2 {
                center + pan + p * zoom
            };

            // Selected cell node (neighbor-edge focus) + parent eNB highlight.
            let selected_node = live.graph_nodes.iter().enumerate().find_map(|(i, n)| {
                if n.kind == GraphKind::Cell && n.flat_ix.is_some() && n.flat_ix == live.selected {
                    Some(i)
                } else {
                    None
                }
            });
            let selected_enb = live.selected.and_then(|sel| {
                for op in &live.operators {
                    for site in &op.sites {
                        if site.cells.contains(&sel) {
                            return live.graph_nodes.iter().position(|n| n.id == site.key);
                        }
                    }
                }
                None
            });

            let filtering = live.search_active();
            let hits = live.match_set();
            let node_is_hit: Vec<bool> = live
                .graph_nodes
                .iter()
                .map(|n| graph_node_matches(live, n, &hits))
                .collect();
            let node_oci_miss: Vec<bool> = live
                .graph_nodes
                .iter()
                .map(|n| graph_node_oci_unmatched(live, n))
                .collect();
            let node_oci_warn: Vec<bool> = live
                .graph_nodes
                .iter()
                .map(|n| graph_node_oci_warn(live, n))
                .collect();

            // Edges — hierarchy always; neighbor links only for the selected cell.
            for e in &live.graph_edges {
                let Some(na) = live.graph_nodes.get(e.a) else { continue };
                let Some(nb) = live.graph_nodes.get(e.b) else { continue };
                if matches!(e.kind, EdgeKind::Neighbor) {
                    let Some(sel) = selected_node else { continue };
                    if e.a != sel && e.b != sel {
                        continue;
                    }
                }
                let a = to_screen(na.pos, live.graph_pan, live.graph_zoom, rect.center());
                let b = to_screen(nb.pos, live.graph_pan, live.graph_zoom, rect.center());
                let dim_edge = filtering
                    && !node_is_hit.get(e.a).copied().unwrap_or(false)
                    && !node_is_hit.get(e.b).copied().unwrap_or(false);
                let stroke = match e.kind {
                    EdgeKind::Hierarchy => Stroke::new(
                        1.4,
                        Color32::from_rgba_unmultiplied(148, 163, 184, if dim_edge { 18 } else { 55 }),
                    ),
                    EdgeKind::Neighbor => Stroke::new(
                        1.8,
                        Color32::from_rgba_unmultiplied(56, 189, 248, 160),
                    ),
                };
                painter.line_segment([a, b], stroke);
            }

            // Nodes
            let mut hit: Option<usize> = None;
            let pointer = resp.interact_pointer_pos();
            let z = live.graph_zoom;
            for (i, n) in live.graph_nodes.iter().enumerate() {
                let c = to_screen(n.pos, live.graph_pan, live.graph_zoom, rect.center());
                let r = match n.kind {
                    GraphKind::Operator => 22.0,
                    GraphKind::Enb => 14.0,
                    GraphKind::Cell => 11.0,
                    GraphKind::Heard => 8.0,
                } * z.sqrt();

                let selected = Some(i) == selected_node || Some(i) == selected_enb;
                let search_hit = node_is_hit.get(i).copied().unwrap_or(false);
                let oci_miss = node_oci_miss.get(i).copied().unwrap_or(false);
                let oci_warn = node_oci_warn.get(i).copied().unwrap_or(false);
                let dim = filtering && !search_hit && !selected;
                // Keep node fill stable; status = ring only (no red/yellow repaint war).
                let col = if dim {
                    th.wash(n.color, 45)
                } else {
                    n.color
                };
                let halo = if selected {
                    th.wash(th.accent, 50)
                } else if search_hit && filtering {
                    th.wash(th.accent, 40)
                } else {
                    th.wash(n.color, 28)
                };
                painter.circle_filled(c, r + 5.0, halo);
                painter.circle_filled(c, r, col);
                let (stroke_w, stroke_col) = if oci_miss && !dim {
                    (2.2, th.wash(th.danger, 200))
                } else if oci_warn && !dim {
                    (2.0, th.wash(th.warning, 170))
                } else if selected {
                    (2.0, th.wash(Color32::WHITE, 160))
                } else if search_hit {
                    (1.6, th.wash(th.accent, 150))
                } else if dim {
                    (1.0, th.wash(Color32::WHITE, 20))
                } else {
                    (1.0, th.wash(Color32::WHITE, 50))
                };
                painter.circle_stroke(c, r, Stroke::new(stroke_w, stroke_col));

                // Label LOD — less overlap when zoomed out; search hits stay labeled.
                let show_label = match n.kind {
                    GraphKind::Operator => true,
                    GraphKind::Enb => z > 0.55 || selected || search_hit,
                    GraphKind::Cell => z > 0.85 || selected || search_hit,
                    GraphKind::Heard => z > 1.1 || selected,
                };
                let show_sub = matches!(n.kind, GraphKind::Cell | GraphKind::Enb)
                    && (z > 1.15 || selected || search_hit);

                if show_label {
                    let font = match n.kind {
                        GraphKind::Operator => 14.0,
                        GraphKind::Enb => 12.0,
                        _ => 11.0,
                    };
                    painter.text(
                        c + Vec2::new(0.0, r + 10.0),
                        egui::Align2::CENTER_TOP,
                        &n.label,
                        egui::FontId::proportional(font),
                        if selected || search_hit {
                            th.text
                        } else if dim {
                            Color32::from_rgba_unmultiplied(148, 163, 184, 50)
                        } else {
                            th.muted
                        },
                    );
                }
                if show_sub {
                    painter.text(
                        c + Vec2::new(0.0, r + 24.0),
                        egui::Align2::CENTER_TOP,
                        &n.sub,
                        egui::FontId::proportional(10.0),
                        th.muted,
                    );
                }

                if let Some(p) = pointer {
                    if c.distance(p) <= r + 6.0 {
                        hit = Some(i);
                    }
                }
            }

            // Legend
            let legend = Rect::from_min_size(rect.min + Vec2::new(14.0, 12.0), Vec2::new(248.0, 100.0));
            painter.rect_filled(
                legend,
                CornerRadius::same(10),
                Color32::from_rgba_unmultiplied(20, 24, 32, 200),
            );
            let mut ly = legend.min.y + 12.0;
            for (lab, col) in [
                ("Hub = operator brand", th.accent),
                ("Ring red = OCI miss", th.danger),
                ("Ring amber = OCI field diff", th.warning),
                ("Ring blue = search / neighbors", th.accent),
            ] {
                painter.circle_filled(Pos2::new(legend.min.x + 16.0, ly + 5.0), 5.0, col);
                painter.text(
                    Pos2::new(legend.min.x + 28.0, ly),
                    egui::Align2::LEFT_TOP,
                    lab,
                    egui::FontId::proportional(12.0),
                    th.text,
                );
                ly += 18.0;
            }

            // Interaction
            if resp.drag_started() {
                if let Some(i) = hit {
                    live.graph_drag = Some(i);
                    // Keep settled: dragging pins the node; don't relaunch the whole simmer.
                    select_graph_node(live, i);
                }
            }
            if let Some(i) = live.graph_drag {
                if resp.dragged() {
                    let z = live.graph_zoom.max(0.2);
                    if let Some(n) = live.graph_nodes.get_mut(i) {
                        n.pos += resp.drag_delta() / z;
                    }
                }
            }
            if resp.drag_stopped() {
                live.graph_drag = None;
            }
            if resp.clicked() {
                if let Some(i) = hit {
                    select_graph_node(live, i);
                }
            }

            if live.graph_nodes.is_empty() {
                painter.text(
                    rect.center(),
                    egui::Align2::CENTER_CENTER,
                    "No graph yet - wait for towers",
                    egui::FontId::proportional(18.0),
                    th.muted,
                );
            }
        });
}

fn empty_state(ui: &mut Ui, th: &Theme) {
    ui.vertical_centered(|ui| {
        ui.add_space(100.0);
        ui.label(RichText::new("No towers in feed yet").size(22.0).color(th.text));
        ui.add_space(8.0);
        ui.label(
            RichText::new("Keep the scanner running - operators and sites appear as ML1 and SIB1 land.")
                .size(15.0)
                .color(th.muted),
        );
    });
}

fn render_operator(
    ui: &mut Ui,
    th: &Theme,
    live: &mut LiveState,
    op: &OperatorNode,
    hits: &BTreeSet<usize>,
    filtering: bool,
) {
    let open = live.expanded.contains(&op.key) || filtering;
    let site_n = op.sites.len();
    let cell_n: usize = op.sites.iter().map(|s| s.cells.len()).sum();
    let heard_n = op.incomplete.len();
    let accent = op.color;

    Frame::new()
        .fill(th.panel)
        .stroke(Stroke::new(1.0, th.stroke))
        .corner_radius(CornerRadius::same(14))
        .inner_margin(Margin::ZERO)
        .show(ui, |ui| {
            ui.horizontal(|ui| {
                let h = ui.available_height().max(72.0);
                let (strip, _) = ui.allocate_exact_size(Vec2::new(6.0, h), Sense::hover());
                ui.painter().rect_filled(
                    strip,
                    CornerRadius {
                        nw: 14,
                        sw: 14,
                        ne: 0,
                        se: 0,
                    },
                    accent,
                );

                ui.vertical(|ui| {
                    let mut do_toggle = false;
                    Frame::new()
                        .inner_margin(Margin::symmetric(16, 14))
                        .show(ui, |ui| {
                            ui.horizontal(|ui| {
                                if expand_toggle(ui, open, th.accent).clicked() {
                                    do_toggle = true;
                                }
                                ui.add_space(8.0);
                                operator_dot(ui, op.color, 8.0);
                                ui.add_space(10.0);
                                ui.vertical(|ui| {
                                    let title = ui.add(
                                        egui::Label::new(
                                            RichText::new(operator_label(&op.plmn))
                                                .strong()
                                                .size(22.0)
                                                .color(th.text),
                                        )
                                        .sense(Sense::click()),
                                    );
                                    if title.clicked() {
                                        do_toggle = true;
                                    }
                                    ui.add_space(6.0);
                                    ui.label(
                                        RichText::new(format!(
                                            "{site_n} sites   ·   {cell_n} full   ·   {heard_n} heard RF"
                                        ))
                                        .size(14.0)
                                        .color(th.muted),
                                    );
                                });
                                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                                    if op.best_rsrp > -200.0 {
                                        ui.vertical(|ui| {
                                            ui.with_layout(
                                                Layout::right_to_left(Align::Center),
                                                |ui| {
                                                    ui.label(
                                                        RichText::new(format!(
                                                            "{:.0} dBm",
                                                            op.best_rsrp
                                                        ))
                                                        .strong()
                                                        .size(20.0)
                                                        .color(rsrp_color(th, op.best_rsrp)),
                                                    );
                                                },
                                            );
                                            ui.add_space(6.0);
                                            signal_meter(ui, th, op.best_rsrp, 140.0, 11.0);
                                        });
                                    }
                                });
                            });
                        });

                    if do_toggle {
                        toggle_expand(&mut live.expanded, &op.key);
                    }

                    if open {
                        ui.separator();
                        Frame::new()
                            .fill(Color32::from_rgb(20, 24, 32))
                            .inner_margin(Margin::symmetric(14, 14))
                            .show(ui, |ui| {
                                for site in &op.sites {
                                    if filtering && !site_has_match(site, hits) {
                                        continue;
                                    }
                                    render_site(ui, th, live, site, hits, filtering);
                                    ui.add_space(10.0);
                                }

                                let incomplete: Vec<usize> = op
                                    .incomplete
                                    .iter()
                                    .copied()
                                    .filter(|ix| !filtering || hits.contains(ix))
                                    .collect();
                                if !incomplete.is_empty() {
                                    let hk = heard_key(&op.plmn);
                                    let hop = live.expanded.contains(&hk) || filtering;
                                    let mut heard_toggle = false;
                                    Frame::new()
                                        .fill(th.panel2)
                                        .stroke(Stroke::new(1.0, th.stroke))
                                        .corner_radius(CornerRadius::same(10))
                                        .inner_margin(Margin::symmetric(12, 10))
                                        .show(ui, |ui| {
                                            ui.horizontal(|ui| {
                                                if nest_toggle(ui, hop, th.accent).clicked() {
                                                    heard_toggle = true;
                                                }
                                                ui.add_space(6.0);
                                                ui.vertical(|ui| {
                                                    ui.label(
                                                        RichText::new(format!(
                                                            "Heard RF  ·  PCI, no CID  ·  {}",
                                                            incomplete.len()
                                                        ))
                                                        .strong()
                                                        .size(14.0)
                                                        .color(th.text),
                                                    );
                                                    ui.label(
                                                        RichText::new(
                                                            "Not under an eNB — identity unknown. Peer EARFCN notes are hints only.",
                                                        )
                                                        .size(11.5)
                                                        .color(th.muted),
                                                    );
                                                });
                                            });
                                        });
                                    if heard_toggle {
                                        toggle_expand(&mut live.expanded, &hk);
                                    }
                                    if hop {
                                        ui.add_space(8.0);
                                        for ix in incomplete {
                                            let peers = earfcn_peer_enbs(live, op, ix);
                                            render_cell_row(
                                                ui,
                                                th,
                                                live,
                                                ix,
                                                1,
                                                CellKind::Heard,
                                            );
                                            if !peers.is_empty() {
                                                ui.horizontal(|ui| {
                                                    ui.add_space(40.0);
                                                    ui.label(
                                                        RichText::new(format!(
                                                            "same EARFCN as eNB {} (unconfirmed)",
                                                            peers.join(", ")
                                                        ))
                                                        .size(11.0)
                                                        .color(th.muted),
                                                    );
                                                });
                                            }
                                            ui.add_space(6.0);
                                        }
                                    }
                                }
                            });
                    }
                });
            });
        });
}

fn render_site(
    ui: &mut Ui,
    th: &Theme,
    live: &mut LiveState,
    site: &SiteNode,
    hits: &BTreeSet<usize>,
    filtering: bool,
) {
    let open = live.expanded.contains(&site.key) || filtering;
    let full_n = site.cells.len();
    let accent = rsrp_color(th, site.best_rsrp);
    let has_serving = site
        .cells
        .iter()
        .any(|&i| live.flat.get(i).map(|f| f.tower.is_serving()).unwrap_or(false));
    let has_camped = site
        .cells
        .iter()
        .any(|&i| live.flat.get(i).map(|f| f.tower.was_identity_camped()).unwrap_or(false));
    let oci_bad = live.site_unmatched(site);
    let oci_warn = !oci_bad
        && site
            .cells
            .iter()
            .any(|&ix| live.ext_status_ref(ix).has_field_mismatch());

    let surface = if oci_bad {
        SurfaceState::Danger
    } else if has_serving {
        SurfaceState::Serving
    } else if has_camped {
        SurfaceState::Camped
    } else {
        SurfaceState::Idle
    };

    Frame::new()
        .fill(th.surface_fill(surface))
        .stroke(th.surface_stroke(surface))
        .corner_radius(CornerRadius::same(12))
        .inner_margin(Margin::symmetric(12, 10))
        .show(ui, |ui| {
            let mut do_toggle = false;
            ui.horizontal(|ui| {
                if expand_toggle(ui, open, th.accent).clicked() {
                    do_toggle = true;
                }
                ui.add_space(8.0);
                ui.vertical(|ui| {
                    ui.horizontal(|ui| {
                        let title = ui.add(
                            egui::Label::new(
                                RichText::new(format!("{}  eNB  {}", icons::CELL_TOWER, site.enb))
                                    .strong()
                                    .size(18.0)
                                    .color(th.text),
                            )
                            .sense(Sense::click()),
                        );
                        if title.clicked() {
                            do_toggle = true;
                        }
                        ui.add_space(8.0);
                        if !site.tac.is_empty() {
                            badge(ui, &format!("TAC {}", site.tac), th.muted);
                        }
                        if oci_bad {
                            ui.add_space(4.0);
                            badge_icon(ui, icons::X_CIRCLE, "MISS", th.danger);
                        } else if oci_warn {
                            ui.add_space(4.0);
                            badge_icon(ui, icons::WARNING, "DIFF", th.warning);
                        }
                        if has_serving {
                            ui.add_space(4.0);
                            badge_icon(ui, icons::BROADCAST, "SERVING", th.serving);
                        } else if has_camped {
                            ui.add_space(4.0);
                            badge_icon(ui, icons::MAP_PIN, "CAMPED", th.warning);
                        }
                    });
                    ui.label(
                        RichText::new(format!("{full_n} full carriers"))
                            .size(13.0)
                            .color(th.muted),
                    );
                });
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if site.best_rsrp > -200.0 {
                        ui.label(
                            RichText::new(format!("{:.0} dBm", site.best_rsrp))
                                .strong()
                                .size(16.0)
                                .color(accent),
                        );
                    }
                });
            });

            if do_toggle {
                toggle_expand(&mut live.expanded, &site.key);
            }

            if open {
                ui.add_space(8.0);
                for &cix in &site.cells {
                    if filtering && !hits.contains(&cix) {
                        continue;
                    }
                    render_cell_row(ui, th, live, cix, 1, CellKind::Cell);
                    ui.add_space(6.0);
                }
            }
        });
}

#[derive(Clone, Copy)]
enum CellKind {
    Cell,
    Heard,
}

fn render_cell_row(ui: &mut Ui, th: &Theme, live: &mut LiveState, ix: usize, depth: u8, kind: CellKind) {
    let ft = live.flat[ix].clone();
    let t = &ft.tower;
    let (fill, fill_c) = fill_label(th, t);
    let rsrp = parse_f32(t.rxl());
    let serving = t.is_serving();
    let camped = t.was_identity_camped() && !serving;
    let selected = live.selected == Some(ix);
    let indent = 12.0 + depth as f32 * 22.0;
    // Read-only — never enqueue OCI from every painted row (was a major lag source).
    let ext = live.ext_status_ref(ix);
    let oci_miss = ext.is_unmatched();
    let oci_diff = ext.has_field_mismatch();
    let nb_n = t.neighbor_count();
    let nb_key = cell_nb_key(&t.key);
    let nb_open = nb_n > 0 && live.expanded.contains(&nb_key);

    // Status via border + badges — keep fills calm so text stays readable.
    let surface = if oci_miss {
        SurfaceState::Danger
    } else if selected {
        SurfaceState::Selected
    } else if serving {
        SurfaceState::Serving
    } else if camped {
        SurfaceState::Camped
    } else {
        SurfaceState::Idle
    };
    let bg = match (surface, kind) {
        (SurfaceState::Idle, CellKind::Heard) => Color32::from_rgb(24, 28, 36),
        (s, _) => th.surface_fill(s),
    };
    let border = if oci_diff && !oci_miss {
        Stroke::new(1.2, th.wash(th.warning, 140))
    } else {
        th.surface_stroke(surface)
    };

    let mut toggle_nb = false;
    let resp = Frame::new()
        .fill(bg)
        .stroke(border)
        .corner_radius(CornerRadius::same(10))
        .inner_margin(Margin {
            left: (indent + 12.0) as i8,
            right: 14,
            top: 10,
            bottom: 10,
        })
        .show(ui, |ui| {
            ui.horizontal(|ui| {
                if nb_n > 0 {
                    if nest_toggle(ui, nb_open, th.accent)
                        .on_hover_text("Neighbor hints (SIB / meas)")
                        .clicked()
                    {
                        toggle_nb = true;
                    }
                    ui.add_space(4.0);
                }

                ui.vertical(|ui| {
                    ui.set_min_width(78.0);
                    let (role_icon, role, role_c) = if serving {
                        (icons::CELL_SIGNAL_FULL, "SERVING", th.serving)
                    } else if camped {
                        (icons::MAP_PIN, "CAMPED", th.warning)
                    } else {
                        match kind {
                            CellKind::Cell => (icons::CELL_TOWER, "CELL", th.accent),
                            CellKind::Heard => (icons::CIRCLE, "RADIO", th.muted),
                        }
                    };
                    ui.label(
                        RichText::new(format!("{role_icon}  {role}"))
                            .strong()
                            .size(11.0)
                            .color(role_c),
                    );
                    ui.label(
                        RichText::new(if t.band().is_empty() { "-" } else { t.band() })
                            .size(15.0)
                            .strong()
                            .color(th.text),
                    );
                });

                ui.add_space(8.0);

                ui.vertical(|ui| {
                    ui.set_min_width(130.0);
                    ui.label(RichText::new("EARFCN / PCI").size(11.0).color(th.muted));
                    ui.label(
                        RichText::new(format!("{}/{}", dash(t.channel()), dash(t.cell_code())))
                            .strong()
                            .size(17.0)
                            .color(th.text),
                    );
                });

                ui.add_space(8.0);

                ui.vertical(|ui| {
                    ui.set_min_width(150.0);
                    ui.label(RichText::new("Identity").size(11.0).color(th.muted));
                    if t.get_id("cid").is_empty() {
                        ui.label(
                            RichText::new(match kind {
                                CellKind::Heard if !t.plmn().is_empty() => {
                                    format!("{}  -  PCI only (neighbor/lock)", normalize_plmn(t.plmn()))
                                }
                                CellKind::Heard => "PCI only  -  no CID".into(),
                                CellKind::Cell if !t.plmn().is_empty() => {
                                    format!("{}  -  pending CID", normalize_plmn(t.plmn()))
                                }
                                CellKind::Cell => "No CID yet".into(),
                            })
                            .size(15.0)
                            .color(th.muted),
                        );
                    } else {
                        ui.label(
                            RichText::new(format!("CID  {}", t.get_id("cid")))
                                .strong()
                                .size(15.0)
                                .color(th.text),
                        );
                    }
                });

                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if nb_n > 0 {
                        let nb_btn = ui
                            .add(
                                egui::Button::new(
                                    RichText::new(format!("NB {nb_n}"))
                                        .size(12.0)
                                        .strong()
                                        .color(if nb_open {
                                            th.accent
                                        } else {
                                            th.muted
                                        }),
                                )
                                .fill(Color32::from_rgba_unmultiplied(
                                    th.accent.r(),
                                    th.accent.g(),
                                    th.accent.b(),
                                    if nb_open { 40 } else { 18 },
                                ))
                                .corner_radius(CornerRadius::same(6))
                                .min_size(Vec2::new(52.0, 26.0)),
                            )
                            .on_hover_text("SIB / meas neighbor hints");
                        if nb_btn.clicked() {
                            toggle_nb = true;
                        }
                        ui.add_space(6.0);
                    }
                    badge(ui, fill, fill_c);
                    ui.add_space(6.0);
                    match &ext {
                        ExtStatus::NotInDb { .. } => {
                            badge_icon(ui, icons::X_CIRCLE, "MISS", th.danger)
                        }
                        ExtStatus::Found { .. } if oci_diff => {
                            badge_icon(ui, icons::WARNING, "DIFF", th.warning)
                        }
                        ExtStatus::Found { .. } => {
                            badge_icon(ui, icons::CHECK_CIRCLE, "OK", th.ok)
                        }
                        ExtStatus::Pending => badge(ui, "…", th.muted),
                        ExtStatus::Error(_) => {
                            badge_icon(ui, icons::WARNING_CIRCLE, "ERR", th.warning)
                        }
                        ExtStatus::NoKey | ExtStatus::Skipped => {}
                    }
                    ui.add_space(10.0);
                    ui.vertical(|ui| {
                        ui.with_layout(Layout::right_to_left(Align::Min), |ui| {
                            if rsrp > -200.0 {
                                ui.label(
                                    RichText::new(format!("{rsrp:.1}"))
                                        .strong()
                                        .size(18.0)
                                        .color(rsrp_color(th, rsrp)),
                                );
                            } else {
                                ui.label(RichText::new("-").size(18.0).color(th.muted));
                            }
                        });
                        ui.add_space(3.0);
                        signal_meter(ui, th, rsrp, 96.0, 8.0);
                    });
                });
            });
        })
        .response
        .interact(Sense::click());

    if live.scroll_to_selected && live.selected == Some(ix) {
        resp.scroll_to_me(Some(Align::Center));
        live.scroll_to_selected = false;
    }

    if toggle_nb {
        toggle_expand(&mut live.expanded, &nb_key);
    } else if resp.clicked() {
        live.selected = Some(ix);
    }

    // Nested neighbor hints under the cell.
    if nb_open {
        ui.add_space(4.0);
        let child_indent = indent + 28.0;
        Frame::new()
            .fill(Color32::from_rgb(18, 22, 28))
            .stroke(Stroke::new(1.0, Color32::from_rgba_unmultiplied(56, 189, 248, 40)))
            .corner_radius(CornerRadius::same(8))
            .inner_margin(Margin {
                left: (child_indent + 10.0) as i8,
                right: 12,
                top: 8,
                bottom: 8,
            })
            .show(ui, |ui| {
                ui.label(
                    RichText::new(format!(
                        "Neighbor hints ({nb_n}) - SIB5 / meas, often EARFCN-only"
                    ))
                    .size(12.0)
                    .color(th.muted),
                );
                ui.add_space(6.0);
                for n in t
                    .neighbors
                    .nb_lte
                    .iter()
                    .chain(t.neighbors.nb_umts.iter())
                    .chain(t.neighbors.nb_gsm.iter())
                    .take(24)
                {
                    let rat = if !n.rat.is_empty() {
                        n.rat.as_str()
                    } else {
                        "LTE"
                    };
                    let ch = n
                        .radio
                        .get("earfcn")
                        .or_else(|| n.radio.get("uarfcn"))
                        .or_else(|| n.radio.get("arfcn"))
                        .map(|s| s.as_str())
                        .unwrap_or("?");
                    let pci = n
                        .radio
                        .get("pci")
                        .or_else(|| n.radio.get("psc"))
                        .or_else(|| n.radio.get("bsic"))
                        .map(|s| s.as_str())
                        .filter(|s| !s.is_empty())
                        .unwrap_or("-");
                    let rxl = n.signal.get("rxl").map(|s| s.as_str()).unwrap_or("-");
                    Frame::new()
                        .fill(Color32::from_rgb(28, 34, 42))
                        .corner_radius(CornerRadius::same(6))
                        .inner_margin(Margin::symmetric(10, 6))
                        .show(ui, |ui| {
                            ui.horizontal(|ui| {
                                ui.label(
                                    RichText::new(rat)
                                        .size(11.0)
                                        .strong()
                                        .color(th.accent),
                                );
                                ui.add_space(8.0);
                                ui.label(
                                    RichText::new(format!("{ch} / {pci}"))
                                        .size(13.0)
                                        .strong()
                                        .color(th.text),
                                );
                                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                                    ui.label(
                                        RichText::new(rxl).size(13.0).color(th.muted),
                                    );
                                });
                            });
                        });
                    ui.add_space(3.0);
                }
                if nb_n > 24 {
                    ui.add_space(2.0);
                    ui.label(
                        RichText::new(format!("...and {} more in Inspector", nb_n - 24))
                            .size(11.5)
                            .color(th.muted),
                    );
                }
            });
        ui.add_space(4.0);
    }
}

fn render_detail(ui: &mut Ui, th: &Theme, ft: &FlatTower, ext: &ExtStatus) {
    let t = &ft.tower;
    let (fill, fill_c) = fill_label(th, t);

    ui.horizontal_wrapped(|ui| {
        ui.label(
            RichText::new(ft.rat.as_str())
                .strong()
                .size(22.0)
                .color(th.rat(ft.rat)),
        );
        ui.add_space(6.0);
        badge(ui, fill, fill_c);
        if t.is_serving() {
            badge_icon(ui, icons::CELL_SIGNAL_FULL, "SERVING", th.serving);
        } else if t.was_identity_camped() {
            badge_icon(ui, icons::MAP_PIN, "CAMPED", th.warning);
        } else if t.was_camped() && !t.has_cid() {
            badge(ui, "RF LOCK", th.muted);
        }
        match ext {
            ExtStatus::NotInDb { .. } => badge_icon(ui, icons::X_CIRCLE, "MISS", th.danger),
            ExtStatus::Found { .. } if ext.has_field_mismatch() => {
                badge_icon(ui, icons::WARNING, "DIFF", th.warning)
            }
            ExtStatus::Found { .. } => badge_icon(ui, icons::CHECK_CIRCLE, "OK", th.ok),
            ExtStatus::Pending => badge(ui, "…", th.muted),
            ExtStatus::Error(_) => badge_icon(ui, icons::WARNING_CIRCLE, "ERR", th.warning),
            ExtStatus::NoKey => badge(ui, "OFF", th.muted),
            ExtStatus::Skipped => {}
        }
    });
    ui.add_space(6.0);
    ui.label(RichText::new(&t.key).size(13.0).monospace().color(th.muted));

    ui.add_space(10.0);
    egui::CollapsingHeader::new(
        RichText::new("OpenCelliD merge / diff")
            .strong()
            .size(13.5)
            .color(th.text),
    )
    .default_open(true)
    .show(ui, |ui| {
        render_ext_compare(ui, th, ft, ext);
    });

    ui.add_space(6.0);
    egui::CollapsingHeader::new(
        RichText::new("Identity")
            .strong()
            .size(13.5)
            .color(th.text),
    )
    .default_open(true)
    .show(ui, |ui| {
        let plmn = normalize_plmn(t.plmn());
        kv_row(ui, th, "PLMN", &plmn);
        let (brand, _) = operator_brand(&plmn);
        if !brand.is_empty() {
            kv_row(ui, th, "Operator", brand);
        }
        kv_row(ui, th, "TAC / LAC", t.lac_or_tac());
        kv_row(ui, th, "CID", t.get_id("cid"));
        kv_row(ui, th, "eNB / RNC", {
            let e = t.get_id("enb_id");
            if e.is_empty() {
                t.get_id("rnc_id")
            } else {
                e
            }
        });
    });

    ui.add_space(4.0);
    egui::CollapsingHeader::new(
        RichText::new("Radio")
            .strong()
            .size(13.5)
            .color(th.text),
    )
    .default_open(true)
    .show(ui, |ui| {
        kv_row(ui, th, "Channel", t.channel());
        kv_row(ui, th, "Code", t.cell_code());
        kv_row(ui, th, "Band", t.band());
        kv_row(ui, th, "RSRP", t.rxl());
        kv_row(ui, th, "RSRQ", t.get_sig("rsrq"));
    });

    ui.add_space(4.0);
    egui::CollapsingHeader::new(
        RichText::new("Session")
            .strong()
            .size(13.5)
            .color(th.text),
    )
    .default_open(false)
    .show(ui, |ui| {
        kv_row(ui, th, "Serving now", if t.is_serving() { "1" } else { "0" });
        kv_row(
            ui,
            th,
            "Camped (identity)",
            if t.was_identity_camped() { "1" } else { "0" },
        );
        kv_row(
            ui,
            th,
            "RF lock sticky (raw)",
            if t.was_camped() { "1" } else { "0" },
        );
        kv_row(ui, th, "Seen", &t.meta.seen);
        kv_row(ui, th, "Last seen", &t.meta.last_seen);
    });

    let nb = t.neighbor_count();
    if nb > 0 {
        ui.add_space(4.0);
        egui::CollapsingHeader::new(
            RichText::new(format!("Neighbor hints ({nb})"))
                .strong()
                .size(13.5)
                .color(th.text),
        )
        .default_open(false)
        .show(ui, |ui| {
            ui.label(
                RichText::new("Often EARFCN-only (SIB5) - not full towers until PCI+CID land")
                    .size(12.0)
                    .color(th.muted),
            );
            ui.add_space(8.0);
            for n in t.neighbors.nb_lte.iter().take(40) {
                let ch = n.radio.get("earfcn").map(|s| s.as_str()).unwrap_or("?");
                let pci = n
                    .radio
                    .get("pci")
                    .map(|s| s.as_str())
                    .filter(|s| !s.is_empty())
                    .unwrap_or("-");
                let rxl = n.signal.get("rxl").map(|s| s.as_str()).unwrap_or("-");
                Frame::new()
                    .fill(th.panel2)
                    .corner_radius(CornerRadius::same(8))
                    .inner_margin(Margin::symmetric(10, 8))
                    .show(ui, |ui| {
                        ui.horizontal(|ui| {
                            ui.label(
                                RichText::new(format!("{ch} / {pci}"))
                                    .size(14.0)
                                    .strong()
                                    .color(th.text),
                            );
                            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                                ui.label(RichText::new(rxl).size(14.0).color(th.accent));
                            });
                        });
                    });
                ui.add_space(4.0);
            }
        });
    }
}

fn render_ext_compare(ui: &mut Ui, th: &Theme, ft: &FlatTower, ext: &ExtStatus) {
    match ext {
        ExtStatus::NoKey => {
            ui.label(
                RichText::new("Set OPENCELLID_API_KEY to look up MCC/MNC/TAC/CID.")
                    .size(13.0)
                    .color(th.muted),
            );
            return;
        }
        ExtStatus::Skipped => {
            ui.label(
                RichText::new("No primary identity (need PLMN + TAC + CID) - skip lookup.")
                    .size(13.0)
                    .color(th.muted),
            );
            return;
        }
        ExtStatus::Pending => {
            ui.label(
                RichText::new("Looking up primary identity in OpenCelliD...")
                    .size(13.0)
                    .color(th.muted),
            );
            return;
        }
        ExtStatus::Error(e) => {
            ui.label(
                RichText::new(format!("OCI error: {e}"))
                    .size(13.0)
                    .color(th.warning),
            );
            return;
        }
        ExtStatus::NotInDb { .. } | ExtStatus::Found { .. } => {}
    }

    let Some(diffs) = ext.diffs() else {
        return;
    };

    // Status strip
    match ext {
        ExtStatus::NotInDb { .. } => {
            Frame::new()
                .fill(th.surface_fill(SurfaceState::Danger))
                .stroke(th.surface_stroke(SurfaceState::Danger))
                .corner_radius(CornerRadius::same(8))
                .inner_margin(Margin::symmetric(10, 8))
                .show(ui, |ui| {
                    ui.horizontal(|ui| {
                        ui.label(
                            RichText::new(icons::X_CIRCLE)
                                .size(15.0)
                                .color(th.danger),
                        );
                        ui.label(
                            RichText::new("CGI not in OpenCelliD — merge = ours only")
                                .strong()
                                .size(13.0)
                                .color(th.text),
                        );
                    });
                    let t = &ft.tower;
                    ui.label(
                        RichText::new(format!(
                            "key  LTE/{}/{}/TAC {}/CID {}",
                            t.get_id("mcc"),
                            t.get_id("mnc"),
                            t.lac_or_tac(),
                            t.get_id("cid")
                        ))
                        .size(11.5)
                        .monospace()
                        .color(th.muted),
                    );
                });
        }
        ExtStatus::Found { .. } => {
            let conflicts = diffs.iter().filter(|d| d.side == DiffSide::Mismatch).count();
            let from_oci = diffs.iter().filter(|d| d.side == DiffSide::ExtOnly).count();
            let surface = if conflicts == 0 {
                SurfaceState::Serving
            } else {
                SurfaceState::Camped
            };
            let icon = if conflicts == 0 {
                icons::CHECK_CIRCLE
            } else {
                icons::WARNING
            };
            let icon_c = if conflicts == 0 { th.ok } else { th.warning };
            Frame::new()
                .fill(th.surface_fill(surface))
                .stroke(th.surface_stroke(surface))
                .corner_radius(CornerRadius::same(8))
                .inner_margin(Margin::symmetric(10, 8))
                .show(ui, |ui| {
                    ui.horizontal(|ui| {
                        ui.label(RichText::new(icon).size(15.0).color(icon_c));
                        ui.label(
                            RichText::new(if conflicts == 0 {
                                format!("CGI found — merged (+{from_oci} from OCI)")
                            } else {
                                format!(
                                    "CGI found — {conflicts} conflict(s), +{from_oci} from OCI"
                                )
                            })
                            .strong()
                            .size(13.0)
                            .color(th.text),
                        );
                    });
                });
        }
        _ => {}
    }

    ui.add_space(8.0);
    ui.label(
        RichText::new("OpenCelliD != CellMapper - Mapper hit can still be OCI miss")
            .size(11.5)
            .color(th.muted),
    );

    ui.add_space(8.0);
    egui::CollapsingHeader::new(
        RichText::new("Merged passport (ours U OCI)")
            .strong()
            .size(13.0)
            .color(th.text),
    )
    .default_open(true)
    .show(ui, |ui| {
        for d in diffs {
            let val = d.merged_value();
            if val.is_empty() {
                continue;
            }
            let (bg, tag_c, tag) = match d.side {
                DiffSide::Match => (th.panel2, th.ok, "both"),
                DiffSide::Mismatch => (th.wash(th.danger, 22), th.danger, "!="),
                DiffSide::ExtOnly => (th.wash(th.accent, 18), th.accent, "oci"),
                DiffSide::OursOnly => (th.panel2, th.muted, "ours"),
            };
            Frame::new()
                .fill(bg)
                .corner_radius(CornerRadius::same(6))
                .inner_margin(Margin::symmetric(8, 5))
                .show(ui, |ui| {
                    ui.horizontal(|ui| {
                        ui.label(RichText::new(d.label).size(12.0).color(th.muted));
                        ui.label(RichText::new(tag).size(10.5).strong().color(tag_c));
                        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                            if d.side == DiffSide::Mismatch {
                                ui.label(
                                    RichText::new(format!(
                                        "{} ≠ {}",
                                        dash(&d.ours),
                                        dash(&d.ext)
                                    ))
                                    .size(13.0)
                                    .strong()
                                    .color(th.text),
                                );
                            } else {
                                ui.label(
                                    RichText::new(val).size(13.5).strong().color(th.text),
                                );
                            }
                        });
                    });
                });
            ui.add_space(3.0);
        }
    });

    ui.add_space(6.0);
    egui::CollapsingHeader::new(
        RichText::new("Side-by-side diff")
            .strong()
            .size(13.0)
            .color(th.text),
    )
    .default_open(false)
    .show(ui, |ui| {
        ui.horizontal(|ui| {
            legend_chip(ui, th, "match", th.ok);
            legend_chip(ui, th, "mismatch", th.danger);
            legend_chip(ui, th, "only oci", th.accent);
            legend_chip(ui, th, "only ours", th.muted);
        });
        ui.add_space(6.0);

        Frame::new()
            .fill(Color32::from_rgb(20, 24, 32))
            .corner_radius(CornerRadius::same(6))
            .inner_margin(Margin::symmetric(8, 6))
            .show(ui, |ui| {
                ui.horizontal(|ui| {
                    ui.allocate_ui_with_layout(
                        Vec2::new(88.0, 18.0),
                        Layout::left_to_right(Align::Center),
                        |ui| {
                            ui.label(RichText::new("field").size(11.0).color(th.muted));
                        },
                    );
                    ui.allocate_ui_with_layout(
                        Vec2::new(100.0, 18.0),
                        Layout::left_to_right(Align::Center),
                        |ui| {
                            ui.label(RichText::new("ours").size(11.0).strong().color(th.text));
                        },
                    );
                    ui.allocate_ui_with_layout(
                        Vec2::new(20.0, 18.0),
                        Layout::left_to_right(Align::Center),
                        |_| {},
                    );
                    ui.allocate_ui_with_layout(
                        Vec2::new(100.0, 18.0),
                        Layout::left_to_right(Align::Center),
                        |ui| {
                            ui.label(
                                RichText::new("oci").size(11.0).strong().color(th.text),
                            );
                        },
                    );
                });
            });
        ui.add_space(4.0);

        for d in diffs {
            let (bg, mark, mark_c) = match d.side {
                DiffSide::Match => (th.panel2, "=", th.ok),
                DiffSide::Mismatch => (th.wash(th.danger, 20), "≠", th.danger),
                DiffSide::ExtOnly => (th.wash(th.accent, 16), "+", th.accent),
                DiffSide::OursOnly => (th.panel2, "·", th.muted),
            };
            Frame::new()
                .fill(bg)
                .corner_radius(CornerRadius::same(5))
                .inner_margin(Margin::symmetric(6, 4))
                .show(ui, |ui| {
                    ui.horizontal(|ui| {
                        ui.allocate_ui_with_layout(
                            Vec2::new(88.0, 20.0),
                            Layout::left_to_right(Align::Center),
                            |ui| {
                                ui.label(RichText::new(d.label).size(11.5).color(th.muted));
                            },
                        );
                        ui.allocate_ui_with_layout(
                            Vec2::new(100.0, 20.0),
                            Layout::left_to_right(Align::Center),
                            |ui| {
                                let c = match d.side {
                                    DiffSide::ExtOnly => th.muted,
                                    _ => th.text,
                                };
                                ui.label(
                                    RichText::new(dash(&d.ours))
                                        .size(12.5)
                                        .strong()
                                        .color(c),
                                );
                            },
                        );
                        ui.allocate_ui_with_layout(
                            Vec2::new(20.0, 20.0),
                            Layout::left_to_right(Align::Center),
                            |ui| {
                                ui.label(
                                    RichText::new(mark).size(11.0).strong().color(mark_c),
                                );
                            },
                        );
                        ui.allocate_ui_with_layout(
                            Vec2::new(100.0, 20.0),
                            Layout::left_to_right(Align::Center),
                            |ui| {
                                let c = match d.side {
                                    DiffSide::OursOnly => th.muted,
                                    DiffSide::ExtOnly => th.accent,
                                    _ => th.text,
                                };
                                ui.label(
                                    RichText::new(dash(&d.ext))
                                        .size(12.5)
                                        .strong()
                                        .color(c),
                                );
                            },
                        );
                    });
                });
            ui.add_space(2.0);
        }
    });
}

fn legend_chip(ui: &mut Ui, th: &Theme, text: &str, color: Color32) {
    ui.horizontal(|ui| {
        icons::status_dot(ui, color);
        ui.label(RichText::new(text).size(11.0).color(th.muted));
    });
    ui.add_space(6.0);
}

fn kv_row(ui: &mut Ui, th: &Theme, k: &str, v: &str) {
    if v.is_empty() {
        return;
    }
    ui.horizontal(|ui| {
        ui.label(
            RichText::new(k)
                .size(14.0)
                .color(th.muted)
                .extra_letter_spacing(0.2),
        );
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            ui.label(RichText::new(v).size(15.0).strong().color(th.text));
        });
    });
    ui.add_space(4.0);
}

fn dash(s: &str) -> &str {
    if s.is_empty() {
        "-"
    } else {
        s
    }
}
