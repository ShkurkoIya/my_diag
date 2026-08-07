//! Calm semantic palette — few roles, readable contrast, status via badges not rainbow fills.

use crate::model::Rat;
use eframe::egui::{Color32, Stroke};

#[derive(Clone, Copy)]
pub struct Theme {
    pub bg: Color32,
    pub panel: Color32,
    pub panel2: Color32,
    pub stroke: Color32,
    pub text: Color32,
    pub muted: Color32,
    pub accent: Color32,
    pub accent2: Color32,
    pub gsm: Color32,
    pub lte: Color32,
    pub wcdma: Color32,
    pub nr: Color32,
    pub serving: Color32,
    pub warning: Color32,
    pub danger: Color32,
    pub ok: Color32,
}

impl Theme {
    /// Cool graphite + single blue accent. Status greens/ambers/reds are secondary.
    pub fn ops() -> Self {
        Self {
            bg: Color32::from_rgb(14, 16, 22),
            panel: Color32::from_rgb(22, 26, 34),
            panel2: Color32::from_rgb(30, 36, 46),
            stroke: Color32::from_rgb(48, 56, 70),
            text: Color32::from_rgb(236, 240, 246),
            muted: Color32::from_rgb(148, 160, 178),
            accent: Color32::from_rgb(96, 165, 214),
            accent2: Color32::from_rgb(214, 176, 96),
            // RAT tints — desaturated, for small badges only
            gsm: Color32::from_rgb(196, 168, 110),
            lte: Color32::from_rgb(110, 168, 196),
            wcdma: Color32::from_rgb(140, 150, 196),
            nr: Color32::from_rgb(170, 140, 196),
            serving: Color32::from_rgb(88, 176, 130),
            warning: Color32::from_rgb(196, 164, 88),
            danger: Color32::from_rgb(196, 110, 110),
            ok: Color32::from_rgb(88, 176, 130),
        }
    }

    pub fn rat(self, rat: Rat) -> Color32 {
        match rat {
            Rat::Gsm => self.gsm,
            Rat::Lte => self.lte,
            Rat::Wcdma => self.wcdma,
            Rat::Nr => self.nr,
        }
    }

    pub fn wash(self, c: Color32, a: u8) -> Color32 {
        Color32::from_rgba_unmultiplied(c.r(), c.g(), c.b(), a)
    }

    pub fn stroke_soft(self) -> Stroke {
        Stroke::new(1.0, self.stroke)
    }

    pub fn stroke_accent(self) -> Stroke {
        Stroke::new(1.5, self.wash(self.accent, 160))
    }

    pub fn stroke_danger(self) -> Stroke {
        Stroke::new(1.5, self.wash(self.danger, 180))
    }
}

/// Row / node visual state — prefer badges over painting the whole surface.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum SurfaceState {
    Idle,
    Selected,
    Serving,
    Camped,
    Danger,
}

impl Theme {
    pub fn surface_fill(self, state: SurfaceState) -> Color32 {
        match state {
            SurfaceState::Idle => self.panel2,
            SurfaceState::Selected => self.wash(self.accent, 28),
            SurfaceState::Serving => self.wash(self.serving, 22),
            SurfaceState::Camped => self.wash(self.warning, 18),
            SurfaceState::Danger => self.wash(self.danger, 24),
        }
    }

    pub fn surface_stroke(self, state: SurfaceState) -> Stroke {
        match state {
            SurfaceState::Idle => Stroke::new(1.0, self.stroke),
            SurfaceState::Selected => Stroke::new(1.5, self.wash(self.accent, 160)),
            SurfaceState::Serving => Stroke::new(1.2, self.wash(self.serving, 120)),
            SurfaceState::Camped => Stroke::new(1.2, self.wash(self.warning, 110)),
            SurfaceState::Danger => Stroke::new(1.5, self.wash(self.danger, 170)),
        }
    }
}

/// Operator brand — only for tiny dots / graph hubs, never full row fills.
pub fn brand_color(plmn: &str) -> Color32 {
    match plmn {
        "250-01" => Color32::from_rgb(200, 48, 48),   // MTS
        "250-02" => Color32::from_rgb(48, 140, 88),   // MegaFon
        "250-20" => Color32::from_rgb(180, 188, 198), // t2
        "250-99" => Color32::from_rgb(210, 150, 48),  // Beeline
        "250-11" => Color32::from_rgb(48, 140, 180),  // Yota
        _ => Color32::from_rgb(120, 132, 150),
    }
}
