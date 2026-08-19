//! DIAG log codes that live_scanner actually parses — for the Lab histogram.
//! Names match `LteParser` / `WcdmaParser` / `GsmParser` / `NrParser` tables.

#[derive(Clone, Copy)]
pub struct DiagCode {
    pub code: u16,
    pub short: &'static str,
    pub family: &'static str,
    /// What this packet is good for when hunting identity / RF by hand.
    pub hint: &'static str,
}

pub const CATALOG: &[DiagCode] = &[
    // LTE RRC / NAS
    DiagCode {
        code: 0xB0C0,
        short: "RRC OTA",
        family: "LTE RRC",
        hint: "SIB1–7, MeasConfig, HO. Passport + neighbors when camped.",
    },
    DiagCode {
        code: 0xB0C1,
        short: "RRC MIB",
        family: "LTE RRC",
        hint: "BW / SFN. Cell is on-air even without SIB1 yet.",
    },
    DiagCode {
        code: 0xB0C2,
        short: "Serving Cell Info",
        family: "LTE RRC",
        hint: "GOLD: PLMN / TAC / CID / band / BW. Real FULL, not RF-only.",
    },
    DiagCode {
        code: 0xB0C3,
        short: "PLMN Search Req",
        family: "LTE RRC",
        hint: "Modem started a PLMN search (COPS=? / ghost).",
    },
    DiagCode {
        code: 0xB0C4,
        short: "PLMN Search Rsp",
        family: "LTE RRC",
        hint: "Search hits — RF rows without CID. Discover fuel.",
    },
    DiagCode {
        code: 0xB0CB,
        short: "RRC Paging",
        family: "LTE RRC",
        hint: "Idle presence. Not identity.",
    },
    DiagCode {
        code: 0xB0CD,
        short: "CA combos",
        family: "LTE RRC",
        hint: "UE CA capability, not the live SCell.",
    },
    DiagCode {
        code: 0xB0E2,
        short: "NAS ESM in",
        family: "LTE NAS",
        hint: "ESM attach/PDN. Means NAS is actually talking.",
    },
    DiagCode {
        code: 0xB0EA,
        short: "NAS EMM sec in",
        family: "LTE NAS",
        hint: "Secured EMM downlink — registered-ish.",
    },
    DiagCode {
        code: 0xB0EB,
        short: "NAS EMM sec out",
        family: "LTE NAS",
        hint: "Secured EMM uplink.",
    },
    DiagCode {
        code: 0xB0EC,
        short: "NAS EMM in",
        family: "LTE NAS",
        hint: "Plain EMM downlink (attach / TAU).",
    },
    DiagCode {
        code: 0xB0ED,
        short: "NAS EMM out",
        family: "LTE NAS",
        hint: "Plain EMM uplink.",
    },
    DiagCode {
        code: 0xB0EE,
        short: "NAS EMM State",
        family: "LTE NAS",
        hint: "Reg state / PLMN / MME. NO_SERVICE vs EMM-REGISTERED.",
    },
    // LTE ML1 / LL1
    DiagCode {
        code: 0xB113,
        short: "LL1 PSS",
        family: "LTE PHY",
        hint: "Primary sync. Earfcn hunt, no PCI yet.",
    },
    DiagCode {
        code: 0xB114,
        short: "LL1 frame timing",
        family: "LTE PHY",
        hint: "Timing advance / frame. Geo-ish, not CID.",
    },
    DiagCode {
        code: 0xB115,
        short: "LL1 SSS",
        family: "LTE PHY",
        hint: "PCI from SSS. RF lock can mint PCI here.",
    },
    DiagCode {
        code: 0xB123,
        short: "LL1 neigh CER",
        family: "LTE PHY",
        hint: "Neighbor coarse energy. Weak identity.",
    },
    DiagCode {
        code: 0xB175,
        short: "ML1 cell metrics",
        family: "LTE ML1",
        hint: "Low-level cell metrics.",
    },
    DiagCode {
        code: 0xB176,
        short: "Initial acq",
        family: "LTE ML1",
        hint: "Acquisition attempt on an EARFCN.",
    },
    DiagCode {
        code: 0xB179,
        short: "Conn intra meas",
        family: "LTE ML1",
        hint: "Connected-mode intra-freq. Neigh PCI while RRC connected.",
    },
    DiagCode {
        code: 0xB17F,
        short: "ML1 serving meas",
        family: "LTE ML1",
        hint: "High-rate serving RSRP. Confirms RF lock, not CID.",
    },
    DiagCode {
        code: 0xB180,
        short: "ML1 neigh meas",
        family: "LTE ML1",
        hint: "Idle/conn neighbor meas — EARFCN|PCI list.",
    },
    DiagCode {
        code: 0xB181,
        short: "ML1 intra resel",
        family: "LTE ML1",
        hint: "Reselection ranking. Intra PCI movement.",
    },
    DiagCode {
        code: 0xB192,
        short: "Idle neigh meas",
        family: "LTE ML1",
        hint: "Idle neighbor PHY. SIB4/5 hunt companion.",
    },
    DiagCode {
        code: 0xB193,
        short: "ML1 meas resp",
        family: "LTE ML1",
        hint: "GOLD RF: filtered RSRP/RSRQ/SINR. Still not TAC/CID.",
    },
    DiagCode {
        code: 0xB194,
        short: "ML1 search",
        family: "LTE ML1",
        hint: "Search req/rsp. Lots of these = hunting, not camped.",
    },
    DiagCode {
        code: 0xB195,
        short: "Conn neigh meas",
        family: "LTE ML1",
        hint: "Connected neighbor PHY.",
    },
    DiagCode {
        code: 0xB197,
        short: "ML1 serving info",
        family: "LTE ML1",
        hint: "Serving EARFCN|PCI stamp. Pair with B0C2 for FULL.",
    },
    // WCDMA
    DiagCode {
        code: 0x4005,
        short: "WCDMA resel rank",
        family: "WCDMA",
        hint: "3G neighbor ranking (UARFCN/PSC).",
    },
    DiagCode {
        code: 0x4027,
        short: "WCDMA cell ID",
        family: "WCDMA",
        hint: "3G CID. FULL on WCDMA.",
    },
    DiagCode {
        code: 0x4111,
        short: "WCDMA active set",
        family: "WCDMA",
        hint: "Soft-handover set.",
    },
    DiagCode {
        code: 0x4127,
        short: "WCDMA serving",
        family: "WCDMA",
        hint: "Serving UARFCN/PSC/RSCP.",
    },
    DiagCode {
        code: 0x412F,
        short: "WCDMA RRC OTA",
        family: "WCDMA",
        hint: "SIB / DCCH. Identity + neigh.",
    },
    DiagCode {
        code: 0x713A,
        short: "UMTS NAS OTA",
        family: "WCDMA",
        hint: "3G NAS. Attach actually happening.",
    },
    // GSM
    DiagCode {
        code: 0x512F,
        short: "GSM RR signaling",
        family: "GSM",
        hint: "SI / L3. GSM identity.",
    },
    DiagCode {
        code: 0x5134,
        short: "GSM cell info",
        family: "GSM",
        hint: "Serving ARFCN / BSIC / CI.",
    },
    DiagCode {
        code: 0x5071,
        short: "GSM surround",
        family: "GSM",
        hint: "BA list / surround cells.",
    },
    // NR
    DiagCode {
        code: 0xB821,
        short: "NR RRC OTA",
        family: "NR",
        hint: "NR SIB / DCCH. Rare on SIM8300 LTE-only pin.",
    },
    DiagCode {
        code: 0xB822,
        short: "NR RRC MIB",
        family: "NR",
        hint: "NR MIB.",
    },
    DiagCode {
        code: 0xB823,
        short: "NR serving",
        family: "NR",
        hint: "NR serving cell info.",
    },
    DiagCode {
        code: 0xB97F,
        short: "NR ML1 meas",
        family: "NR",
        hint: "NR meas DB.",
    },
    DiagCode {
        code: 0xB992,
        short: "NR ML1 serving",
        family: "NR",
        hint: "NR serving PHY.",
    },
];

pub fn lookup(code: u16) -> Option<&'static DiagCode> {
    CATALOG.iter().find(|c| c.code == code)
}

pub fn parse_diag_top(s: &str) -> Vec<Flying> {
    let mut out = Vec::new();
    for part in s.split(',') {
        let part = part.trim();
        if part.is_empty() {
            continue;
        }
        // "B194:120/4"
        let (hex, rest) = match part.split_once(':') {
            Some(x) => x,
            None => continue,
        };
        let code = u16::from_str_radix(hex.trim(), 16).ok();
        let Some(code) = code else { continue };
        let (seen, ev) = match rest.split_once('/') {
            Some((a, b)) => (
                a.trim().parse::<u64>().unwrap_or(0),
                b.trim().parse::<u64>().unwrap_or(0),
            ),
            None => (rest.trim().parse::<u64>().unwrap_or(0), 0),
        };
        let meta = lookup(code);
        out.push(Flying {
            code,
            seen,
            events: ev,
            short: meta.map(|m| m.short).unwrap_or("unparsed"),
            family: meta.map(|m| m.family).unwrap_or("other"),
            hint: meta.map(|m| m.hint).unwrap_or("Flew on DIAG; we have no parser."),
        });
    }
    out
}

#[derive(Clone)]
pub struct Flying {
    pub code: u16,
    pub seen: u64,
    pub events: u64,
    pub short: &'static str,
    pub family: &'static str,
    pub hint: &'static str,
}

impl Flying {
    pub fn hex(&self) -> String {
        format!("{:04X}", self.code)
    }
}
