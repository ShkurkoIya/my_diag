//! Lab — hand-drive AT / camp caught towers / watch DIAG codes.
//! Same visual language as Live Scan; three panes so the bench stays readable.

use crate::at_bus::{self, AtBus, AtEvent, AtStep};
use crate::diag_codes::{self, CATALOG};
use crate::icons;
use crate::live::LiveState;
use crate::model::{FlatTower, Rat};
use crate::scanner::ScannerPhase;
use crate::theme::Theme;
use eframe::egui::{
    self, Align, Color32, CornerRadius, Frame, Key, Layout, Margin, RichText, ScrollArea, Sense,
    Stroke, TextEdit, Ui, Vec2,
};
use egui_extras::{Column, TableBuilder};
use std::collections::VecDeque;

const TRANSCRIPT_CAP: usize = 400;

#[derive(Clone, Copy, PartialEq, Eq)]
enum RightTab {
    Towers,
    Diag,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Danger {
    Safe,
    Careful,
    Hot,
}

struct CatItem {
    group: &'static str,
    label: &'static str,
    hint: &'static str,
    danger: Danger,
    kind: CatKind,
}

enum CatKind {
    Lit(&'static str, u32),
    /// Filled from the target card (EARFCN/PCI/band/PLMN).
    Target(&'static str),
}

enum Confirm {
    Idle,
    Ask {
        title: String,
        body: String,
        job: String,
        steps: Vec<AtStep>,
    },
}

struct Line {
    kind: LineKind,
    text: String,
}

#[derive(Clone, Copy)]
enum LineKind {
    Tx,
    RxOk,
    RxErr,
    Note,
    Err,
}

pub struct LabState {
    at: AtBus,
    port: String,
    force_at: bool,
    invite_nas: bool,
    raw: String,
    raw_timeout_s: f32,
    history: VecDeque<String>,
    hist_i: Option<usize>,
    transcript: Vec<Line>,
    follow: bool,
    search: String,
    hover_hint: String,
    earfcn: String,
    pci: String,
    band: String,
    plmn: String,
    rat: Rat,
    selected_key: String,
    cpsi: String,
    cnmp: String,
    lock: String,
    right: RightTab,
    tower_q: String,
    diag_q: String,
    confirm: Confirm,
    last_err: String,
}

impl LabState {
    pub fn new() -> Self {
        let at = AtBus::new();
        let port = at.port.clone();
        Self {
            at,
            port,
            force_at: false,
            invite_nas: true,
            raw: String::new(),
            raw_timeout_s: 3.0,
            history: VecDeque::new(),
            hist_i: None,
            transcript: Vec::new(),
            follow: true,
            search: String::new(),
            hover_hint: String::new(),
            earfcn: String::new(),
            pci: String::new(),
            band: String::new(),
            plmn: String::new(),
            rat: Rat::Lte,
            selected_key: String::new(),
            cpsi: String::new(),
            cnmp: String::new(),
            lock: String::new(),
            right: RightTab::Towers,
            tower_q: String::new(),
            diag_q: String::new(),
            confirm: Confirm::Idle,
            last_err: String::new(),
        }
    }

    pub fn tick(&mut self) {
        for ev in self.at.poll() {
            match ev {
                AtEvent::Opened(p) => {
                    self.port = p.clone();
                    self.push(LineKind::Note, format!("connected {p}"));
                    self.last_err.clear();
                }
                AtEvent::Closed => self.push(LineKind::Note, "AT disconnected"),
                AtEvent::Busy(_) => {}
                AtEvent::Tx { cmd } => self.push(LineKind::Tx, cmd),
                AtEvent::Rx { cmd, text, ok, ms } => {
                    self.harvest(&cmd, &text);
                    let kind = if ok { LineKind::RxOk } else { LineKind::RxErr };
                    let body = format_rx(&text);
                    self.push(kind, format!("[{ms} ms]\n{body}"));
                }
                AtEvent::Progress {
                    cmd,
                    elapsed_ms,
                    hint,
                } => {
                    let h = if hint.is_empty() {
                        String::new()
                    } else {
                        format!(" · {hint}")
                    };
                    self.push(
                        LineKind::Note,
                        format!("… {cmd}  {}s{h}", elapsed_ms / 1000),
                    );
                }
                AtEvent::Err(e) => {
                    self.last_err = e.clone();
                    self.push(LineKind::Err, e);
                }
                AtEvent::Note(n) => self.push(LineKind::Note, n),
            }
        }
    }

    pub fn wants_repaint(&self) -> bool {
        self.at.busy
    }

    fn push(&mut self, kind: LineKind, text: impl Into<String>) {
        self.transcript.push(Line {
            kind,
            text: text.into(),
        });
        if self.transcript.len() > TRANSCRIPT_CAP {
            let extra = self.transcript.len() - TRANSCRIPT_CAP;
            self.transcript.drain(..extra);
        }
    }

    fn harvest(&mut self, cmd: &str, text: &str) {
        let u = cmd.to_ascii_uppercase();
        if u.contains("CPSI") {
            if let Some(s) = urc(text, "+CPSI:") {
                self.cpsi = s;
            }
        }
        if u.contains("CNMP") {
            if let Some(s) = urc(text, "+CNMP:") {
                self.cnmp = s;
            }
        }
        if u.contains("CCELLCFG?") {
            self.lock = urc(text, "+CCELLCFG:")
                .map(|s| format!("CCELLCFG {s}"))
                .unwrap_or_else(|| first_final(text));
        }
        if u.contains("CLECELL?") {
            let extra = urc(text, "+CLECELL:").unwrap_or_else(|| first_final(text));
            if self.lock.is_empty() {
                self.lock = format!("CLECELL {extra}");
            } else {
                self.lock = format!("{} · CLECELL {extra}", self.lock);
            }
        }
    }

    fn scanner_holds_at(&self, live: &LiveState) -> bool {
        matches!(
            live.scanner.phase,
            ScannerPhase::Starting | ScannerPhase::Running | ScannerPhase::Stopping
        )
    }

    fn can_send(&self, live: &LiveState) -> Result<(), String> {
        if !self.at.connected {
            return Err("connect AT first (ttyUSB2)".into());
        }
        if self.at.busy {
            return Err("AT busy — wait or Cancel".into());
        }
        if self.scanner_holds_at(live) && !self.force_at {
            return Err("Live Scan owns the AT tty — stop it, or tick Force AT".into());
        }
        Ok(())
    }

    fn send_steps(&mut self, live: &LiveState, job: &str, steps: Vec<AtStep>) {
        if let Err(e) = self.can_send(live) {
            self.last_err = e.clone();
            self.push(LineKind::Err, e);
            return;
        }
        self.at.run(job, steps);
    }

    fn send_lit(&mut self, live: &LiveState, cmd: &str, timeout_ms: u32) {
        self.send_steps(
            live,
            cmd,
            vec![AtStep {
                cmd: normalize_at(cmd),
                timeout_ms,
            }],
        );
    }

    fn fill_from_tower(&mut self, ft: &FlatTower) {
        self.rat = ft.rat;
        self.selected_key = ft.tower.key.clone();
        self.earfcn = ft.tower.channel().to_string();
        self.pci = ft.tower.cell_code().to_string();
        let b = ft.tower.band().trim();
        if b.is_empty() {
            if let Ok(ear) = self.earfcn.parse::<u32>() {
                if let Some((n, name)) = lte_band(ear) {
                    self.band = n.to_string();
                    self.push(LineKind::Note, format!("armed {} / {}  {name}", self.earfcn, self.pci));
                }
            }
        } else {
            self.band = b.trim_start_matches('B').to_string();
            self.push(
                LineKind::Note,
                format!("armed {} / {}  B{}", self.earfcn, self.pci, self.band),
            );
        }
        self.plmn = plmn_numeric(ft.tower.plmn());
    }

    fn resolved_band(&self) -> Option<u8> {
        if let Ok(b) = self.band.parse::<u8>() {
            if b > 0 {
                return Some(b);
            }
        }
        let ear: u32 = self.earfcn.parse().ok()?;
        lte_band(ear).map(|(b, _)| b)
    }

    fn camp_steps(&self) -> Result<Vec<AtStep>, String> {
        match self.rat {
            Rat::Wcdma => wcdma_camp_steps(&self.earfcn, &self.pci),
            _ => lte_camp_steps(&self.earfcn, &self.pci, self.resolved_band(), self.invite_nas),
        }
    }

    fn unlock_steps() -> Vec<AtStep> {
        [
            ("AT+CCELLCFG=0", 2000),
            ("AT+CLECELL", 2000),
            ("AT+CLEARFCN", 2000),
            ("AT+CLUCELL", 2000),
            ("AT+CLUARFCN", 2000),
        ]
        .into_iter()
        .map(|(cmd, timeout_ms)| AtStep {
            cmd: cmd.into(),
            timeout_ms,
        })
        .collect()
    }

    fn pulse_steps() -> Vec<AtStep> {
        [
            ("AT", 1500),
            ("AT+CPSI?", 2000),
            ("AT+CNMP?", 2000),
            ("AT+CCELLCFG?", 1500),
            ("AT+CLECELL?", 1500),
        ]
        .into_iter()
        .map(|(cmd, timeout_ms)| AtStep {
            cmd: cmd.into(),
            timeout_ms,
        })
        .collect()
    }
}

fn catalog() -> &'static [CatItem] {
    use CatKind::{Lit, Target};
    use Danger::{Careful, Hot, Safe};
    &[
        CatItem { group: "Pulse", label: "AT", hint: "Ping the tty. Should return OK.", danger: Safe, kind: Lit("AT", 1500) },
        CatItem { group: "Pulse", label: "CPSI?", hint: "Serving passport: RAT, PLMN, TAC/CID, PCI, EARFCN, RSRP. NO SERVICE is normal during RF-lock.", danger: Safe, kind: Lit("AT+CPSI?", 2000) },
        CatItem { group: "Pulse", label: "CNMP?", hint: "Network mode: 38=LTE, 14=WCDMA, 54=both.", danger: Safe, kind: Lit("AT+CNMP?", 2000) },
        CatItem { group: "Pulse", label: "CCELLCFG?", hint: "Qualcomm sticky lock. Reply +CCELLCFG: pci,earfcn", danger: Safe, kind: Lit("AT+CCELLCFG?", 1500) },
        CatItem { group: "Pulse", label: "CLECELL?", hint: "SIMCOM cell lock. Reply +CLECELL: earfcn,pci (order flipped vs CCELLCFG).", danger: Safe, kind: Lit("AT+CLECELL?", 1500) },
        CatItem { group: "Pulse", label: "CLUCELL?", hint: "WCDMA cell lock. NOT IN WCDMA if CNMP≠14.", danger: Safe, kind: Lit("AT+CLUCELL?", 1500) },
        CatItem { group: "Pulse", label: "CNWINFO?", hint: "EGCI without PCI/EARFCN. Don't use as the only cell key.", danger: Safe, kind: Lit("AT+CNWINFO?", 2000) },
        CatItem { group: "RAT", label: "LTE-only", hint: "AT+CNMP=38 — pin LTE. Main 4G survey mode.", danger: Safe, kind: Lit("AT+CNMP=38", 8000) },
        CatItem { group: "RAT", label: "WCDMA-only", hint: "AT+CNMP=14 — required before CLUCELL/CLUARFCN.", danger: Safe, kind: Lit("AT+CNMP=14", 8000) },
        CatItem { group: "RAT", label: "LTE+WCDMA", hint: "AT+CNMP=54 — restore default dual-mode.", danger: Safe, kind: Lit("AT+CNMP=54", 8000) },
        CatItem { group: "RF", label: "CFUN=1", hint: "RF on. Safe recover. Do not use CFUN=1,1 (USB re-enum).", danger: Careful, kind: Lit("AT+CFUN=1", 8000) },
        CatItem { group: "RF", label: "CFUN=4", hint: "Airplane / RF off. Used before FPLMN wipe. Confirm.", danger: Hot, kind: Lit("AT+CFUN=4", 8000) },
        CatItem { group: "RF", label: "CFUN=1,1", hint: "Soft reset — SIM8300 often re-enumerates USB and kills DIAG. Avoid.", danger: Hot, kind: Lit("AT+CFUN=1,1", 10000) },
        CatItem { group: "Operator", label: "COPS?", hint: "Currently selected operator.", danger: Safe, kind: Lit("AT+COPS?", 2000) },
        CatItem { group: "Operator", label: "COPS=?", hint: "Full PLMN scan. Up to ~180 s. Seeds ML1/neighbors. Blocks AT.", danger: Hot, kind: Lit("AT+COPS=?", 180_000) },
        CatItem { group: "Operator", label: "COPS=0", hint: "Auto select. After dual-lock this invites NAS onto the locked cell.", danger: Safe, kind: Lit("AT+COPS=0", 8000) },
        CatItem { group: "Operator", label: "COPS=2", hint: "Deregister. Before a heavy COPS=? / deep search.", danger: Careful, kind: Lit("AT+COPS=2", 8000) },
        CatItem { group: "Operator", label: "COPS=3,2", hint: "Numeric PLMN in replies (MCC MNC digits).", danger: Safe, kind: Lit("AT+COPS=3,2", 2000) },
        CatItem { group: "Operator", label: "Ghost 99999", hint: "COPS=1,2,\"99999\",7 — fake PLMN so the modem keeps searching. LTE AcT.", danger: Hot, kind: Lit("AT+COPS=1,2,\"99999\",7", 60_000) },
        CatItem { group: "Operator", label: "CMSSN pin", hint: "Hard-pin operator from the PLMN field. Complements COPS=1 (which is often ERROR on SIM8300).", danger: Careful, kind: Target("cmssn") },
        CatItem { group: "Operator", label: "CMSSN clear", hint: "AT+CMSSN — drop operator pin.", danger: Safe, kind: Lit("AT+CMSSN", 3000) },
        CatItem { group: "Serving", label: "CMGRMI=4", hint: "LTE gold: serving + intra/inter/CA neighbors. ERROR if CPSI is NO SERVICE — don't spam.", danger: Careful, kind: Lit("AT+CMGRMI=4", 3500) },
        CatItem { group: "Serving", label: "CMGRMI=3", hint: "WCDMA neighbor snapshot (raw; parser is LTE-first).", danger: Careful, kind: Lit("AT+CMGRMI=3", 3500) },
        CatItem { group: "Serving", label: "CEREG=2", hint: "LTE reg URC with TAC/CID. We do not stamp CID onto a PCI without EARFCN.", danger: Safe, kind: Lit("AT+CEREG=2", 2000) },
        CatItem { group: "LTE lock", label: "CLEARFCN", hint: "AT+CLEARFCN=<band>,<earfcn> — freq lock. Band from EARFCN (B1=375, B40≈38xxx).", danger: Careful, kind: Target("clearfcn") },
        CatItem { group: "LTE lock", label: "CCELLCFG", hint: "AT+CCELLCFG=1,<pci>,<earfcn> — Qualcomm sticky. PCI then EARFCN.", danger: Careful, kind: Target("ccellcfg") },
        CatItem { group: "LTE lock", label: "CLECELL", hint: "AT+CLECELL=<earfcn>,<pci> — SIMCOM lock. EARFCN then PCI (flipped!).", danger: Careful, kind: Target("clecell") },
        CatItem { group: "LTE lock", label: "CCELLCFG=0", hint: "Drop Qualcomm lock.", danger: Safe, kind: Lit("AT+CCELLCFG=0", 2000) },
        CatItem { group: "LTE lock", label: "CLECELL clr", hint: "Drop SIMCOM cell lock.", danger: Safe, kind: Lit("AT+CLECELL", 2000) },
        CatItem { group: "LTE lock", label: "CLEARFCN clr", hint: "Drop freq lock.", danger: Safe, kind: Lit("AT+CLEARFCN", 2000) },
        CatItem { group: "WCDMA", label: "CLUCELL", hint: "AT+CLUCELL=<uarfcn>,<psc>. Needs CNMP=14.", danger: Careful, kind: Target("clucell") },
        CatItem { group: "WCDMA", label: "CLUARFCN", hint: "AT+CLUARFCN=<uarfcn> — freq-only assist.", danger: Careful, kind: Target("cluarfcn") },
        CatItem { group: "WCDMA", label: "CLUCELL clr", hint: "Drop 3G cell lock.", danger: Safe, kind: Lit("AT+CLUCELL", 2000) },
        CatItem { group: "WCDMA", label: "CLUARFCN clr", hint: "Drop 3G freq lock.", danger: Safe, kind: Lit("AT+CLUARFCN", 2000) },
        CatItem { group: "SIM", label: "lte_band?", hint: "Read allowed LTE bands (restore on survey exit).", danger: Safe, kind: Lit("AT+CSYSSEL=\"lte_band\"", 3000) },
        CatItem { group: "SIM", label: "w_band?", hint: "Read allowed WCDMA bands.", danger: Safe, kind: Lit("AT+CSYSSEL=\"w_band\"", 3000) },
        CatItem { group: "SIM", label: "FPLMN read", hint: "CRSM READ EF_FPLMN. Empty = FFFFFFFFFFFFFFFFFFFFFFFF.", danger: Safe, kind: Lit("AT+CRSM=176,28539,0,0,12", 3000) },
        CatItem { group: "SIM", label: "FPLMN wipe", hint: "WRITE EF_FPLMN to FF. Use when a PLMN got forbidden and camp dies. Sometimes needs CFUN=4 first.", danger: Hot, kind: Lit("AT+CRSM=214,28539,0,0,12,\"FFFFFFFFFFFFFFFFFFFFFFFF\"", 4000) },
    ]
}

fn lab_card(th: &Theme, outer: Margin) -> Frame {
    Frame::new()
        .fill(th.panel)
        .stroke(Stroke::new(1.0, th.stroke))
        .corner_radius(CornerRadius::same(14))
        .inner_margin(Margin::same(14))
        .outer_margin(outer)
}

pub fn render_lab(ui: &mut Ui, th: &Theme, lab: &mut LabState, live: &mut LiveState) {
    lab.hover_hint.clear();
    confirm_modal(ui, th, lab, live);

    // Real nested panels. A raw horizontal + allocate_ui inherited
    // left-to-right layout and stacked catalog / hint / workbench in one pile.
    egui::TopBottomPanel::top("lab_header")
        .resizable(false)
        .show_separator_line(false)
        .frame(
            Frame::new()
                .fill(th.bg)
                .inner_margin(Margin::ZERO)
                .outer_margin(Margin {
                    bottom: 10,
                    ..Margin::ZERO
                }),
        )
        .show_inside(ui, |ui| {
            ui.set_width(ui.available_width());
            header(ui, th, lab, live);
        });

    egui::SidePanel::left("lab_catalog")
        .exact_width(300.0)
        .resizable(false)
        .show_separator_line(false)
        .frame(lab_card(
            th,
            Margin {
                right: 8,
                ..Margin::ZERO
            },
        ))
        .show_inside(ui, |ui| {
            ui.set_width(ui.available_width());
            palette(ui, th, lab, live);
        });

    egui::SidePanel::right("lab_caught")
        .default_width(440.0)
        .min_width(340.0)
        .max_width(620.0)
        .resizable(true)
        .show_separator_line(false)
        .frame(lab_card(
            th,
            Margin {
                left: 8,
                ..Margin::ZERO
            },
        ))
        .show_inside(ui, |ui| {
            ui.set_width(ui.available_width());
            right_pane(ui, th, lab, live);
        });

    egui::CentralPanel::default()
        .frame(lab_card(th, Margin::ZERO))
        .show_inside(ui, |ui| {
            ui.set_width(ui.available_width());
            workbench(ui, th, lab, live);
        });
}

fn header(ui: &mut Ui, th: &Theme, lab: &mut LabState, live: &LiveState) {
    ui.horizontal(|ui| {
        ui.vertical(|ui| {
            ui.horizontal(|ui| {
                ui.label(RichText::new(icons::FLASK).size(22.0).color(th.accent));
                ui.add_space(8.0);
                ui.label(RichText::new("Lab").strong().size(28.0).color(th.text));
            });
            ui.label(
                RichText::new("Hand-drive the modem. Catch a tower, camp it, watch which DIAG codes move.")
                    .size(14.0)
                    .color(th.muted),
            );
        });
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            let ports = at_bus::list_at_ports();
            let connected = lab.at.connected;
            let col = if connected { th.serving } else { th.muted };
            if connected {
                if ui
                    .add(
                        egui::Button::new(
                            RichText::new(format!("{}  Disconnect", icons::STOP))
                                .strong()
                                .size(14.0)
                                .color(th.danger),
                        )
                        .fill(th.wash(th.danger, 28))
                        .corner_radius(CornerRadius::same(20)),
                    )
                    .clicked()
                {
                    lab.at.close();
                }
            } else if ui
                .add(
                    egui::Button::new(
                        RichText::new(format!("{}  Connect", icons::PLAY))
                            .strong()
                            .size(14.0)
                            .color(th.serving),
                    )
                    .fill(th.wash(th.serving, 28))
                    .corner_radius(CornerRadius::same(20)),
                )
                .clicked()
            {
                lab.at.open(&lab.port);
            }

            egui::ComboBox::from_id_salt("lab_port")
                .selected_text(RichText::new(&lab.port).size(13.0).color(col))
                .width(148.0)
                .show_ui(ui, |ui| {
                    for p in &ports {
                        ui.selectable_value(&mut lab.port, p.clone(), p);
                    }
                });

            ui.label(RichText::new("AT").size(12.0).color(th.muted));
        });
    });

    ui.add_space(8.0);
    ui.horizontal(|ui| {
        let scan_hold = lab.scanner_holds_at(live);
        if scan_hold {
            let txt = if lab.force_at {
                "Live Scan is running — Force AT is on (both will collide)"
            } else {
                "Live Scan holds /dev/ttyUSB2 — stop it to drive AT, or Force AT"
            };
            ui.label(
                RichText::new(format!("{}  {txt}", icons::WARNING))
                    .size(13.0)
                    .color(th.warning),
            );
            ui.checkbox(&mut lab.force_at, "Force AT");
        } else {
            ui.label(
                RichText::new("One AT tty. DIAG stays on ttyUSB0; this pane talks AT only.")
                    .size(13.0)
                    .color(th.muted),
            );
        }
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            if lab.at.busy {
                if ui
                    .add(
                        egui::Button::new(RichText::new("Cancel").color(th.danger).size(13.0))
                            .fill(th.wash(th.danger, 24)),
                    )
                    .clicked()
                {
                    lab.at.cancel();
                }
                ui.label(RichText::new("AT busy").color(th.accent2).size(13.0));
            }
        });
    });

    ui.add_space(8.0);
    ui.horizontal_wrapped(|ui| {
        status_pill(ui, th, "CPSI", if lab.cpsi.is_empty() { "—" } else { &lab.cpsi }, cpsi_color(th, &lab.cpsi));
        status_pill(ui, th, "CNMP", if lab.cnmp.is_empty() { "—" } else { &lab.cnmp }, th.accent);
        status_pill(ui, th, "LOCK", if lab.lock.is_empty() { "—" } else { &lab.lock }, th.accent2);
        if let Some(m) = live.doc.as_ref().map(|d| &d.meta) {
            if !m.cpsi_ok.is_empty() {
                status_pill(ui, th, "scanner cpsi_ok", &m.cpsi_ok, if m.cpsi_ok == "1" { th.serving } else { th.muted });
            }
            if !m.survey_phase.is_empty() {
                status_pill(ui, th, "phase", &m.survey_phase, th.accent);
            }
        }
        if !lab.last_err.is_empty() {
            ui.label(RichText::new(&lab.last_err).size(12.5).color(th.danger));
        }
    });
}

fn status_pill(ui: &mut Ui, th: &Theme, k: &str, v: &str, c: Color32) {
    Frame::new()
        .fill(th.wash(c, 22))
        .stroke(Stroke::new(1.0, th.wash(c, 80)))
        .corner_radius(CornerRadius::same(8))
        .inner_margin(Margin::symmetric(10, 6))
        .show(ui, |ui| {
            ui.horizontal(|ui| {
                ui.label(RichText::new(k).size(11.0).color(th.muted));
                ui.label(
                    RichText::new(v)
                        .size(12.5)
                        .strong()
                        .color(c)
                        .family(egui::FontFamily::Monospace),
                );
            });
        });
}

fn cpsi_color(th: &Theme, s: &str) -> Color32 {
    let u = s.to_ascii_uppercase();
    if u.contains("NO SERVICE") || u.contains("NOSERVICE") {
        th.warning
    } else if u.contains("ONLINE") || u.contains("LTE") {
        th.serving
    } else if s.is_empty() {
        th.muted
    } else {
        th.accent
    }
}

fn palette(ui: &mut Ui, th: &Theme, lab: &mut LabState, live: &LiveState) {
    ui.vertical(|ui| {
        ui.label(RichText::new("AT catalog").strong().size(15.0).color(th.text));
        ui.label(
            RichText::new("Full-width commands. Hint is pinned at the bottom.")
                .size(12.0)
                .color(th.muted),
        );
        ui.add_space(8.0);
        ui.add(
            TextEdit::singleline(&mut lab.search)
                .hint_text("filter  CPSI  lock  COPS…")
                .desired_width(ui.available_width()),
        );
        ui.add_space(8.0);

        let q = lab.search.trim().to_ascii_lowercase();
        let items: Vec<&CatItem> = catalog()
            .iter()
            .filter(|it| {
                if q.is_empty() {
                    return true;
                }
                it.group.to_ascii_lowercase().contains(&q)
                    || it.label.to_ascii_lowercase().contains(&q)
                    || it.hint.to_ascii_lowercase().contains(&q)
            })
            .collect();

        let mut click: Option<usize> = None;
        let scroll_h = (ui.available_height() - 88.0).max(120.0);
        ScrollArea::vertical()
            .id_salt("lab_catalog")
            .max_height(scroll_h)
            .auto_shrink([false, false])
            .show(ui, |ui| {
                ui.set_width(ui.available_width());
                let mut last = "";
                for (idx, it) in items.iter().enumerate() {
                    if it.group != last {
                        last = it.group;
                        ui.add_space(8.0);
                        ui.label(RichText::new(it.group).strong().size(11.5).color(th.muted));
                        ui.add_space(4.0);
                    }
                    let resp = catalog_chip(ui, th, it);
                    if resp.hovered() {
                        lab.hover_hint = format!("{}  —  {}", it.label, it.hint);
                    }
                    if resp.clicked() {
                        click = Some(idx);
                    }
                }
                ui.add_space(6.0);
            });
        if let Some(i) = click {
            if let Some(it) = items.get(i) {
                fire_catalog(lab, live, it);
            }
        }

        ui.add_space(8.0);
        let hint = if lab.hover_hint.is_empty() {
            "Hover a command. Target chips use the EARFCN/PCI card in the middle."
        } else {
            lab.hover_hint.as_str()
        };
        Frame::new()
            .fill(th.panel2)
            .stroke(Stroke::new(1.0, th.stroke))
            .corner_radius(CornerRadius::same(10))
            .inner_margin(Margin::symmetric(10, 8))
            .show(ui, |ui| {
                ui.set_min_height(56.0);
                ui.set_width(ui.available_width());
                ui.label(RichText::new(hint).size(12.0).color(th.text));
            });
    });
}

fn catalog_chip(ui: &mut Ui, th: &Theme, it: &CatItem) -> egui::Response {
    let (fg, fill, bar) = match it.danger {
        Danger::Safe => (th.text, th.panel2, th.stroke),
        Danger::Careful => (th.warning, th.wash(th.warning, 22), th.warning),
        Danger::Hot => (th.danger, th.wash(th.danger, 24), th.danger),
    };
    let w = ui.available_width();
    ui.add(
        egui::Button::new(
            RichText::new(it.label)
                .size(13.0)
                .color(fg)
                .strong(),
        )
        .fill(fill)
        .stroke(Stroke::new(1.0, bar))
        .corner_radius(CornerRadius::same(8))
        .min_size(Vec2::new(w, 32.0)),
    )
    .on_hover_text(it.hint)
}

fn fire_catalog(lab: &mut LabState, live: &LiveState, it: &CatItem) {
    match it.kind {
        CatKind::Lit(cmd, to) => {
            if matches!(it.danger, Danger::Hot) {
                lab.confirm = Confirm::Ask {
                    title: it.label.into(),
                    body: it.hint.into(),
                    job: cmd.into(),
                    steps: vec![AtStep {
                        cmd: cmd.into(),
                        timeout_ms: to,
                    }],
                };
            } else {
                lab.send_lit(live, cmd, to);
            }
        }
        CatKind::Target(kind) => match target_cmd(kind, lab) {
            Ok((job, steps)) => {
                if matches!(it.danger, Danger::Hot) {
                    lab.confirm = Confirm::Ask {
                        title: it.label.into(),
                        body: it.hint.into(),
                        job,
                        steps,
                    };
                } else {
                    lab.send_steps(live, &job, steps);
                }
            }
            Err(e) => {
                lab.last_err = e.clone();
                lab.push(LineKind::Err, e);
            }
        },
    }
}

fn target_cmd(kind: &str, lab: &LabState) -> Result<(String, Vec<AtStep>), String> {
    let ear = lab.earfcn.trim();
    let pci = lab.pci.trim();
    match kind {
        "clearfcn" => {
            let b = lab.resolved_band().ok_or("need band or a known EARFCN")?;
            if ear.is_empty() {
                return Err("need EARFCN".into());
            }
            let cmd = format!("AT+CLEARFCN={b},{ear}");
            Ok((cmd.clone(), vec![AtStep { cmd, timeout_ms: 2500 }]))
        }
        "ccellcfg" => {
            if ear.is_empty() || pci.is_empty() {
                return Err("need EARFCN and PCI".into());
            }
            let cmd = format!("AT+CCELLCFG=1,{pci},{ear}");
            Ok((cmd.clone(), vec![AtStep { cmd, timeout_ms: 2500 }]))
        }
        "clecell" => {
            if ear.is_empty() || pci.is_empty() {
                return Err("need EARFCN and PCI".into());
            }
            let cmd = format!("AT+CLECELL={ear},{pci}");
            Ok((cmd.clone(), vec![AtStep { cmd, timeout_ms: 2500 }]))
        }
        "clucell" => {
            if ear.is_empty() || pci.is_empty() {
                return Err("need UARFCN and PSC".into());
            }
            let cmd = format!("AT+CLUCELL={ear},{pci}");
            Ok((cmd.clone(), vec![AtStep { cmd, timeout_ms: 3000 }]))
        }
        "cluarfcn" => {
            if ear.is_empty() {
                return Err("need UARFCN".into());
            }
            let cmd = format!("AT+CLUARFCN={ear}");
            Ok((cmd.clone(), vec![AtStep { cmd, timeout_ms: 3000 }]))
        }
        "cmssn" => {
            let p = lab.plmn.trim();
            if p.is_empty() {
                return Err("need PLMN (e.g. 25001)".into());
            }
            let cmd = format!("AT+CMSSN={p}");
            Ok((cmd.clone(), vec![AtStep { cmd, timeout_ms: 4000 }]))
        }
        _ => Err("unknown target cmd".into()),
    }
}

fn workbench(ui: &mut Ui, th: &Theme, lab: &mut LabState, live: &LiveState) {
    ui.vertical(|ui| {
        ui.set_width(ui.available_width());
        ui.label(RichText::new("Workbench").strong().size(15.0).color(th.text));
        ui.label(
            RichText::new("Fill from a caught tower, dual-lock camp, or type any AT.")
                .size(12.0)
                .color(th.muted),
        );
        ui.add_space(8.0);

        Frame::new()
            .fill(th.panel2)
            .stroke(Stroke::new(1.0, th.stroke))
            .corner_radius(CornerRadius::same(12))
            .inner_margin(Margin::symmetric(12, 10))
            .show(ui, |ui| {
                ui.set_width(ui.available_width());
                ui.horizontal(|ui| {
                    ui.label(RichText::new("Target").strong().size(13.0).color(th.accent));
                    let rat_c = th.rat(lab.rat);
                    ui.label(
                        RichText::new(lab.rat.as_str())
                            .size(12.0)
                            .color(rat_c)
                            .strong(),
                    );
                    if !lab.selected_key.is_empty() {
                        ui.label(
                            RichText::new(&lab.selected_key)
                                .size(11.5)
                                .color(th.muted)
                                .family(egui::FontFamily::Monospace),
                        );
                    }
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        ui.checkbox(&mut lab.invite_nas, "COPS=0 after camp");
                    });
                });
                ui.add_space(8.0);
                ui.horizontal(|ui| {
                    labeled_field(ui, th, ch_label(lab.rat), &mut lab.earfcn, 96.0);
                    labeled_field(ui, th, phy_label(lab.rat), &mut lab.pci, 72.0);
                    labeled_field(ui, th, "Band", &mut lab.band, 56.0);
                    labeled_field(ui, th, "PLMN", &mut lab.plmn, 80.0);
                    if let Ok(ear) = lab.earfcn.parse::<u32>() {
                        if let Some((b, name)) = lte_band(ear) {
                            ui.vertical(|ui| {
                                ui.label(RichText::new(" ").size(11.0));
                                ui.label(
                                    RichText::new(format!("→ {name} ({b})"))
                                        .size(12.0)
                                        .color(th.muted),
                                );
                            });
                        }
                    }
                });
            });

        ui.add_space(10.0);
        ui.horizontal_wrapped(|ui| {
            let camp_l = match lab.rat {
                Rat::Wcdma => format!("{}  Camp 3G", icons::LOCK_SIMPLE),
                _ => format!("{}  Camp dual-lock", icons::LOCK_SIMPLE),
            };
            if primary(
                ui,
                th,
                &camp_l,
                th.serving,
                "CLEARFCN → CCELLCFG → CLECELL (PCI/EARFCN order differs). No CFUN after lock.",
            )
            .clicked()
            {
                match lab.camp_steps() {
                    Ok(steps) => lab.send_steps(live, "camp", steps),
                    Err(e) => {
                        lab.last_err = e.clone();
                        lab.push(LineKind::Err, e);
                    }
                }
            }
            if primary(
                ui,
                th,
                &format!("{}  Unlock", icons::LOCK_SIMPLE_OPEN),
                th.accent2,
                "Drop CCELLCFG + CLECELL + CLEARFCN + 3G locks.",
            )
            .clicked()
            {
                lab.send_steps(live, "unlock", LabState::unlock_steps());
            }
            if primary(
                ui,
                th,
                &format!("{}  Pulse", icons::LIGHTNING),
                th.accent,
                "AT + CPSI? + CNMP? + lock queries.",
            )
            .clicked()
            {
                lab.send_steps(live, "pulse", LabState::pulse_steps());
            }
        });

        ui.add_space(12.0);
        ui.horizontal(|ui| {
            ui.label(RichText::new(icons::KEYBOARD).size(16.0).color(th.muted));
            ui.label(RichText::new("Raw AT").strong().size(13.0).color(th.text));
            ui.label(
                RichText::new("Enter sends · ↑↓ history")
                    .size(12.0)
                    .color(th.muted),
            );
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                ui.add(
                    egui::Slider::new(&mut lab.raw_timeout_s, 1.0..=180.0)
                        .text("s")
                        .logarithmic(true),
                );
            });
        });
        let raw_resp = ui.add(
            TextEdit::singleline(&mut lab.raw)
                .font(egui::TextStyle::Monospace)
                .hint_text("AT+CPSI?   or   AT+CCELLCFG=1,156,375")
                .desired_width(ui.available_width()),
        );
        if raw_resp.has_focus() {
            let (up, down, enter) = ui.input(|i| {
                (
                    i.key_pressed(Key::ArrowUp),
                    i.key_pressed(Key::ArrowDown),
                    i.key_pressed(Key::Enter),
                )
            });
            if up {
                hist_move(lab, -1);
            }
            if down {
                hist_move(lab, 1);
            }
            if enter {
                send_raw(lab, live);
            }
        }
        ui.horizontal(|ui| {
            if ui.button("Send").clicked() {
                send_raw(lab, live);
            }
            if ui.button("Clear log").clicked() {
                lab.transcript.clear();
            }
            ui.checkbox(&mut lab.follow, "Follow");
        });

        ui.add_space(8.0);
        ui.label(RichText::new("Transcript").strong().size(13.0).color(th.muted));
        let follow = lab.follow;
        ScrollArea::vertical()
            .id_salt("lab_tx")
            .stick_to_bottom(follow)
            .auto_shrink([false, false])
            .show(ui, |ui| {
                ui.set_width(ui.available_width());
                if lab.transcript.is_empty() {
                    ui.label(
                        RichText::new("Replies land here. Connect AT, then Pulse.")
                            .size(13.0)
                            .color(th.muted),
                    );
                }
                for line in &lab.transcript {
                    let (c, prefix) = match line.kind {
                        LineKind::Tx => (th.accent, "→ "),
                        LineKind::RxOk => (th.serving, "← "),
                        LineKind::RxErr => (th.danger, "← "),
                        LineKind::Note => (th.muted, "· "),
                        LineKind::Err => (th.danger, "! "),
                    };
                    ui.label(
                        RichText::new(format!("{prefix}{}", line.text.trim()))
                            .size(12.5)
                            .color(c)
                            .family(egui::FontFamily::Monospace),
                    );
                }
            });
    });
}

fn send_raw(lab: &mut LabState, live: &LiveState) {
    if lab.raw.trim().is_empty() {
        return;
    }
    let cmd = normalize_at(&lab.raw);
    if lab.history.front().map(|s| s.as_str()) != Some(lab.raw.trim()) {
        lab.history.push_front(lab.raw.trim().to_string());
        if lab.history.len() > 40 {
            lab.history.pop_back();
        }
    }
    lab.hist_i = None;
    let to = (lab.raw_timeout_s * 1000.0) as u32;
    lab.send_lit(live, &cmd, to);
    lab.raw.clear();
}

fn hist_move(lab: &mut LabState, dir: i32) {
    if lab.history.is_empty() {
        return;
    }
    let i = match lab.hist_i {
        None if dir < 0 => 0,
        None => return,
        Some(i) => {
            let n = lab.history.len() as i32;
            (i as i32 + dir).clamp(0, n - 1) as usize
        }
    };
    lab.hist_i = Some(i);
    if let Some(s) = lab.history.get(i) {
        lab.raw = s.clone();
    }
}

fn labeled_field(ui: &mut Ui, th: &Theme, lab: &str, val: &mut String, w: f32) {
    ui.vertical(|ui| {
        ui.label(RichText::new(lab).size(11.0).color(th.muted));
        ui.add(TextEdit::singleline(val).desired_width(w).font(egui::TextStyle::Monospace));
    });
}

fn ch_label(rat: Rat) -> &'static str {
    match rat {
        Rat::Wcdma => "UARFCN",
        Rat::Gsm => "ARFCN",
        Rat::Nr => "NR-ARFCN",
        Rat::Lte => "EARFCN",
    }
}

fn phy_label(rat: Rat) -> &'static str {
    match rat {
        Rat::Wcdma => "PSC",
        Rat::Gsm => "BSIC",
        Rat::Nr | Rat::Lte => "PCI",
    }
}

fn primary(ui: &mut Ui, th: &Theme, label: &str, c: Color32, hint: &str) -> egui::Response {
    ui.add(
        egui::Button::new(RichText::new(label).strong().size(14.0).color(c))
            .fill(th.wash(c, 28))
            .stroke(Stroke::new(1.0, th.wash(c, 90)))
            .corner_radius(CornerRadius::same(10))
            .min_size(Vec2::new(0.0, 36.0)),
    )
    .on_hover_text(hint)
}

fn right_pane(ui: &mut Ui, th: &Theme, lab: &mut LabState, live: &mut LiveState) {
    ui.vertical(|ui| {
        ui.set_width(ui.available_width());
        ui.horizontal(|ui| {
        tab_chip(ui, th, "Caught", lab.right == RightTab::Towers, icons::CELL_TOWER).then(|| {
            lab.right = RightTab::Towers;
        });
        tab_chip(ui, th, "DIAG", lab.right == RightTab::Diag, icons::RADIO).then(|| {
            lab.right = RightTab::Diag;
        });
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            let n = live.flat.len();
            ui.label(RichText::new(format!("{n}")).size(12.0).color(th.muted));
        });
    });
    ui.add_space(8.0);
    match lab.right {
        RightTab::Towers => towers_panel(ui, th, lab, live),
        RightTab::Diag => diag_panel(ui, th, lab, live),
    }
    });
}

fn tab_chip(ui: &mut Ui, th: &Theme, label: &str, on: bool, icon: &str) -> bool {
    let c = if on { th.accent } else { th.muted };
    ui.add(
        egui::Button::new(RichText::new(format!("{icon}  {label}")).strong().size(13.0).color(c))
            .fill(if on { th.wash(th.accent, 28) } else { Color32::TRANSPARENT })
            .stroke(if on {
                Stroke::new(1.0, th.wash(th.accent, 90))
            } else {
                Stroke::NONE
            })
            .corner_radius(CornerRadius::same(8)),
    )
    .clicked()
}

fn towers_panel(ui: &mut Ui, th: &Theme, lab: &mut LabState, live: &mut LiveState) {
    ui.add(
        TextEdit::singleline(&mut lab.tower_q)
            .hint_text("filter EARFCN PCI CID PLMN")
            .desired_width(f32::INFINITY),
    );
    ui.add_space(4.0);
    ui.label(
        RichText::new("Click a row to arm the target. Camp sends dual-lock (or CLUCELL on 3G).")
            .size(12.0)
            .color(th.muted),
    );
    ui.add_space(6.0);

    let q = lab.tower_q.trim().to_ascii_lowercase();
    let mut rows: Vec<usize> = (0..live.flat.len())
        .filter(|&ix| {
            let ft = &live.flat[ix];
            if q.is_empty() {
                return true;
            }
            let t = &ft.tower;
            format!(
                "{} {} {} {} {} {}",
                ft.rat.as_str(),
                t.channel(),
                t.cell_code(),
                t.get_id("cid"),
                t.plmn(),
                t.key
            )
            .to_ascii_lowercase()
            .contains(&q)
        })
        .collect();
    rows.sort_by(|&a, &b| {
        let ta = &live.flat[a].tower;
        let tb = &live.flat[b].tower;
        tb.is_serving()
            .cmp(&ta.is_serving())
            .then(tb.has_full_passport().cmp(&ta.has_full_passport()))
            .then(ta.channel().cmp(tb.channel()))
            .then(ta.cell_code().cmp(tb.cell_code()))
    });

    let height = (ui.available_height() - 8.0).max(80.0);
    let mut camp_ix: Option<usize> = None;
    let mut pick_ix: Option<usize> = None;

    TableBuilder::new(ui)
        .striped(true)
        .sense(Sense::click())
        .cell_layout(Layout::left_to_right(Align::Center))
        .min_scrolled_height(height)
        .max_scroll_height(height)
        .column(Column::exact(52.0))
        .column(Column::exact(44.0))
        .column(Column::exact(92.0))
        .column(Column::exact(56.0))
        .column(Column::remainder().at_least(64.0))
        .column(Column::exact(52.0))
        .header(22.0, |mut h| {
            for lab in ["ST", "RAT", "EARFCN/PCI", "RSRP", "CID", ""] {
                h.col(|ui| {
                    ui.label(RichText::new(lab).strong().size(10.0).color(th.muted));
                });
            }
        })
        .body(|body| {
            body.rows(34.0, rows.len(), |mut row| {
                let ix = rows[row.index()];
                let Some(ft) = live.flat.get(ix) else { return };
                let t = &ft.tower;
                let sel = lab.selected_key == t.key;
                if sel {
                    row.set_selected(true);
                }
                let serving = t.is_serving();
                let full = t.has_full_passport();
                row.col(|ui| {
                    let (lab, c) = if serving {
                        ("SRV", th.serving)
                    } else if full {
                        ("FULL", th.accent)
                    } else if t.was_camped() {
                        ("LOCK", th.warning)
                    } else {
                        ("RF", th.muted)
                    };
                    ui.label(RichText::new(lab).size(11.0).strong().color(c));
                });
                row.col(|ui| {
                    ui.label(
                        RichText::new(ft.rat.as_str())
                            .size(11.0)
                            .color(th.rat(ft.rat)),
                    );
                });
                row.col(|ui| {
                    ui.label(
                        RichText::new(format!("{}/{}", t.channel(), t.cell_code()))
                            .size(12.0)
                            .family(egui::FontFamily::Monospace),
                    );
                });
                row.col(|ui| {
                    let r = t.rsrp_display();
                    ui.label(RichText::new(if r.is_empty() { "—" } else { r }).size(12.0));
                });
                row.col(|ui| {
                    let cid = t.get_id("cid");
                    ui.label(
                        RichText::new(if cid.is_empty() { "—" } else { cid })
                            .size(12.0)
                            .family(egui::FontFamily::Monospace),
                    );
                });
                row.col(|ui| {
                    if ui
                        .add(
                            egui::Button::new(RichText::new("Camp").size(11.0).color(th.serving))
                                .fill(th.wash(th.serving, 24))
                                .min_size(Vec2::new(48.0, 22.0)),
                        )
                        .clicked()
                    {
                        camp_ix = Some(ix);
                    }
                });
                if row.response().clicked() {
                    pick_ix = Some(ix);
                }
            });
        });

    if let Some(ix) = pick_ix {
        if let Some(ft) = live.flat.get(ix).cloned() {
            live.selected = Some(ix);
            lab.fill_from_tower(&ft);
        }
    }
    if let Some(ix) = camp_ix {
        if let Some(ft) = live.flat.get(ix).cloned() {
            live.selected = Some(ix);
            lab.fill_from_tower(&ft);
            match lab.camp_steps() {
                Ok(steps) => lab.send_steps(live, "camp", steps),
                Err(e) => {
                    lab.last_err = e.clone();
                    lab.push(LineKind::Err, e);
                }
            }
        }
    }

    if live.flat.is_empty() {
        ui.add_space(12.0);
        ui.label(
            RichText::new("No live towers yet. Start Live Scan, or connect AT and Pulse — hop JSON lands in /tmp/qcom_live_towers.json.")
                .size(13.0)
                .color(th.muted),
        );
    }
}

fn diag_panel(ui: &mut Ui, th: &Theme, lab: &mut LabState, live: &LiveState) {
    ui.add(
        TextEdit::singleline(&mut lab.diag_q)
            .hint_text("B0C2  SIB  meas  NAS…")
            .desired_width(f32::INFINITY),
    );
    ui.add_space(6.0);

    let flying = live
        .doc
        .as_ref()
        .map(|d| diag_codes::parse_diag_top(&d.meta.diag_top))
        .unwrap_or_default();
    let max_seen = flying.iter().map(|f| f.seen).max().unwrap_or(1).max(1);

    ui.label(RichText::new("Flying now").strong().size(13.0).color(th.text));
    ui.label(
        RichText::new("seen / with-events from live_scanner. High seen + 0 events = RF noise, not identity.")
            .size(12.0)
            .color(th.muted),
    );
    ui.add_space(4.0);
    if flying.is_empty() {
        ui.label(
            RichText::new("No diag_top yet — start Live Scan so the parser fills code_hist.")
                .size(13.0)
                .color(th.muted),
        );
    } else {
        for f in &flying {
            let frac = (f.seen as f32 / max_seen as f32).clamp(0.05, 1.0);
            ui.horizontal(|ui| {
                ui.label(
                    RichText::new(format!("0x{}", f.hex()))
                        .size(12.0)
                        .strong()
                        .color(th.accent)
                        .family(egui::FontFamily::Monospace),
                );
                ui.label(RichText::new(f.short).size(12.5).color(th.text));
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    ui.label(
                        RichText::new(format!("{}/{}", f.seen, f.events))
                            .size(12.0)
                            .color(if f.events > 0 { th.serving } else { th.warning })
                            .family(egui::FontFamily::Monospace),
                    );
                });
            });
            let (rect, resp) = ui.allocate_exact_size(Vec2::new(ui.available_width(), 6.0), Sense::hover());
            ui.painter().rect_filled(rect, CornerRadius::same(3), th.panel2);
            let mut bar = rect;
            bar.set_width(rect.width() * frac);
            ui.painter().rect_filled(
                bar,
                CornerRadius::same(3),
                if f.events > 0 {
                    th.wash(th.serving, 160)
                } else {
                    th.wash(th.warning, 140)
                },
            );
            resp.on_hover_text(format!("{} — {}", f.family, f.hint));
            ui.add_space(4.0);
        }
    }

    ui.add_space(8.0);
    ui.label(RichText::new("All codes we parse").strong().size(13.0).color(th.text));
    let q = lab.diag_q.trim().to_ascii_lowercase();
    let flying_codes: Vec<u16> = flying.iter().map(|f| f.code).collect();
    ScrollArea::vertical()
        .id_salt("lab_diag_cat")
        .auto_shrink([false, false])
        .show(ui, |ui| {
            let mut last = "";
            for d in CATALOG {
                if !q.is_empty() {
                    let blob = format!("{:04x} {} {} {}", d.code, d.short, d.family, d.hint).to_ascii_lowercase();
                    if !blob.contains(&q) {
                        continue;
                    }
                }
                if d.family != last {
                    last = d.family;
                    ui.add_space(6.0);
                    ui.label(RichText::new(d.family).strong().size(11.5).color(th.muted));
                }
                let hot = flying_codes.contains(&d.code);
                let c = if hot { th.accent } else { th.text };
                ui.horizontal(|ui| {
                    ui.label(
                        RichText::new(format!("0x{:04X}", d.code))
                            .size(12.0)
                            .color(if hot { th.accent } else { th.muted })
                            .family(egui::FontFamily::Monospace),
                    );
                    ui.label(RichText::new(d.short).size(12.5).color(c));
                    if hot {
                        ui.label(RichText::new("live").size(11.0).color(th.serving));
                    }
                });
                ui.label(RichText::new(d.hint).size(11.5).color(th.muted));
                ui.add_space(2.0);
            }
        });
}

fn confirm_modal(ui: &mut Ui, th: &Theme, lab: &mut LabState, live: &LiveState) {
    let Confirm::Ask { title, body, job, steps } = &lab.confirm else {
        return;
    };
    let title = title.clone();
    let body = body.clone();
    let job = job.clone();
    let steps = steps.clone();
    let mut open = true;
    egui::Window::new(format!("{}  {title}", icons::WARNING))
        .collapsible(false)
        .resizable(false)
        .anchor(egui::Align2::CENTER_CENTER, Vec2::ZERO)
        .open(&mut open)
        .frame(
            Frame::new()
                .fill(th.panel)
                .stroke(Stroke::new(1.0, th.wash(th.danger, 140)))
                .corner_radius(CornerRadius::same(12))
                .inner_margin(Margin::same(16)),
        )
        .show(ui.ctx(), |ui| {
            ui.set_min_width(360.0);
            ui.label(RichText::new(body).size(14.0).color(th.text));
            ui.add_space(12.0);
            ui.horizontal(|ui| {
                if ui
                    .add(
                        egui::Button::new(RichText::new("Send anyway").color(th.danger))
                            .fill(th.wash(th.danger, 28)),
                    )
                    .clicked()
                {
                    lab.send_steps(live, &job, steps.clone());
                    lab.confirm = Confirm::Idle;
                }
                if ui.button("Cancel").clicked() {
                    lab.confirm = Confirm::Idle;
                }
            });
        });
    if !open {
        lab.confirm = Confirm::Idle;
    }
}

fn lte_camp_steps(
    earfcn: &str,
    pci: &str,
    band: Option<u8>,
    invite_nas: bool,
) -> Result<Vec<AtStep>, String> {
    let ear = earfcn.trim();
    let pci = pci.trim();
    if ear.is_empty() || pci.is_empty() {
        return Err("need EARFCN and PCI".into());
    }
    let band = band.ok_or("need band (or an EARFCN we know: B1/3/7/8/20/28/38/40/41)")?;
    let mut steps = vec![
        AtStep {
            cmd: format!("AT+CLEARFCN={band},{ear}"),
            timeout_ms: 2500,
        },
        AtStep {
            cmd: format!("AT+CCELLCFG=1,{pci},{ear}"),
            timeout_ms: 2500,
        },
        AtStep {
            cmd: format!("AT+CLECELL={ear},{pci}"),
            timeout_ms: 2500,
        },
    ];
    if invite_nas {
        steps.push(AtStep {
            cmd: "AT+COPS=0".into(),
            timeout_ms: 8000,
        });
    }
    steps.push(AtStep {
        cmd: "AT+CPSI?".into(),
        timeout_ms: 2000,
    });
    steps.push(AtStep {
        cmd: "AT+CCELLCFG?".into(),
        timeout_ms: 1500,
    });
    steps.push(AtStep {
        cmd: "AT+CLECELL?".into(),
        timeout_ms: 1500,
    });
    Ok(steps)
}

fn wcdma_camp_steps(uarfcn: &str, psc: &str) -> Result<Vec<AtStep>, String> {
    let u = uarfcn.trim();
    let p = psc.trim();
    if u.is_empty() || p.is_empty() {
        return Err("need UARFCN and PSC".into());
    }
    Ok(vec![
        AtStep {
            cmd: "AT+CNMP=14".into(),
            timeout_ms: 8000,
        },
        AtStep {
            cmd: format!("AT+CLUARFCN={u}"),
            timeout_ms: 3000,
        },
        AtStep {
            cmd: format!("AT+CLUCELL={u},{p}"),
            timeout_ms: 3000,
        },
        AtStep {
            cmd: "AT+CPSI?".into(),
            timeout_ms: 2000,
        },
        AtStep {
            cmd: "AT+CLUCELL?".into(),
            timeout_ms: 1500,
        },
    ])
}

/// TS 36.101 subset used by hop CLEARFCN (BandInfo.h).
fn lte_band(earfcn: u32) -> Option<(u8, &'static str)> {
    const ROWS: &[(u8, u32, u32, &str)] = &[
        (1, 0, 599, "B1 FDD"),
        (3, 1200, 1949, "B3 FDD"),
        (7, 2750, 3449, "B7 FDD"),
        (8, 3450, 3799, "B8 FDD"),
        (20, 6150, 6449, "B20 FDD"),
        (28, 9210, 9659, "B28 FDD"),
        (38, 37750, 38249, "B38 TDD"),
        (40, 38650, 39649, "B40 TDD"),
        (41, 39650, 41589, "B41 TDD"),
    ];
    ROWS.iter()
        .find(|&&(_, lo, hi, _)| earfcn >= lo && earfcn <= hi)
        .map(|&(b, _, _, n)| (b, n))
}

fn plmn_numeric(s: &str) -> String {
    let d: String = s.chars().filter(|c| c.is_ascii_digit()).collect();
    d
}

fn normalize_at(raw: &str) -> String {
    let s = raw.trim();
    if s.is_empty() {
        return String::new();
    }
    if s.eq_ignore_ascii_case("at") || s.to_ascii_uppercase().starts_with("AT") {
        s.to_string()
    } else {
        format!("AT{s}")
    }
}

fn urc(text: &str, tag: &str) -> Option<String> {
    for line in text.split(['\r', '\n']) {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix(tag) {
            return Some(rest.trim().trim_start_matches(':').trim().to_string());
        }
        if line.starts_with(tag) {
            return Some(line[tag.len()..].trim().to_string());
        }
    }
    None
}

fn first_final(text: &str) -> String {
    text.split(['\r', '\n'])
        .map(str::trim)
        .find(|l| !l.is_empty())
        .unwrap_or("")
        .to_string()
}

fn format_rx(text: &str) -> String {
    text.replace('\r', "")
        .lines()
        .map(str::trim)
        .filter(|l| !l.is_empty())
        .collect::<Vec<_>>()
        .join("\n")
}
