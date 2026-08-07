//! qcom.towers.v4 JSON model (RAT-grouped, complete unique towers).

use serde::Deserialize;
use std::collections::BTreeMap;
use std::path::Path;

#[derive(Debug, Clone, Deserialize)]
pub struct Document {
    pub meta: Meta,
    pub towers: TowersByRat,
}

#[derive(Debug, Clone, Deserialize, Default)]
pub struct Meta {
    #[serde(default)]
    pub source: String,
    #[serde(default)]
    pub situation_as_of: String,
    #[serde(default)]
    pub tower_count: String,
    #[serde(default)]
    pub raw_tower_count: String,
    #[serde(default)]
    pub filter: String,
    #[serde(default)]
    pub serving_count: String,
    #[serde(default)]
    pub towers_with_neighbors: String,
    #[serde(default)]
    pub gsm: String,
    #[serde(default)]
    pub lte: String,
    #[serde(default)]
    pub wcdma: String,
    #[serde(default)]
    pub nr: String,
    #[serde(default)]
    pub schema: String,
    #[serde(default)]
    pub origin: String,
    #[serde(default)]
    pub complete_count: String,
    #[serde(default)]
    pub updates: String,
    #[serde(default)]
    pub hop_kicks: String,
    #[serde(default)]
    pub hop_locks: String,
    #[serde(default)]
    pub hop_fulls: String,
    #[serde(default)]
    pub hop_cops: String,
    #[serde(default)]
    pub cpsi_ok: String,
    #[serde(default)]
    pub qmi_hop_snaps: String,
    #[serde(default)]
    pub qmi_reg: String,
    #[serde(default)]
    pub qmi_ps: String,
    #[serde(default)]
    pub qmi_radio: String,
    #[serde(default)]
    pub qmi_plmn: String,
    #[serde(default)]
    pub qmi_plmn_name: String,
    #[serde(default)]
    pub qmi_rsrp: String,
    #[serde(default)]
    pub qmi_rsrq: String,
    #[serde(default)]
    pub qmi_snr: String,
}

#[derive(Debug, Clone, Deserialize, Default)]
pub struct TowersByRat {
    #[serde(default)]
    pub gsm: Vec<Tower>,
    #[serde(default)]
    pub lte: Vec<Tower>,
    #[serde(default)]
    pub wcdma: Vec<Tower>,
    #[serde(default)]
    pub nr: Vec<Tower>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Rat {
    Gsm,
    Lte,
    Wcdma,
    Nr,
}

impl Rat {
    pub fn as_str(self) -> &'static str {
        match self {
            Rat::Gsm => "GSM",
            Rat::Lte => "LTE",
            Rat::Wcdma => "WCDMA",
            Rat::Nr => "NR",
        }
    }
}

#[derive(Debug, Clone, Deserialize)]
pub struct Tower {
    pub key: String,
    #[serde(default)]
    pub meta: TowerMeta,
    #[serde(default)]
    pub identity: BTreeMap<String, String>,
    #[serde(default)]
    pub radio: BTreeMap<String, String>,
    #[serde(default)]
    pub signal: BTreeMap<String, String>,
    #[serde(default)]
    pub neighbors: Neighbors,
}

#[derive(Debug, Clone, Deserialize, Default)]
pub struct TowerMeta {
    #[serde(default)]
    pub serving: String,
    /// "1" if modem camped/locked this cell at least once this session.
    #[serde(default)]
    pub camped: String,
    #[serde(default)]
    pub seen: String,
    #[serde(default)]
    pub first_seen: String,
    #[serde(default)]
    pub last_seen: String,
}

#[derive(Debug, Clone, Deserialize, Default)]
pub struct Neighbors {
    #[serde(default)]
    pub nb_lte: Vec<Neighbor>,
    #[serde(default)]
    pub nb_gsm: Vec<Neighbor>,
    #[serde(default)]
    pub nb_umts: Vec<Neighbor>,
}

#[derive(Debug, Clone, Deserialize, Default)]
pub struct Neighbor {
    #[serde(default)]
    pub rat: String,
    #[serde(default)]
    pub resolved: String,
    #[serde(default)]
    pub meta: TowerMeta,
    #[serde(default)]
    pub identity: BTreeMap<String, String>,
    #[serde(default)]
    pub radio: BTreeMap<String, String>,
    #[serde(default)]
    pub signal: BTreeMap<String, String>,
}

#[derive(Debug, Clone)]
pub struct FlatTower {
    pub rat: Rat,
    pub tower: Tower,
}

impl Document {
    pub fn load(path: impl AsRef<Path>) -> Result<Self, String> {
        let bytes = std::fs::read(path.as_ref()).map_err(|e| e.to_string())?;
        serde_json::from_slice(&bytes).map_err(|e| e.to_string())
    }

    pub fn flatten(&self) -> Vec<FlatTower> {
        let mut out = Vec::new();
        for t in &self.towers.gsm {
            out.push(FlatTower {
                rat: Rat::Gsm,
                tower: t.clone(),
            });
        }
        for t in &self.towers.lte {
            out.push(FlatTower {
                rat: Rat::Lte,
                tower: t.clone(),
            });
        }
        for t in &self.towers.wcdma {
            out.push(FlatTower {
                rat: Rat::Wcdma,
                tower: t.clone(),
            });
        }
        for t in &self.towers.nr {
            out.push(FlatTower {
                rat: Rat::Nr,
                tower: t.clone(),
            });
        }
        out
    }
}

impl Tower {
    pub fn get_id(&self, k: &str) -> &str {
        self.identity.get(k).map(|s| s.as_str()).unwrap_or("")
    }
    pub fn get_radio(&self, k: &str) -> &str {
        self.radio.get(k).map(|s| s.as_str()).unwrap_or("")
    }
    pub fn get_sig(&self, k: &str) -> &str {
        self.signal.get(k).map(|s| s.as_str()).unwrap_or("")
    }

    pub fn channel(&self) -> &str {
        for k in ["earfcn", "uarfcn", "arfcn", "nrarfcn"] {
            let v = self.get_radio(k);
            if !v.is_empty() {
                return v;
            }
        }
        ""
    }

    pub fn cell_code(&self) -> &str {
        for k in ["pci", "psc", "bsic"] {
            let v = self.get_radio(k);
            if !v.is_empty() {
                return v;
            }
        }
        ""
    }

    pub fn lac_or_tac(&self) -> &str {
        let tac = self.get_id("tac");
        if !tac.is_empty() {
            return tac;
        }
        self.get_id("lac")
    }

    pub fn plmn(&self) -> &str {
        let m = self.get_id("mcc_mnc");
        if !m.is_empty() {
            return m;
        }
        ""
    }

    pub fn neighbor_count(&self) -> usize {
        self.neighbors.nb_lte.len() + self.neighbors.nb_gsm.len() + self.neighbors.nb_umts.len()
    }

    pub fn is_serving(&self) -> bool {
        self.meta.serving == "1"
    }

    /// Sticky RF-lock flag from feed (may include hop locks without SIB1).
    pub fn was_camped(&self) -> bool {
        self.meta.camped == "1" || self.meta.serving == "1"
    }

    /// Real camp: sticky/serving **and** Cell Identity present.
    pub fn was_identity_camped(&self) -> bool {
        self.was_camped() && !self.get_id("cid").is_empty()
    }

    pub fn has_cid(&self) -> bool {
        !self.get_id("cid").is_empty()
    }

    pub fn band(&self) -> &str {
        self.get_radio("band")
    }

    pub fn rxl(&self) -> &str {
        self.get_sig("rxl")
    }

    /// Full identity for cross-dump match: RAT|PLMN|LAC/TAC|CID|channel|code.
    pub fn full_match_key(&self, rat: Rat) -> String {
        format!(
            "{}|{}|{}|{}|{}|{}",
            rat.as_str(),
            self.plmn(),
            self.lac_or_tac(),
            self.get_id("cid"),
            self.channel(),
            self.cell_code()
        )
    }
}

#[derive(Debug, Clone, Default)]
pub struct Stats {
    pub total: usize,
    pub gsm: usize,
    pub lte: usize,
    pub wcdma: usize,
    pub nr: usize,
    pub serving: usize,
    pub with_neighbors: usize,
    pub bands: BTreeMap<String, usize>,
    pub operators: BTreeMap<String, usize>,
    pub latest: String,
    pub earliest: String,
}

impl Stats {
    pub fn from_flat(towers: &[FlatTower]) -> Self {
        let mut s = Stats {
            total: towers.len(),
            ..Default::default()
        };
        for ft in towers {
            match ft.rat {
                Rat::Gsm => s.gsm += 1,
                Rat::Lte => s.lte += 1,
                Rat::Wcdma => s.wcdma += 1,
                Rat::Nr => s.nr += 1,
            }
            if ft.tower.is_serving() {
                s.serving += 1;
            }
            if ft.tower.neighbor_count() > 0 {
                s.with_neighbors += 1;
            }
            let band = ft.tower.band();
            if !band.is_empty() {
                *s.bands.entry(band.to_string()).or_default() += 1;
            }
            let plmn = ft.tower.plmn();
            if !plmn.is_empty() {
                *s.operators.entry(plmn.to_string()).or_default() += 1;
            }
            let ls = &ft.tower.meta.last_seen;
            if !ls.is_empty() {
                if s.latest.is_empty() || ls > &s.latest {
                    s.latest = ls.clone();
                }
                if s.earliest.is_empty() || ls < &s.earliest {
                    s.earliest = ls.clone();
                }
            }
        }
        s
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    #[test]
    fn loads_v4_dump() {
        let p = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../scan_dumps/android_vlad_20260729/ours_towers.json");
        let doc = Document::load(&p).expect("load");
        assert!(doc.meta.schema.contains("towers"));
        let flat = doc.flatten();
        assert!(!flat.is_empty());
        assert!(!flat[0].tower.key.is_empty());
    }
}
