mod at_bus;
mod diag_codes;
mod enrich;
mod icons;
mod lab;
mod live;
mod model;
mod scanner;
mod theme;

use eframe::egui::{self, Color32, CornerRadius, Frame, Margin, RichText, Sense, Ui, Vec2};
use egui_extras::{Column, TableBuilder};
use lab::LabState;
use live::LiveState;
use model::{format_scan_time_rel, Document, FlatTower, Neighbor, Rat, Stats};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use theme::Theme;

#[derive(Clone, Copy, PartialEq, Eq)]
enum AppSection {
    LiveScan,
    Lab,
    Dumps,
}

fn main() -> eframe::Result<()> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([1500.0, 920.0])
            .with_min_inner_size([1100.0, 700.0])
            .with_title("Tower Observer"),
        ..Default::default()
    };
    eframe::run_native(
        "Tower Observer",
        options,
        Box::new(|cc| Ok(Box::new(TowerApp::new(cc)))),
    )
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum SortKey {
    LastSeen,
    Key,
    Cid,
    Channel,
    Rxl,
    Band,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ViewMode {
    Towers,
    Stats,
}

struct Filters {
    query: String,
    show_gsm: bool,
    show_lte: bool,
    show_wcdma: bool,
    show_nr: bool,
    serving_only: bool,
    with_cid_only: bool,
    with_neighbors_only: bool,
    band: String,
    plmn: String,
    sort: SortKey,
    sort_desc: bool,
}

impl Default for Filters {
    fn default() -> Self {
        Self {
            query: String::new(),
            show_gsm: true,
            show_lte: true,
            show_wcdma: true,
            show_nr: true,
            serving_only: false,
            with_cid_only: false,
            with_neighbors_only: false,
            band: String::new(),
            plmn: String::new(),
            sort: SortKey::LastSeen,
            sort_desc: true,
        }
    }
}

/// One open dump document (shared between panes by index).
struct Doc {
    title: String,
    path: PathBuf,
    doc: Document,
    flat: Vec<FlatTower>,
    filters: Filters,
    filtered_ix: Vec<usize>,
    selected: Option<usize>,
    view: ViewMode,
    /// Detail panel height fraction of remaining space (0.25..0.7).
    detail_frac: f32,
}

impl Doc {
    fn open(path: PathBuf) -> Result<Self, String> {
        let document = Document::load(&path)?;
        let title = tab_title(&path, &document);
        let flat = document.flatten();
        let mut d = Self {
            title,
            path,
            doc: document,
            flat,
            filters: Filters::default(),
            filtered_ix: Vec::new(),
            selected: None,
            view: ViewMode::Towers,
            detail_frac: 0.38,
        };
        d.refilter();
        Ok(d)
    }

    fn refilter(&mut self) {
        let q = self.filters.query.to_lowercase();
        let mut ix: Vec<usize> = self
            .flat
            .iter()
            .enumerate()
            .filter(|(_, ft)| {
                let f = &self.filters;
                let rat_ok = match ft.rat {
                    Rat::Gsm => f.show_gsm,
                    Rat::Lte => f.show_lte,
                    Rat::Wcdma => f.show_wcdma,
                    Rat::Nr => f.show_nr,
                };
                if !rat_ok {
                    return false;
                }
                if f.serving_only && !ft.tower.is_serving() {
                    return false;
                }
                if f.with_cid_only && ft.tower.get_id("cid").is_empty() {
                    return false;
                }
                if f.with_neighbors_only && ft.tower.neighbor_count() == 0 {
                    return false;
                }
                if !f.band.is_empty() && ft.tower.band() != f.band {
                    return false;
                }
                if !f.plmn.is_empty() && ft.tower.plmn() != f.plmn {
                    return false;
                }
                if q.is_empty() {
                    return true;
                }
                let hay = format!(
                    "{} {} {} {} {} {} {} {}",
                    ft.tower.key,
                    ft.tower.get_id("cid"),
                    ft.tower.plmn(),
                    ft.tower.lac_or_tac(),
                    ft.tower.channel(),
                    ft.tower.cell_code(),
                    ft.tower.band(),
                    ft.rat.as_str()
                )
                .to_lowercase();
                hay.contains(&q)
            })
            .map(|(i, _)| i)
            .collect();

        let sort = self.filters.sort;
        let desc = self.filters.sort_desc;
        ix.sort_by(|&a, &b| {
            let ta = &self.flat[a].tower;
            let tb = &self.flat[b].tower;
            let ord = match sort {
                SortKey::LastSeen => ta.meta.last_seen.cmp(&tb.meta.last_seen),
                SortKey::Key => ta.key.cmp(&tb.key),
                SortKey::Cid => ta.get_id("cid").cmp(tb.get_id("cid")),
                SortKey::Channel => ta.channel().cmp(tb.channel()),
                SortKey::Rxl => parse_i32(ta.rxl()).cmp(&parse_i32(tb.rxl())),
                SortKey::Band => ta.band().cmp(tb.band()),
            };
            if desc {
                ord.reverse()
            } else {
                ord
            }
        });

        if let Some(sel) = self.selected {
            if !ix.contains(&sel) {
                self.selected = ix.first().copied();
            }
        }
        self.filtered_ix = ix;
    }

    fn bands(&self) -> Vec<String> {
        let mut s = std::collections::BTreeSet::new();
        for ft in &self.flat {
            if !ft.tower.band().is_empty() {
                s.insert(ft.tower.band().to_string());
            }
        }
        s.into_iter().collect()
    }

    fn plmns(&self) -> Vec<String> {
        let mut s = std::collections::BTreeSet::new();
        for ft in &self.flat {
            if !ft.tower.plmn().is_empty() {
                s.insert(ft.tower.plmn().to_string());
            }
        }
        s.into_iter().collect()
    }
}

struct FullMatch {
    rat: Rat,
    plmn: String,
    lac: String,
    cid: String,
    channel: String,
    code: String,
    left_title: String,
    right_title: String,
    left_key: String,
    right_key: String,
    left_rxl: String,
    right_rxl: String,
    left_last: String,
    right_last: String,
    left_serving: bool,
    right_serving: bool,
}

struct TowerApp {
    theme: Theme,
    section: AppSection,
    live: LiveState,
    lab: LabState,
    docs: Vec<Doc>,
    /// Document shown in the primary (left / only) pane.
    left_doc: usize,
    /// Document shown in the right pane when split.
    right_doc: usize,
    split: bool,
    split_ratio: f32,
    /// Full-match diff view (separate from dump panes).
    show_diff: bool,
    diff_rows: Vec<FullMatch>,
    diff_selected: Option<usize>,
    /// Blocks SidePanel horizontal resize while table/detail splitter is dragged.
    v_resize_lock: bool,
    status: String,
}

impl TowerApp {
    fn new(cc: &eframe::CreationContext<'_>) -> Self {
        let mut fonts = egui::FontDefinitions::default();
        egui_phosphor::add_to_fonts(&mut fonts, egui_phosphor::Variant::Regular);
        cc.egui_ctx.set_fonts(fonts);

        let theme = Theme::ops();
        apply_style(&cc.egui_ctx, &theme);

        let mut app = Self {
            theme,
            section: AppSection::LiveScan,
            live: LiveState::new(),
            lab: LabState::new(),
            docs: Vec::new(),
            left_doc: 0,
            right_doc: 0,
            split: false,
            split_ratio: 0.5,
            show_diff: false,
            diff_rows: Vec::new(),
            diff_selected: None,
            v_resize_lock: false,
            status: String::new(),
        };

        let root =
            PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../scan_dumps/android_vlad_20260729");
        for p in [root.join("ours_towers.json"), root.join("vlad_towers.json")] {
            if p.exists() {
                let _ = app.open_doc(p);
            }
        }
        if app.docs.len() >= 2 {
            app.split = true;
            app.left_doc = 0;
            app.right_doc = 1;
            app.status = "Split: ours | vlad".into();
        } else if app.docs.len() == 1 {
            app.status = format!("Loaded {}", app.docs[0].title);
        } else {
            app.status = "Open JSON dumps to compare".into();
        }
        app
    }

    fn open_doc(&mut self, path: PathBuf) -> Result<(), String> {
        if let Some(i) = self.docs.iter().position(|d| d.path == path) {
            self.left_doc = i;
            return Ok(());
        }
        let doc = Doc::open(path)?;
        self.docs.push(doc);
        let i = self.docs.len() - 1;
        self.left_doc = i;
        if self.docs.len() == 2 {
            self.split = true;
            self.right_doc = if i == 0 { 1 } else { 0 };
        }
        self.status = format!("Opened {}", self.docs[i].title);
        Ok(())
    }

    fn close_doc(&mut self, ix: usize) {
        if ix >= self.docs.len() {
            return;
        }
        self.docs.remove(ix);
        let n = self.docs.len();
        if n == 0 {
            self.left_doc = 0;
            self.right_doc = 0;
            self.split = false;
            self.show_diff = false;
            self.diff_rows.clear();
            return;
        }
        if self.left_doc >= n {
            self.left_doc = n - 1;
        } else if self.left_doc > ix {
            self.left_doc -= 1;
        }
        if self.right_doc >= n {
            self.right_doc = n - 1;
        } else if self.right_doc > ix {
            self.right_doc -= 1;
        }
        if n < 2 {
            self.split = false;
            self.show_diff = false;
            self.diff_rows.clear();
        }
    }

    fn rebuild_diff(&mut self) {
        self.diff_rows.clear();
        self.diff_selected = None;
        if self.docs.len() < 2 {
            self.status = "Need 2 dumps for match diff".into();
            self.show_diff = false;
            return;
        }
        let a = self.left_doc.min(self.docs.len() - 1);
        let mut b = self.right_doc.min(self.docs.len() - 1);
        if a == b {
            b = (a + 1) % self.docs.len();
        }
        let left = &self.docs[a];
        let right = &self.docs[b];
        let mut right_map: BTreeMap<String, &FlatTower> = BTreeMap::new();
        for ft in &right.flat {
            right_map.insert(ft.tower.full_match_key(ft.rat), ft);
        }
        for ft in &left.flat {
            let k = ft.tower.full_match_key(ft.rat);
            let Some(other) = right_map.get(&k) else {
                continue;
            };
            self.diff_rows.push(FullMatch {
                rat: ft.rat,
                plmn: ft.tower.plmn().to_string(),
                lac: ft.tower.lac_or_tac().to_string(),
                cid: ft.tower.get_id("cid").to_string(),
                channel: ft.tower.channel().to_string(),
                code: ft.tower.cell_code().to_string(),
                left_title: left.title.clone(),
                right_title: right.title.clone(),
                left_key: ft.tower.key.clone(),
                right_key: other.tower.key.clone(),
                left_rxl: ft.tower.rxl().to_string(),
                right_rxl: other.tower.rxl().to_string(),
                left_last: ft.tower.meta.last_seen.clone(),
                right_last: other.tower.meta.last_seen.clone(),
                left_serving: ft.tower.is_serving(),
                right_serving: other.tower.is_serving(),
            });
        }
        self.diff_rows.sort_by(|x, y| {
            y.left_last
                .cmp(&x.left_last)
                .then_with(|| x.cid.cmp(&y.cid))
        });
        self.show_diff = true;
        self.status = format!(
            "Full matches {} ({} ↔ {}) · key=RAT|PLMN|LAC|CID|CH|CODE",
            self.diff_rows.len(),
            left.title,
            right.title
        );
    }
}

impl eframe::App for TowerApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        let th = self.theme;
        let dt = ctx.input(|i| i.stable_dt).min(0.05);
        self.lab.tick();
        let on_live = self.section == AppSection::LiveScan;
        let on_lab = self.section == AppSection::Lab;
        if on_live || on_lab {
            self.live.tick(dt);
            if self.live.wants_continuous_repaint() {
                ctx.request_repaint();
            } else if self.lab.wants_repaint() || self.live.wants_log_repaint() {
                ctx.request_repaint_after(std::time::Duration::from_millis(180));
            } else if self.live.watching {
                ctx.request_repaint_after(std::time::Duration::from_millis(450));
            } else {
                ctx.request_repaint_after(std::time::Duration::from_millis(1200));
            }
        } else if self.lab.wants_repaint() {
            ctx.request_repaint_after(std::time::Duration::from_millis(180));
        }

        // ── left nav ──
        egui::SidePanel::left("app_nav")
            .exact_width(188.0)
            .resizable(false)
            .show_separator_line(true)
            .frame(
                Frame::new()
                    .fill(th.panel)
                    .inner_margin(Margin::symmetric(14, 18)),
            )
            .show(ctx, |ui| {
                ui.label(
                    RichText::new("Tower")
                        .strong()
                        .size(18.0)
                        .color(th.text),
                );
                ui.label(
                    RichText::new("Observer")
                        .size(13.0)
                        .color(th.accent),
                );
                ui.add_space(22.0);

                let nav_btn = |ui: &mut Ui, active: bool, icon: &str, label: &str, hint: &str| -> bool {
                    let fill = if active {
                        th.wash(th.accent, 28)
                    } else {
                        Color32::TRANSPARENT
                    };
                    let col = if active { th.accent } else { th.muted };
                    let stroke = if active {
                        egui::Stroke::new(1.0, th.wash(th.accent, 90))
                    } else {
                        egui::Stroke::NONE
                    };
                    ui.add(
                        egui::Button::new(
                            RichText::new(format!("{icon}  {label}"))
                                .strong()
                                .color(col)
                                .size(15.0),
                        )
                        .fill(fill)
                        .stroke(stroke)
                        .corner_radius(CornerRadius::same(10))
                        .min_size(Vec2::new(160.0, 44.0)),
                    )
                    .on_hover_text(hint)
                    .clicked()
                };

                if nav_btn(
                    ui,
                    self.section == AppSection::LiveScan,
                    icons::BROADCAST,
                    "Live Scan",
                    "Poll live_scanner survey JSON (eNB tree)",
                ) {
                    self.section = AppSection::LiveScan;
                    self.show_diff = false;
                }
                ui.add_space(8.0);
                if nav_btn(
                    ui,
                    self.section == AppSection::Lab,
                    icons::FLASK,
                    "Lab",
                    "Hand AT, camp caught towers, watch DIAG codes",
                ) {
                    self.section = AppSection::Lab;
                    self.show_diff = false;
                }
                ui.add_space(8.0);
                if nav_btn(
                    ui,
                    self.section == AppSection::Dumps,
                    icons::FOLDER_OPEN,
                    "Dumps",
                    "Browse / compare qcom.towers JSON dumps",
                ) {
                    self.section = AppSection::Dumps;
                }

                ui.with_layout(egui::Layout::bottom_up(egui::Align::LEFT), |ui| {
                    ui.label(RichText::new(&self.status).color(th.muted).size(11.0));
                    ui.add_space(6.0);
                    ui.label(
                        RichText::new(match self.section {
                            AppSection::LiveScan => "LIVE",
                            AppSection::Lab => "LAB",
                            AppSection::Dumps => "DUMPS",
                        })
                        .strong()
                        .color(th.accent)
                        .size(11.0),
                    );
                });
            });

        if self.section == AppSection::LiveScan {
            egui::CentralPanel::default()
                .frame(
                    Frame::new()
                        .fill(th.bg)
                        .inner_margin(Margin::symmetric(22, 18)),
                )
                .show(ctx, |ui| {
                    live::render_live(ui, &th, &mut self.live);
                });
            return;
        }

        if self.section == AppSection::Lab {
            egui::CentralPanel::default()
                .frame(
                    Frame::new()
                        .fill(th.bg)
                        .inner_margin(Margin::symmetric(22, 18)),
                )
                .show(ctx, |ui| {
                    lab::render_lab(ui, &th, &mut self.lab, &mut self.live);
                });
            return;
        }

        // ── dumps: top chrome ──
        egui::TopBottomPanel::top("chrome")
            .exact_height(40.0)
            .show_separator_line(false)
            .frame(
                Frame::new()
                    .fill(th.panel)
                    .inner_margin(Margin::symmetric(12, 6)),
            )
            .show(ctx, |ui| {
                ui.horizontal_centered(|ui| {
                    ui.label(
                        RichText::new(format!("{}  DUMPS", icons::FOLDER_OPEN))
                            .strong()
                            .size(15.0)
                            .color(th.accent),
                    );
                    ui.separator();
                    if ui
                        .selectable_label(
                            self.split && !self.show_diff,
                            RichText::new(format!("{}  Split", icons::LIST))
                                .size(13.0),
                        )
                        .clicked()
                    {
                        self.show_diff = false;
                        self.split = !self.split;
                        if self.split && self.docs.len() >= 2 && self.left_doc == self.right_doc {
                            self.right_doc = (self.left_doc + 1) % self.docs.len();
                        }
                    }
                    if ui
                        .selectable_label(
                            self.show_diff,
                            RichText::new(format!("{}  Matches", icons::CHECK))
                                .size(13.0),
                        )
                        .on_hover_text(
                            "RAT|PLMN|LAC|CID|channel|code exact matches between left & right dumps",
                        )
                        .clicked()
                    {
                        if self.show_diff {
                            self.show_diff = false;
                        } else {
                            self.rebuild_diff();
                        }
                    }
                    if ui
                        .button(
                            RichText::new(format!("{}  Open", icons::FOLDER_OPEN)).size(13.0),
                        )
                        .clicked()
                    {
                        if let Some(p) = rfd::FileDialog::new()
                            .add_filter("json", &["json"])
                            .pick_file()
                        {
                            if let Err(e) = self.open_doc(p) {
                                self.status = e;
                            }
                        }
                    }
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        ui.label(RichText::new(&self.status).color(th.muted).size(12.0));
                    });
                });
            });

        // ── open documents strip ──
        egui::TopBottomPanel::top("docs")
            .exact_height(34.0)
            .show_separator_line(false)
            .frame(
                Frame::new()
                    .fill(th.bg)
                    .inner_margin(Margin::symmetric(8, 4)),
            )
            .show(ctx, |ui| {
                ui.horizontal(|ui| {
                    let mut close = None;
                    for (i, d) in self.docs.iter().enumerate() {
                        let in_left = self.left_doc == i;
                        let in_right = self.split && self.right_doc == i;
                        let mark = if in_left && in_right {
                            " L+R "
                        } else if in_left {
                            " L "
                        } else if in_right {
                            " R "
                        } else {
                            ""
                        };
                        let active = in_left || in_right;
                        let resp = ui.add(
                            egui::Button::new(
                                RichText::new(format!("{}{} ({})", d.title, mark, d.flat.len()))
                                    .color(if active { th.accent } else { th.text })
                                    .size(12.0),
                            )
                            .fill(if active { th.panel2 } else { th.panel }),
                        );
                        if resp.clicked() {
                            if ui.input(|i| i.modifiers.shift) && self.split {
                                self.right_doc = i;
                            } else {
                                self.left_doc = i;
                            }
                        }
                        if resp.secondary_clicked() && self.split {
                            self.right_doc = i;
                        }
                        // close button next to tab
                        if ui
                            .add(
                                egui::Button::new(
                                    RichText::new(icons::X_CIRCLE).size(14.0).color(th.muted),
                                )
                                .fill(Color32::TRANSPARENT)
                                .min_size(Vec2::new(18.0, 18.0)),
                            )
                            .clicked()
                        {
                            close = Some(i);
                        }
                        ui.add_space(4.0);
                    }
                    if let Some(i) = close {
                        self.close_doc(i);
                    }
                    if self.split {
                        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                            ui.label(
                                RichText::new("click = left · Shift/RMB = right pane")
                                    .color(th.muted)
                                    .size(11.0),
                            );
                        });
                    }
                });
            });

        if self.docs.is_empty() {
            egui::CentralPanel::default()
                .frame(Frame::new().fill(th.bg).inner_margin(40.0))
                .show(ctx, |ui| {
                    ui.vertical_centered(|ui| {
                        ui.add_space(180.0);
                        ui.label(
                            RichText::new("Open ours_towers.json and vlad_towers.json")
                                .size(18.0)
                                .color(th.muted),
                        );
                        ui.label(
                            RichText::new("Then enable Split view to compare side-by-side")
                                .size(13.0)
                                .color(th.muted),
                        );
                    });
                });
            return;
        }

        // clamp indices
        self.left_doc = self.left_doc.min(self.docs.len() - 1);
        self.right_doc = self.right_doc.min(self.docs.len() - 1);

        if self.show_diff {
            egui::CentralPanel::default()
                .frame(
                    Frame::new()
                        .fill(th.bg)
                        .inner_margin(Margin::same(8)),
                )
                .show(ctx, |ui| {
                    render_diff_view(ui, th, &self.diff_rows, &mut self.diff_selected);
                });
            return;
        }

        let pane_frame_style = Frame::new()
            .fill(th.panel)
            .inner_margin(Margin::same(8));

        // Zed-style: native egui SidePanel + CentralPanel
        if self.split && self.docs.len() >= 2 {
            let screen_w = ctx.screen_rect().width();
            let default_left =
                (screen_w * self.split_ratio.clamp(0.25, 0.75)).clamp(320.0, screen_w - 320.0);

            // Use previous-frame lock so SidePanel doesn't steal the vertical drag.
            let allow_h_resize = !self.v_resize_lock;
            self.v_resize_lock = false;

            let left = egui::SidePanel::left("split_left")
                .resizable(allow_h_resize)
                .default_width(default_left)
                .width_range(280.0..=(screen_w - 280.0).max(400.0))
                .show_separator_line(true)
                .frame(pane_frame_style)
                .show(ctx, |ui| {
                    ui.label(RichText::new("LEFT").strong().color(th.muted).size(10.0));
                    ui.add_space(2.0);
                    if let Some(new_ix) = render_doc_pane(
                        ui,
                        th,
                        &mut self.docs,
                        self.left_doc,
                        0,
                        &mut self.v_resize_lock,
                    ) {
                        self.left_doc = new_ix;
                    }
                });

            let left_w = left.response.rect.width();
            if left_w > 0.0 && screen_w > 0.0 && allow_h_resize {
                self.split_ratio = (left_w / screen_w).clamp(0.2, 0.8);
            }

            egui::CentralPanel::default()
                .frame(
                    Frame::new()
                        .fill(th.panel)
                        .inner_margin(Margin::same(8)),
                )
                .show(ctx, |ui| {
                    ui.label(RichText::new("RIGHT").strong().color(th.muted).size(10.0));
                    ui.add_space(2.0);
                    if let Some(new_ix) = render_doc_pane(
                        ui,
                        th,
                        &mut self.docs,
                        self.right_doc,
                        1,
                        &mut self.v_resize_lock,
                    ) {
                        self.right_doc = new_ix;
                    }
                });
        } else {
            egui::CentralPanel::default()
                .frame(
                    Frame::new()
                        .fill(th.bg)
                        .inner_margin(Margin::same(8)),
                )
                .show(ctx, |ui| {
                    Frame::new()
                        .fill(th.panel)
                        .stroke(egui::Stroke::new(1.0, th.stroke))
                        .corner_radius(CornerRadius::same(6))
                        .inner_margin(Margin::same(8))
                        .show(ui, |ui| {
                            if let Some(new_ix) = render_doc_pane(
                                ui,
                                th,
                                &mut self.docs,
                                self.left_doc,
                                0,
                                &mut self.v_resize_lock,
                            ) {
                                self.left_doc = new_ix;
                            }
                        });
                });
        }
    }
}

/// Renders one document pane. Returns `Some(doc_index)` if user picked another dump.
fn render_doc_pane(
    ui: &mut Ui,
    th: Theme,
    docs: &mut [Doc],
    ix: usize,
    pane_id: u8,
    v_resize_lock: &mut bool,
) -> Option<usize> {
    if ix >= docs.len() {
        return None;
    }

    let titles: Vec<String> = docs.iter().map(|d| d.title.clone()).collect();
    let mut switch_to = None;
    let mut dirty = false;

    ui.horizontal(|ui| {
        egui::ComboBox::from_id_salt(("doc_pick", pane_id))
            .selected_text(&titles[ix])
            .width(160.0)
            .show_ui(ui, |ui| {
                for (i, t) in titles.iter().enumerate() {
                    if ui.selectable_label(i == ix, t).clicked() {
                        switch_to = Some(i);
                    }
                }
            });

        let doc = &mut docs[ix];
        if ui
            .selectable_label(doc.view == ViewMode::Towers, "Towers")
            .clicked()
        {
            doc.view = ViewMode::Towers;
        }
        if ui
            .selectable_label(doc.view == ViewMode::Stats, "Stats")
            .clicked()
        {
            doc.view = ViewMode::Stats;
        }
        ui.label(
            RichText::new(format!("{} / {}", doc.filtered_ix.len(), doc.flat.len()))
                .color(th.muted)
                .size(11.0),
        );
    });

    // Compact filter row
    {
        let doc = &mut docs[ix];
        let bands = doc.bands();
        let plmns = doc.plmns();
        ui.add_space(4.0);
        ui.horizontal_wrapped(|ui| {
            let te = ui.add(
                egui::TextEdit::singleline(&mut doc.filters.query)
                    .hint_text("search…")
                    .desired_width(120.0)
                    .id_source(("q", pane_id)),
            );
            if te.changed() {
                dirty = true;
            }
            dirty |= chip(ui, "GSM", th.gsm, &mut doc.filters.show_gsm);
            dirty |= chip(ui, "LTE", th.lte, &mut doc.filters.show_lte);
            dirty |= chip(ui, "UMTS", th.wcdma, &mut doc.filters.show_wcdma);
            dirty |= ui.checkbox(&mut doc.filters.with_cid_only, "CID").changed();
            dirty |= ui.checkbox(&mut doc.filters.serving_only, "Srv").changed();
            dirty |= ui
                .checkbox(&mut doc.filters.with_neighbors_only, "NB")
                .changed();

            egui::ComboBox::from_id_salt(("plmn", pane_id))
                .selected_text(if doc.filters.plmn.is_empty() {
                    "PLMN"
                } else {
                    doc.filters.plmn.as_str()
                })
                .width(88.0)
                .show_ui(ui, |ui| {
                    if ui
                        .selectable_label(doc.filters.plmn.is_empty(), "All")
                        .clicked()
                    {
                        doc.filters.plmn.clear();
                        dirty = true;
                    }
                    for p in &plmns {
                        if ui.selectable_label(doc.filters.plmn == *p, p).clicked() {
                            doc.filters.plmn = p.clone();
                            dirty = true;
                        }
                    }
                });

            egui::ComboBox::from_id_salt(("band", pane_id))
                .selected_text(if doc.filters.band.is_empty() {
                    "Band"
                } else {
                    doc.filters.band.as_str()
                })
                .width(96.0)
                .show_ui(ui, |ui| {
                    if ui
                        .selectable_label(doc.filters.band.is_empty(), "All")
                        .clicked()
                    {
                        doc.filters.band.clear();
                        dirty = true;
                    }
                    for b in &bands {
                        if ui.selectable_label(doc.filters.band == *b, b).clicked() {
                            doc.filters.band = b.clone();
                            dirty = true;
                        }
                    }
                });

            egui::ComboBox::from_id_salt(("sort", pane_id))
                .selected_text(sort_label(doc.filters.sort))
                .width(96.0)
                .show_ui(ui, |ui| {
                    for (k, lab) in [
                        (SortKey::LastSeen, "Last seen"),
                        (SortKey::Key, "Key"),
                        (SortKey::Cid, "CID"),
                        (SortKey::Channel, "Channel"),
                        (SortKey::Rxl, "Rxl"),
                        (SortKey::Band, "Band"),
                    ] {
                        if ui.selectable_label(doc.filters.sort == k, lab).clicked() {
                            doc.filters.sort = k;
                            dirty = true;
                        }
                    }
                });
            dirty |= ui.checkbox(&mut doc.filters.sort_desc, "↓").changed();
        });
        if dirty {
            doc.refilter();
        }
    }

    ui.add_space(4.0);
    ui.separator();
    ui.add_space(4.0);

    let view = docs[ix].view;
    match view {
        ViewMode::Stats => {
            egui::ScrollArea::vertical()
                .id_salt(("stats", pane_id))
                .auto_shrink([false, false])
                .show(ui, |ui| {
                    stats_body(ui, th, &docs[ix]);
                });
        }
        ViewMode::Towers => {
            let avail = ui.available_height().max(220.0);
            let frac = docs[ix].detail_frac.clamp(0.22, 0.55);
            let detail_h = (avail * frac).clamp(140.0, avail - 140.0);
            let table_h = (avail - detail_h - 12.0).max(100.0);

            // Full-width table (no horizontal ScrollArea — that fought the SidePanel drag).
            ui.allocate_ui_with_layout(
                Vec2::new(ui.available_width(), table_h),
                egui::Layout::top_down(egui::Align::Min),
                |ui| {
                    ui.set_min_width(ui.available_width());
                    tower_table(ui, th, &mut docs[ix], table_h);
                },
            );

            // Vertical-only splitter — ignore horizontal component, lock SidePanel while active.
            let (handle, hresp) =
                ui.allocate_exact_size(Vec2::new(ui.available_width(), 10.0), Sense::drag());
            ui.painter().rect_filled(
                handle.shrink2(Vec2::new(48.0, 3.0)),
                CornerRadius::same(2),
                if hresp.hovered() || hresp.dragged() {
                    th.accent
                } else {
                    th.stroke
                },
            );
            if hresp.hovered() || hresp.dragged() {
                ui.ctx().set_cursor_icon(egui::CursorIcon::ResizeVertical);
            }
            if hresp.dragged() {
                let d = hresp.drag_delta();
                // Only act on vertical-dominant drags so we don't fight the pane splitter.
                if d.y.abs() >= d.x.abs() {
                    *v_resize_lock = true;
                    let new_table = table_h + d.y;
                    docs[ix].detail_frac = (1.0 - new_table / avail).clamp(0.22, 0.55);
                }
            } else if hresp.drag_stopped() {
                *v_resize_lock = false;
            }

            egui::ScrollArea::vertical()
                .id_salt(("detail", pane_id))
                .auto_shrink([false, false])
                .max_height(detail_h)
                .show(ui, |ui| {
                    ui.set_min_width(ui.available_width());
                    detail_body(ui, th, &docs[ix]);
                });
        }
    }

    switch_to
}

fn tower_table(ui: &mut Ui, th: Theme, doc: &mut Doc, height: f32) {
    let filtered = doc.filtered_ix.clone();
    let mut clicked = None;
    let row_h = 22.0;
    let header_h = 20.0;
    let scroll_h = (height - header_h - 4.0).max(60.0);
    let full_w = ui.available_width();
    ui.set_min_width(full_w);

    TableBuilder::new(ui)
        .striped(true)
        .sense(Sense::click())
        .cell_layout(egui::Layout::left_to_right(egui::Align::Center))
        .min_scrolled_height(scroll_h)
        .max_scroll_height(scroll_h)
        .column(Column::exact(48.0))
        .column(Column::remainder().at_least(100.0))
        .column(Column::exact(78.0))
        .column(Column::exact(78.0))
        .column(Column::exact(56.0))
        .column(Column::exact(48.0))
        .column(Column::exact(48.0))
        .column(Column::exact(150.0))
        .header(header_h, |mut h| {
            for lab in ["RAT", "KEY", "CID", "PLMN", "CH", "CODE", "RXL", "LAST"] {
                h.col(|ui| {
                    ui.label(RichText::new(lab).strong().color(th.muted).size(11.0));
                });
            }
        })
        .body(|body| {
            body.rows(row_h, filtered.len(), |mut row| {
                let fi = filtered[row.index()];
                let ft = &doc.flat[fi];
                let t = &ft.tower;
                if doc.selected == Some(fi) {
                    row.set_selected(true);
                }
                row.col(|ui| {
                    ui.label(
                        RichText::new(ft.rat.as_str())
                            .color(th.rat(ft.rat))
                            .strong()
                            .monospace()
                            .size(12.0),
                    );
                });
                row.col(|ui| {
                    ui.label(
                        RichText::new(&t.key)
                            .monospace()
                            .size(12.0)
                            .color(if t.is_serving() {
                                th.serving
                            } else {
                                th.text
                            }),
                    );
                });
                row.col(|ui| {
                    ui.label(
                        RichText::new(dash(t.get_id("cid")))
                            .monospace()
                            .size(12.0)
                            .color(th.muted),
                    );
                });
                row.col(|ui| {
                    ui.label(
                        RichText::new(dash(t.plmn()))
                            .monospace()
                            .size(12.0)
                            .color(th.muted),
                    );
                });
                row.col(|ui| {
                    ui.label(
                        RichText::new(dash(t.channel()))
                            .monospace()
                            .size(12.0)
                            .color(th.text),
                    );
                });
                row.col(|ui| {
                    ui.label(
                        RichText::new(dash(t.cell_code()))
                            .monospace()
                            .size(12.0)
                            .color(th.muted),
                    );
                });
                row.col(|ui| {
                    ui.label(
                        RichText::new(dash(t.rxl()))
                            .monospace()
                            .size(12.0)
                            .color(th.accent2),
                    );
                });
                row.col(|ui| {
                    let now = &doc.doc.meta.situation_as_of;
                    let last = format_scan_time_rel(&t.meta.last_seen, now);
                    ui.label(
                        RichText::new(dash(&last))
                            .size(11.0)
                            .color(th.muted),
                    );
                });
                if row.response().clicked() {
                    clicked = Some(fi);
                }
            });
        });

    if let Some(i) = clicked {
        doc.selected = Some(i);
    }
}

fn render_diff_view(
    ui: &mut Ui,
    th: Theme,
    rows: &[FullMatch],
    selected: &mut Option<usize>,
) {
    ui.horizontal(|ui| {
        ui.label(
            RichText::new(format!("FULL MATCHES · {}", rows.len()))
                .strong()
                .size(15.0)
                .color(th.accent),
        );
        ui.label(
            RichText::new("identity = RAT | PLMN | LAC/TAC | CID | channel | code")
                .color(th.muted)
                .size(12.0),
        );
    });
    ui.add_space(6.0);

    if rows.is_empty() {
        ui.label(
            RichText::new("No full matches between left and right dumps.")
                .color(th.muted)
                .size(14.0),
        );
        return;
    }

    let avail = ui.available_height().max(200.0);
    let detail_h = 120.0_f32.min(avail * 0.28);
    let table_h = (avail - detail_h - 8.0).max(120.0);
    ui.set_min_width(ui.available_width());

    let mut clicked = None;
    TableBuilder::new(ui)
        .striped(true)
        .sense(Sense::click())
        .cell_layout(egui::Layout::left_to_right(egui::Align::Center))
        .min_scrolled_height(table_h - 24.0)
        .max_scroll_height(table_h - 24.0)
        .column(Column::exact(48.0))
        .column(Column::exact(78.0))
        .column(Column::exact(72.0))
        .column(Column::exact(72.0))
        .column(Column::exact(56.0))
        .column(Column::exact(48.0))
        .column(Column::remainder().at_least(80.0))
        .column(Column::exact(52.0))
        .column(Column::exact(52.0))
        .column(Column::exact(150.0))
        .header(22.0, |mut h| {
            for lab in [
                "RAT", "CID", "PLMN", "LAC", "CH", "CODE", "KEYS", "RXL←", "RXL→", "LAST←",
            ] {
                h.col(|ui| {
                    ui.label(RichText::new(lab).strong().color(th.muted).size(11.0));
                });
            }
        })
        .body(|body| {
            body.rows(24.0, rows.len(), |mut row| {
                let i = row.index();
                let m = &rows[i];
                if *selected == Some(i) {
                    row.set_selected(true);
                }
                row.col(|ui| {
                    ui.label(
                        RichText::new(m.rat.as_str())
                            .color(th.rat(m.rat))
                            .strong()
                            .monospace()
                            .size(12.0),
                    );
                });
                row.col(|ui| {
                    ui.label(RichText::new(&m.cid).monospace().size(12.0));
                });
                row.col(|ui| {
                    ui.label(RichText::new(&m.plmn).monospace().size(12.0).color(th.muted));
                });
                row.col(|ui| {
                    ui.label(RichText::new(&m.lac).monospace().size(12.0).color(th.muted));
                });
                row.col(|ui| {
                    ui.label(RichText::new(&m.channel).monospace().size(12.0));
                });
                row.col(|ui| {
                    ui.label(RichText::new(&m.code).monospace().size(12.0).color(th.muted));
                });
                row.col(|ui| {
                    ui.label(
                        RichText::new(format!("{} · {}", m.left_key, m.right_key))
                            .monospace()
                            .size(11.0)
                            .color(th.text),
                    );
                });
                row.col(|ui| {
                    ui.label(
                        RichText::new(dash(&m.left_rxl))
                            .monospace()
                            .size(12.0)
                            .color(th.accent2),
                    );
                });
                row.col(|ui| {
                    ui.label(
                        RichText::new(dash(&m.right_rxl))
                            .monospace()
                            .size(12.0)
                            .color(th.accent2),
                    );
                });
                row.col(|ui| {
                    ui.label(
                        RichText::new(dash(&m.left_last))
                            .monospace()
                            .size(11.0)
                            .color(th.muted),
                    );
                });
                if row.response().clicked() {
                    clicked = Some(i);
                }
            });
        });

    if let Some(i) = clicked {
        *selected = Some(i);
    }

    ui.add_space(6.0);
    egui::ScrollArea::vertical()
        .id_salt("diff_detail")
        .max_height(detail_h)
        .show(ui, |ui| {
            let Some(i) = *selected else {
                ui.label(RichText::new("Select a match row").color(th.muted));
                return;
            };
            let Some(m) = rows.get(i) else {
                return;
            };
            ui.label(
                RichText::new(format!(
                    "{}  CID {}  {}  LAC/TAC {}  CH {}  CODE {}",
                    m.rat.as_str(),
                    m.cid,
                    m.plmn,
                    m.lac,
                    m.channel,
                    m.code
                ))
                .strong()
                .size(14.0),
            );
            ui.add_space(4.0);
            egui::Grid::new("diff_kv")
                .num_columns(3)
                .spacing([16.0, 4.0])
                .striped(true)
                .show(ui, |ui| {
                    ui.label(RichText::new("").color(th.muted));
                    ui.label(RichText::new(&m.left_title).strong().color(th.accent));
                    ui.label(RichText::new(&m.right_title).strong().color(th.accent2));
                    ui.end_row();
                    ui.label(RichText::new("key").color(th.muted));
                    ui.label(RichText::new(&m.left_key).monospace());
                    ui.label(RichText::new(&m.right_key).monospace());
                    ui.end_row();
                    ui.label(RichText::new("rxl").color(th.muted));
                    ui.label(RichText::new(dash(&m.left_rxl)).monospace());
                    ui.label(RichText::new(dash(&m.right_rxl)).monospace());
                    ui.end_row();
                    ui.label(RichText::new("last_seen").color(th.muted));
                    ui.label(RichText::new(dash(&m.left_last)).monospace().size(11.0));
                    ui.label(RichText::new(dash(&m.right_last)).monospace().size(11.0));
                    ui.end_row();
                    ui.label(RichText::new("serving").color(th.muted));
                    ui.label(if m.left_serving { "1" } else { "0" });
                    ui.label(if m.right_serving { "1" } else { "0" });
                    ui.end_row();
                });
        });
}

fn detail_body(ui: &mut Ui, th: Theme, doc: &Doc) {
    let Some(sel) = doc.selected else {
        ui.label(RichText::new("Select a row").color(th.muted));
        return;
    };
    let Some(ft) = doc.flat.get(sel) else {
        return;
    };
    let t = &ft.tower;

    ui.horizontal(|ui| {
        ui.label(
            RichText::new(ft.rat.as_str())
                .color(th.rat(ft.rat))
                .strong()
                .size(14.0),
        );
        ui.label(RichText::new(&t.key).monospace().size(14.0).color(th.text));
        if t.is_serving() {
            ui.label(RichText::new("SERVING").strong().color(th.serving).size(11.0));
        }
    });
    let now = &doc.doc.meta.situation_as_of;
    ui.label(
        RichText::new(format!(
            "seen {} · last {}",
            dash(&t.meta.seen),
            dash(&format_scan_time_rel(&t.meta.last_seen, now))
        ))
        .color(th.muted)
        .size(11.0),
    );
    ui.add_space(6.0);

    kv_table(ui, th, "Identity", &t.identity, true);
    kv_table(ui, th, "Radio", &t.radio, true);
    kv_table(ui, th, "Signal", &t.signal, false);

    egui::CollapsingHeader::new(
        RichText::new(format!("Neighbors ({})", t.neighbor_count()))
            .strong()
            .color(th.accent2),
    )
    .default_open(false)
    .show(ui, |ui| {
        nb_table(ui, th, "LTE", &t.neighbors.nb_lte);
        nb_table(ui, th, "GSM", &t.neighbors.nb_gsm);
        nb_table(ui, th, "UMTS", &t.neighbors.nb_umts);
        nb_table(ui, th, "NR", &t.neighbors.nb_nr);
        if t.neighbor_count() == 0 {
            ui.label(RichText::new("-").color(th.muted));
        }
    });
}

fn kv_table(ui: &mut Ui, th: Theme, title: &str, map: &BTreeMap<String, String>, open: bool) {
    egui::CollapsingHeader::new(RichText::new(title).strong().color(th.accent2))
        .default_open(open)
        .show(ui, |ui| {
            egui::Grid::new(ui.id().with(title))
                .num_columns(2)
                .spacing([20.0, 3.0])
                .striped(true)
                .show(ui, |ui| {
                    for (k, v) in map {
                        ui.label(RichText::new(k).color(th.muted).size(12.0));
                        ui.label(
                            RichText::new(dash(v))
                                .color(if v.is_empty() { th.muted } else { th.text })
                                .monospace()
                                .size(12.0),
                        );
                        ui.end_row();
                    }
                });
        });
}

fn nb_table(ui: &mut Ui, th: Theme, title: &str, xs: &[Neighbor]) {
    if xs.is_empty() {
        return;
    }
    ui.label(RichText::new(format!("{title} · {}", xs.len())).color(th.muted).size(11.0));
    egui::Grid::new(ui.id().with(title))
        .num_columns(5)
        .spacing([12.0, 2.0])
        .striped(true)
        .show(ui, |ui| {
            for (i, n) in xs.iter().enumerate() {
                let ch = n
                    .radio
                    .get("earfcn")
                    .or_else(|| n.radio.get("uarfcn"))
                    .or_else(|| n.radio.get("arfcn"))
                    .map(|s| s.as_str())
                    .unwrap_or("");
                let code = n
                    .radio
                    .get("pci")
                    .or_else(|| n.radio.get("psc"))
                    .or_else(|| n.radio.get("bsic"))
                    .map(|s| s.as_str())
                    .unwrap_or("");
                let rxl = n.signal.get("rxl").map(|s| s.as_str()).unwrap_or("");
                ui.label(RichText::new(i.to_string()).color(th.muted).monospace().size(11.0));
                ui.label(RichText::new(dash(&n.rat)).color(th.accent).monospace().size(11.0));
                ui.label(RichText::new(dash(ch)).color(th.text).monospace().size(11.0));
                ui.label(RichText::new(dash(code)).color(th.muted).monospace().size(11.0));
                ui.label(RichText::new(dash(rxl)).color(th.accent2).monospace().size(11.0));
                ui.end_row();
            }
        });
}

fn stats_body(ui: &mut Ui, th: Theme, doc: &Doc) {
    let shown: Vec<_> = doc
        .filtered_ix
        .iter()
        .filter_map(|&i| doc.flat.get(i).cloned())
        .collect();
    let st = Stats::from_flat(&shown);
    let all = Stats::from_flat(&doc.flat);

    ui.label(RichText::new("Filtered").strong().color(th.accent));
    ui.horizontal_wrapped(|ui| {
        card(ui, th, "Towers", &st.total.to_string(), th.accent);
        card(ui, th, "Serving", &st.serving.to_string(), th.serving);
        card(ui, th, "GSM", &st.gsm.to_string(), th.gsm);
        card(ui, th, "LTE", &st.lte.to_string(), th.lte);
        card(ui, th, "WCDMA", &st.wcdma.to_string(), th.wcdma);
    });
    ui.add_space(10.0);
    ui.label(RichText::new("File").strong().color(th.muted));
    ui.horizontal_wrapped(|ui| {
        card(ui, th, "Total", &all.total.to_string(), th.text);
        card(ui, th, "Serving", &all.serving.to_string(), th.serving);
    });
    ui.add_space(10.0);
    ui.label(RichText::new("Operators").strong().color(th.accent2));
    bars(ui, th, &st.operators, th.accent);
    ui.add_space(8.0);
    ui.label(RichText::new("Bands").strong().color(th.accent2));
    bars(ui, th, &st.bands, th.accent2);
    ui.add_space(8.0);
    ui.label(
        RichText::new(&doc.doc.meta.source)
            .color(th.muted)
            .monospace()
            .size(11.0),
    );
}

fn apply_style(ctx: &egui::Context, th: &Theme) {
    let mut style = (*ctx.style()).clone();
    style.visuals.dark_mode = true;
    style.visuals.panel_fill = th.panel;
    style.visuals.window_fill = th.panel;
    style.visuals.extreme_bg_color = th.bg;
    style.visuals.faint_bg_color = th.panel2;
    style.visuals.widgets.inactive.bg_fill = th.panel2;
    style.visuals.widgets.inactive.fg_stroke = egui::Stroke::new(1.0, th.muted);
    style.visuals.widgets.hovered.bg_fill = Color32::from_rgb(40, 48, 62);
    style.visuals.widgets.hovered.fg_stroke = egui::Stroke::new(1.0, th.text);
    style.visuals.widgets.active.bg_fill = Color32::from_rgb(48, 58, 74);
    style.visuals.widgets.active.fg_stroke = egui::Stroke::new(1.0, th.text);
    style.visuals.selection.bg_fill = th.wash(th.accent, 45);
    style.visuals.widgets.noninteractive.fg_stroke = egui::Stroke::new(1.0, th.muted);
    style.visuals.override_text_color = Some(th.text);
    style.spacing.item_spacing = Vec2::new(10.0, 8.0);
    style.spacing.button_padding = Vec2::new(12.0, 8.0);
    if let Some(font) = style.text_styles.get_mut(&egui::TextStyle::Body) {
        font.size = 15.0;
    }
    if let Some(font) = style.text_styles.get_mut(&egui::TextStyle::Button) {
        font.size = 14.0;
    }
    if let Some(font) = style.text_styles.get_mut(&egui::TextStyle::Heading) {
        font.size = 24.0;
    }
    if let Some(font) = style.text_styles.get_mut(&egui::TextStyle::Monospace) {
        font.size = 13.5;
    }
    ctx.set_style(style);
}

fn tab_title(path: &Path, doc: &Document) -> String {
    if !doc.meta.origin.is_empty() {
        return doc.meta.origin.clone();
    }
    path.file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("dump")
        .to_string()
}

fn parse_i32(s: &str) -> i32 {
    s.parse().unwrap_or(i32::MIN)
}

fn dash(s: &str) -> &str {
    if s.is_empty() {
        "-"
    } else {
        s
    }
}

fn sort_label(k: SortKey) -> &'static str {
    match k {
        SortKey::LastSeen => "Last seen",
        SortKey::Key => "Key",
        SortKey::Cid => "CID",
        SortKey::Channel => "Channel",
        SortKey::Rxl => "Rxl",
        SortKey::Band => "Band",
    }
}

fn chip(ui: &mut Ui, label: &str, color: Color32, on: &mut bool) -> bool {
    let text = RichText::new(label)
        .color(if *on {
            color
        } else {
            Color32::from_rgb(80, 90, 100)
        })
        .strong()
        .size(11.0);
    ui.toggle_value(on, text).changed()
}

fn card(ui: &mut Ui, th: Theme, label: &str, value: &str, color: Color32) {
    let size = Vec2::new(88.0, 56.0);
    let (rect, _) = ui.allocate_exact_size(size, Sense::hover());
    ui.painter().rect(
        rect,
        CornerRadius::same(6),
        th.panel,
        egui::Stroke::new(1.0, th.stroke),
        egui::StrokeKind::Inside,
    );
    let mut content = ui.new_child(
        egui::UiBuilder::new()
            .max_rect(rect.shrink2(Vec2::new(10.0, 6.0)))
            .layout(egui::Layout::top_down(egui::Align::Min)),
    );
    content.label(RichText::new(label).color(th.muted).size(11.0));
    content.label(RichText::new(value).color(color).strong().size(20.0));
}

fn bars(ui: &mut Ui, th: Theme, map: &BTreeMap<String, usize>, color: Color32) {
    let mut items: Vec<_> = map.iter().collect();
    items.sort_by(|a, b| b.1.cmp(a.1));
    let max = items.first().map(|(_, n)| **n).unwrap_or(1).max(1);
    for (k, n) in items.into_iter().take(12) {
        ui.horizontal(|ui| {
            ui.label(
                RichText::new(format!("{k:16}"))
                    .monospace()
                    .size(11.0)
                    .color(th.text),
            );
            let wmax = (ui.available_width() - 28.0).max(40.0);
            let w = wmax * (*n as f32 / max as f32);
            let (r, _) = ui.allocate_exact_size(Vec2::new(wmax, 10.0), Sense::hover());
            ui.painter()
                .rect_filled(r, CornerRadius::same(2), th.bg);
            let mut f = r;
            f.set_width(w.max(2.0));
            ui.painter()
                .rect_filled(f, CornerRadius::same(2), color.gamma_multiply(0.85));
            ui.label(RichText::new(n.to_string()).color(th.muted).size(11.0));
        });
    }
}
