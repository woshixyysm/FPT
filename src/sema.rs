//! Semantic analysis layer (Sema) with lazy triggers.
//! The Sema does NOT analyze everything at once — it provides functions that analyze a single ZIR shard on demand.
//! It enforces rules from the Zig philosophy: explicit types for exported symbols, comptime only when marked, no implicit exceptions, explicit typeclass instances, etc.

use crate::zir::{ZirMmapView, ZirDeclRecord, DeclId, ZirNodeId, OpCode};
use anyhow::{anyhow, Result};
use once_cell::sync::OnceCell;
use std::sync::Arc;
use std::collections::HashMap;
use thiserror::Error;
use log::{info, warn};

#[derive(Debug, Error)]
pub enum SemaError {
    #[error("missing explicit type annotation for public symbol `{0}`")]
    MissingType(String),

    #[error("invalid comptime use in `{0}`")]
    ComptimeError(String),

    #[error("autodiff trigger failed for `{0}`: {1}")]
    AutodiffError(String, String),

    #[error("error in node {1}: {0}")]
    NodeError(String, DeclId),
}

/// Sema type cache/representation, avoids repeated String allocation
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum ZirType {
    Void,
    F64,
    Unknown,
}

impl ZirType {
    pub fn to_id(self) -> u32 {
        match self {
            ZirType::Void => 0,
            ZirType::F64 => 1,
            ZirType::Unknown => 2,
        }
    }

    pub fn from_id(id: u32) -> ZirType {
        match id {
            1 => ZirType::F64,
            0 => ZirType::Void,
            _ => ZirType::Unknown,
        }
    }
}

/// Cached semantic result for a decl
pub struct SemaResult {
    pub zir: ZirDeclRecord,
    pub checked: bool,
    // other semantic info: type signatures, inferred attributes, etc.
}

/// A tiny per-declaration semantic cache. This uses OnceCell to ensure analysis runs once.
struct DeclCache {
    inner: OnceCell<Arc<SemaResult>>,
}

impl DeclCache {
    fn new() -> Self {
        Self {
            inner: OnceCell::new(),
        }
    }

    fn get_or_init<F>(&self, f: F) -> Arc<SemaResult>
    where
        F: FnOnce() -> Arc<SemaResult>,
    {
        self.inner.get_or_init(|| f()).clone()
    }
}

/// Very small in-memory cache keyed by module::name
use parking_lot::RwLock;

lazy_static::lazy_static! {
    static ref SEMACACHE: RwLock<HashMap<String, DeclCache>> = RwLock::new(HashMap::new());
}

/// Analyze a single declaration lazily.
/// This function reads the ZIR shard and performs targeted checks.
/// Expensive features (AD, comptime with heavy workload, exhaustive match) are only triggered
/// if the declaration's tokens include the respective keywords or if downstream requests ask for them.
pub fn analyze_declaration_lazy(zir_path: &str, module: &str, name: &str) -> Result<Arc<SemaResult>> {
    let key = format!("{}::{}", module, name);
    {
        // fast path: already in cache
        let map = SEMACACHE.read();
        if let Some(cache) = map.get(&key) {
            let existing = cache.get_or_init(|| {
                let view = ZirMmapView::open(zir_path).unwrap();
                let decl = view.decls().iter().find(|d| view.get_str(d.module_name) == module && view.get_str(d.decl_name) == name).unwrap();
                Arc::new(SemaResult {
                    zir: *decl,
                    checked: true,
                })
            });
            return Ok(existing);
        }
    }

    // install a new cache entry
    let cache = DeclCache::new();
    SEMACACHE.write().insert(key.clone(), cache);

    // now initialize it (do real work)
    let view = ZirMmapView::open(zir_path)?;
    let decl = view.decls().iter().find(|d| view.get_str(d.module_name) == module && view.get_str(d.decl_name) == name).ok_or_else(|| anyhow!("Decl not found"))?;
    let tokens: Vec<String> = view.get_tokens_for(decl).map(|s| s.to_string()).collect();

    // Lightweight checks:
    // - If decl is exported (pub) -> enforce explicit type in tokens (heuristic)
    if tokens.iter().any(|t| t == "pub") {
        // missing type annotation detection heuristic:
        // if we see "fn" but no "->" in tokens, fail
        if tokens.iter().any(|t| t == "fn") && !tokens.iter().any(|t| t == "->") {
            return Err(SemaError::MissingType(view.get_str(decl.decl_name).to_string()).into());
        }
    }

    // Comptime usage check: only allowed inside comptime: blocks or when marked
    if tokens.iter().any(|t| t == "comptime") {
        // for prototype: ensure 'comptime:' label appears (strict)
        if !tokens.iter().any(|t| t == ":") {
            return Err(SemaError::ComptimeError(view.get_str(decl.decl_name).to_string()).into());
        }
    }

    // AD trigger: if 'autodiff' appears in tokens, we mark that analysis for potential later heavy work.
    if tokens.iter().any(|t| t == "autodiff") {
        info!("Declaration {}::{} references autodiff — analysis will be lazily scheduled", module, name);
        // We do not run AD generation here. We only record presence. Actual AD generation occurs when user calls sema::generate_ad_for(...)
    }

    let res = Arc::new(SemaResult {
        zir: *decl,
        checked: true,
    });

    // store into cache (get_or_init)
    let map_key = key.clone();
    let map = SEMACACHE.read();
    let cache = map.get(&map_key).unwrap();
    cache.get_or_init(|| res.clone());

    Ok(res)
}

/// 基于 ZIR node graph 的“递归”类型检查。节点逻辑在 ID 级别展开。
pub fn type_check_node(node_id: ZirNodeId, view: &ZirMmapView, cache: &mut HashMap<ZirNodeId, ZirType>) -> Result<ZirType> {
    if let Some(cached) = cache.get(&node_id) {
        return Ok(*cached);
    }

    let node = view.get_node(node_id).ok_or_else(|| anyhow!("node not found {}", node_id))?;

    let ty = match node.opcode() {
        OpCode::Const => ZirType::F64,
        OpCode::Arg => ZirType::F64,
        OpCode::Add => {
            let lhs_ty = type_check_node(node.input0, view, cache)?;
            let rhs_ty = type_check_node(node.input1, view, cache)?;
            if lhs_ty == rhs_ty {
                lhs_ty
            } else {
                return Err(SemaError::NodeError(format!("type mismatch {:?} vs {:?}", lhs_ty, rhs_ty), DeclId(node_id)).into());
            }
        }
        OpCode::Call => ZirType::Unknown,
        OpCode::Let => type_check_node(node.input0, view, cache)?,
        OpCode::Jump => ZirType::Void,
        OpCode::Return => type_check_node(node.input0, view, cache)?,
        OpCode::Nop => ZirType::Void,
    };

    cache.insert(node_id, ty);
    Ok(ty)
}



/// Example: force AD generation for a function (expensive). This will be done only when requested.
pub fn generate_ad_for(zir_path: &str, module: &str, name: &str) -> Result<()> {
    let view = ZirMmapView::open(zir_path)?;
    let decl = view.decls().iter().find(|d| view.get_str(d.module_name) == module && view.get_str(d.decl_name) == name).ok_or_else(|| anyhow!("Decl not found"))?;
    let tokens: Vec<String> = view.get_tokens_for(decl).map(|s| s.to_string()).collect();
    if !tokens.iter().any(|t| t == "autodiff") {
        warn!("AD requested for {}::{} but declaration does not reference autodiff; proceeding anyway", module, name);
    }
    // For prototype: call into autodiff module which would convert ZIR -> AIR -> MIR and produce differentiated code.
    match crate::autodiff::generate_ad_for_decl(&view, decl) {
        Ok(_) => {
            info!("AD generation completed for {}::{}", module, name);
            Ok(())
        }
        Err(e) => Err(SemaError::AutodiffError(name.to_string(), e.to_string()).into()),
    }
}