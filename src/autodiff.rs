//! Autodiff stub — demonstrates lazy trigger and staged lowering concept.
//! For the prototype we do not implement actual AD math; instead we demonstrate the trigger points:
//! ZIR -> (lower) -> AIR -> MIR -> generate differential function.
//! Each stage is intentionally lazy and cached.

use crate::zir::{ZirDeclRecord, ZirMmapView};
use anyhow::Result;
use once_cell::sync::OnceCell;
use parking_lot::RwLock;
use std::collections::HashMap;
use std::sync::Arc;
use log::info;

/// Placeholder types for IR
#[derive(Debug, Clone)]
pub struct Air {}
#[derive(Debug, Clone)]
pub struct Mir {}

/// Caches to avoid regenerating heavy artifacts
lazy_static::lazy_static! {
    static ref AIR_CACHE: RwLock<HashMap<String, Arc<Air>>> = RwLock::new(HashMap::new());
    static ref MIR_CACHE: RwLock<HashMap<String, Arc<Mir>>> = RwLock::new(HashMap::new());
}

/// Generate AD for a specific declaration (by parsing its tokens/source).
/// This function simulates expensive lowering stages but runs only when this function is called.
pub fn generate_ad_for_decl(view: &ZirMmapView, decl: &ZirDeclRecord) -> Result<()> {
    let key = format!("{}::{}", view.get_str(decl.module_name), view.get_str(decl.decl_name));
    // stage 1: lower ZIR -> AIR (lazy, cached)
    let air = {
        let map = AIR_CACHE.read();
        if let Some(a) = map.get(&key) {
            a.clone()
        } else {
            drop(map);
            info!("Lowering ZIR -> AIR for {}", key);
            // simulate expensive work
            let a = Arc::new(Air {});
            AIR_CACHE.write().insert(key.clone(), a.clone());
            a
        }
    };

    // stage 2: lower AIR -> MIR
    {
        let map = MIR_CACHE.read();
        if map.get(&key).is_some() {
            info!("MIR already generated for {}", key);
        } else {
            drop(map);
            info!("Lowering AIR -> MIR for {}", key);
            // simulate expensive work
            let m = Arc::new(Mir {});
            MIR_CACHE.write().insert(key.clone(), m);
        }
    }

    // stage 3: generate differentiated function artifacts (emit to codegen, etc.)
    info!("Generated AD artifacts for {}", key);
    Ok(())
}