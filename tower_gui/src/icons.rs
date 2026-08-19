//! Phosphor icon helpers — icon font glyphs instead of tofu unicode.

use eframe::egui::{self, Color32, CornerRadius, RichText, Sense, Stroke, Ui, Vec2};
use egui_phosphor::regular;

pub use egui_phosphor::regular::{
    ARROW_LEFT, ARROW_RIGHT, BROADCAST, CELL_SIGNAL_FULL, CELL_TOWER, CHECK, CHECK_CIRCLE,
    CIRCLE, FILE_TEXT, FLASK, FOLDER_OPEN, KEYBOARD, LIGHTNING, LIST, LIST_BULLETS,
    LOCK_SIMPLE, LOCK_SIMPLE_OPEN, MAGNIFYING_GLASS, MAP_PIN, PAUSE, PLAY, RADIO,
    SHARE_NETWORK, STOP, TABLE, TERMINAL, WARNING, WARNING_CIRCLE, X_CIRCLE,
};

/// Compact circle-caret for nested buckets (neighbors, heard RF).
pub fn nest_glyph(open: bool) -> &'static str {
    if open {
        regular::CARET_CIRCLE_DOWN
    } else {
        regular::CARET_CIRCLE_RIGHT
    }
}

/// Clickable expand chip — chevron in a rounded square (not a folder).
pub fn expand_toggle(ui: &mut Ui, open: bool, accent: Color32) -> egui::Response {
    let size = Vec2::splat(28.0);
    let (rect, resp) = ui.allocate_exact_size(size, Sense::click());
    let hovered = resp.hovered();
    let bg = if hovered {
        Color32::from_rgba_unmultiplied(accent.r(), accent.g(), accent.b(), 40)
    } else {
        Color32::from_rgb(36, 42, 54)
    };
    let stroke = if open || hovered {
        Color32::from_rgba_unmultiplied(accent.r(), accent.g(), accent.b(), 140)
    } else {
        Color32::from_rgb(56, 64, 78)
    };
    let painter = ui.painter();
    painter.rect_filled(rect, CornerRadius::same(8), bg);
    painter.rect_stroke(
        rect,
        CornerRadius::same(8),
        Stroke::new(1.0, stroke),
        egui::StrokeKind::Inside,
    );

    // Painted chevron — clearer than folder glyphs for tree expand.
    let c = rect.center();
    let col = if open {
        accent
    } else if hovered {
        Color32::from_rgb(210, 220, 235)
    } else {
        Color32::from_rgb(160, 172, 190)
    };
    let stroke = Stroke::new(2.0, col);
    if open {
        let a = c + Vec2::new(-5.5, -1.5);
        let b = c + Vec2::new(0.0, 4.0);
        let d = c + Vec2::new(5.5, -1.5);
        painter.line_segment([a, b], stroke);
        painter.line_segment([b, d], stroke);
    } else {
        let a = c + Vec2::new(-1.5, -5.5);
        let b = c + Vec2::new(4.0, 0.0);
        let d = c + Vec2::new(-1.5, 5.5);
        painter.line_segment([a, b], stroke);
        painter.line_segment([b, d], stroke);
    }
    resp.on_hover_text(if open { "Collapse" } else { "Expand" })
}

/// Smaller nest toggle for neighbor / heard sections.
pub fn nest_toggle(ui: &mut Ui, open: bool, accent: Color32) -> egui::Response {
    let size = Vec2::splat(24.0);
    let (rect, resp) = ui.allocate_exact_size(size, Sense::click());
    let hovered = resp.hovered();
    let bg = if hovered {
        Color32::from_rgba_unmultiplied(accent.r(), accent.g(), accent.b(), 36)
    } else {
        Color32::TRANSPARENT
    };
    if bg != Color32::TRANSPARENT {
        ui.painter()
            .rect_filled(rect, CornerRadius::same(6), bg);
    }
    let glyph = nest_glyph(open);
    let color = if open {
        accent
    } else if hovered {
        Color32::from_rgb(200, 210, 224)
    } else {
        Color32::from_rgb(140, 152, 170)
    };
    ui.painter().text(
        rect.center(),
        egui::Align2::CENTER_CENTER,
        glyph,
        egui::FontId::proportional(18.0),
        color,
    );
    resp
}

pub fn icon_text(glyph: &str, size: f32, color: Color32) -> RichText {
    RichText::new(glyph).size(size).color(color)
}

/// Small painted status disc (no unicode bullet).
pub fn status_dot(ui: &mut Ui, color: Color32) {
    let (rect, _) = ui.allocate_exact_size(Vec2::splat(9.0), Sense::hover());
    ui.painter().circle_filled(rect.center(), 3.5, color);
}
