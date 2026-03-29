use anyhow::Result;
use std::path::PathBuf;
use fpt::{build_from_package, PackageConfig};

#[test]
fn build_and_inspect_output() -> Result<()> {
    // Build package with examples/simple.fp
    let mut pkg = PackageConfig { name: "testpkg".into(), version: "0.1.0".into(), module_paths: vec![] };
    let base = std::env::current_dir()?;
    let example = base.join("examples/simple.fp");
    pkg.module_paths.push(example);

    // Run build pipeline
    build_from_package(&pkg)?;

    // Inspect output.exe
    let out = PathBuf::from("output.exe");
    assert!(out.exists(), "output.exe was not generated");
    let data = std::fs::read(out)?;

    // Check RTTI string "f64\0" present
    assert!(data.windows(4).any(|w| w == b"f64\0"), "RTTI 'f64' not found in output.exe");

    // Check constant 1.0 bytes (little-endian f64)
    let one_bytes = 1.0f64.to_le_bytes();
    assert!(data.windows(8).any(|w| w == one_bytes), "constant 1.0 bytes not found in output.exe");

    Ok(())
}
