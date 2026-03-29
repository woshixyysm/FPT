//! Comptime evaluator with budget/limits.
//! This is a sandbox that executes a restricted subset of computations for compile-time evaluation.
//! In this prototype we simulate limits and provide an API to run a closure with a CPU/time budget.
//! Real implementation would require a proper interpreter with resource accounting.

use crate::zir::{ZirMmapView, ZirMmapBuilder, ZirDeclRecord, DeclId};
use anyhow::{anyhow, Result};
use log::info;
use std::time::{Duration, Instant};

/// Comptime resource limits
#[derive(Debug, Clone)]
pub struct ComptimeLimits {
    pub max_duration: Duration,
    pub max_steps: u64,
}

impl Default for ComptimeLimits {
    fn default() -> Self {
        ComptimeLimits {
            max_duration: Duration::from_millis(200), // 200ms by default
            max_steps: 10_000,
        }
    }
}

/// Run a closure in comptime with limits. The closure should be deterministic and side-effect free.
/// We simulate step counting by incrementing a counter inside the closure (user provided).
pub fn run_comptime<F, T>(limits: ComptimeLimits, mut f: F) -> Result<T>
where
    F: FnMut(&mut u64) -> Result<T>,
{
    let start = Instant::now();
    let mut steps = 0u64;
    let res = f(&mut steps)?;

    let elapsed = start.elapsed();
    if elapsed > limits.max_duration || steps > limits.max_steps {
        return Err(anyhow!(
            "comptime evaluation exceeded resource limits (elapsed: {:?}, steps: {}, max: {:?}/{})",
            elapsed,
            steps,
            limits.max_duration,
            limits.max_steps
        ));
    }

    info!("comptime finished in {:?}, steps={}", elapsed, steps);
    Ok(res)
}

/// Comptime context that allows accessing ZIR object graph and generating new nodes
pub struct ComptimeContext<'a> {
    pub zir_view: &'a ZirMmapView,
    pub builder: &'a mut ZirMmapBuilder,
    pub steps: &'a mut u64,
}

impl<'a> ComptimeContext<'a> {
    /// Access a ZIR node by DeclId
    pub fn get_decl(&self, id: DeclId) -> Option<&ZirDeclRecord> {
        self.zir_view.decls().get(id.0 as usize)
    }

    /// Generate a new ZIR node (constant)
    pub fn add_const(&mut self, value: f64) -> crate::zir::ZirNodeId {
        self.builder.add_node_const(value)
    }

    pub fn add_add(&mut self, lhs: crate::zir::ZirNodeId, rhs: crate::zir::ZirNodeId) -> crate::zir::ZirNodeId {
        self.builder.add_node_add(lhs, rhs)
    }

    pub fn add_block(&mut self, nodes: Vec<crate::zir::ZirNode>) -> (crate::zir::ZirNodeId, u32) {
        let start = self.builder.next_node_id();
        for n in nodes {
            self.builder.push_node(n);
        }
        let count = self.builder.next_node_id() - start;
        (start, count)
    }

    /// Generate a new declaration record referencing graph range.
    pub fn generate_decl_with_block(
        &mut self,
        module: &str,
        name: &str,
        kind: &str,
        tokens: &[String],
        span: (u32, u32),
        source_file_offset: u32,
        dependencies: &[DeclId],
        block_start: crate::zir::ZirNodeId,
        block_count: u32,
    ) -> DeclId {
        let id = self.builder.next_decl_id();
        self.builder.add_decl(module, name, kind, tokens, span, source_file_offset, dependencies, block_start, block_count);
        id
    }

    /// Check if we've exceeded steps
    pub fn check_limits(&self, limits: &ComptimeLimits) -> Result<()> {
        if *self.steps > limits.max_steps {
            return Err(anyhow!("comptime steps exceeded"));
        }
        Ok(())
    }
}

/// Run comptime with ZIR access
pub fn run_comptime_with_zir<F, T>(limits: ComptimeLimits, zir_view: &ZirMmapView, builder: &mut ZirMmapBuilder, mut f: F) -> Result<T>
where
    F: FnMut(&mut ComptimeContext) -> Result<T>,
{
    let start = Instant::now();
    let mut steps = 0u64;
    let mut ctx = ComptimeContext {
        zir_view,
        builder,
        steps: &mut steps,
    };
    let res = f(&mut ctx)?;

    let elapsed = start.elapsed();
    if elapsed > limits.max_duration || steps > limits.max_steps {
        return Err(anyhow!(
            "comptime evaluation exceeded resource limits (elapsed: {:?}, steps: {}, max: {:?}/{})",
            elapsed,
            steps,
            limits.max_duration,
            limits.max_steps
        ));
    }

    info!("comptime with ZIR finished in {:?}, steps={}", elapsed, steps);
    Ok(res)
}