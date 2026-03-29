//! fpt — a minimal prototype of the module/ZIR/sema pipeline described.
//! Focus: tokenize -> produce distributed ZIR (per-declaration) -> lazy Sema/AIR/MIR triggers
pub mod tokenizer;
pub mod parser;
pub mod zir;
pub mod module;
pub mod sema;
pub mod comptime;
pub mod autodiff;
pub mod codegen;
pub mod codegen_x86;
pub mod rtti;
pub mod codegen_pe_full;
pub mod codegen_elf;
pub mod executor;
pub mod control;

pub use module::PackageConfig;

use anyhow::Result;
use log::{info, warn};
use parser::parse_module_file;
use crate::zir::{ZirMmapBuilder, ZirMmapView};
use std::path::Path;

pub fn build_from_package(pkg: &PackageConfig) -> Result<()> {
    info!("Building package {} v{}", pkg.name, pkg.version);

    // 1) For each module path, parse and build ZIR mmap file
    let mut builder = ZirMmapBuilder::new();

    for p in &pkg.module_paths {
        info!("Parsing module file: {}", p.display());
        let module = parse_module_file(Path::new(p), &mut builder)?;
        for decl in module.declarations {
            info!("Added ZIR decl: {}::{}", &module.name, &decl.name);
        }
    }

    // Write the ZIR file
    builder.write_to_file("zir_cache.zir")?;
    info!("Wrote ZIR file: zir_cache.zir");

    // 3) Generate executables
    // Invoke x86 backend (will eventually lower ZIR to machine code)
    codegen_x86::generate_x86_from_zir("zir_cache.zir", &std::path::Path::new("output.exe"))?;
    info!("Generated output: output.exe (via x86 backend)");

    // 2) Lazy semantic analysis: We do NOT analyze everything now.
    // We demonstrate lazy trigger: request semantic info for one function.
    if let Some(path) = pkg.module_paths.first() {
        let mut temp_builder = ZirMmapBuilder::new();
        let parsed = parse_module_file(Path::new(path), &mut temp_builder)?;
        if let Some(d) = parsed
            .declarations
            .into_iter()
            .find(|d| d.kind == module::DeclKind::Function)
        {
            info!("Triggering semantic analysis (lazy) for {}::{}", parsed.name, d.name);
            // sema will fetch ZIR from mmap file, perform checks, but only for requested decl
            sema::analyze_declaration_lazy("zir_cache.zir", &parsed.name, &d.name)?;
        } else {
            warn!("No function decl found in first module, skipping lazy Sema demo.");
        }
    }

    Ok(())
}