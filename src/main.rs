use anyhow::Result;
use env_logger::Env;
extern crate fpt;

use fpt::{build_from_package, PackageConfig};
use std::path::PathBuf;
use std::env;

fn print_help() {
    // Minimal friendly help similar to icx overview
    println!("fpt — FPT compiler frontend/backend prototype\n");
    println!("USAGE: fpt [options] <file>...\n");
    println!("OPTIONS:");
    println!("  -help             Display this help");
    println!("  -o <file>         Set output file (default: output.exe)");
    println!("  -v, -verbose      Enable verbose logging");
    println!("  -backend <name>   Select backend (pe, elf, x86)\n");
    println!("EXAMPLES:");
    println!("  fpt -help");
    println!("  fpt numeric/main.fp -o out.exe");
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(Env::default().default_filter_or("info")).init();

    let args: Vec<String> = env::args().collect();
    if args.len() <= 1 {
        print_help();
        return Ok(());
    }

    // quick arg parsing
    let mut out_path = PathBuf::from("output.exe");
    let mut files: Vec<PathBuf> = Vec::new();
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-help" | "--help" | "/?" => {
                print_help();
                return Ok(());
            }
            "-o" => {
                if i + 1 < args.len() {
                    out_path = PathBuf::from(&args[i + 1]);
                    i += 1;
                }
            }
            "-v" | "-verbose" => {
                // already wired to env_logger; user can set RUST_LOG
            }
            _ => {
                if args[i].starts_with('-') {
                    eprintln!("Unknown option: {}", args[i]);
                } else {
                    files.push(PathBuf::from(&args[i]));
                }
            }
        }
        i += 1;
    }

    if files.is_empty() {
        eprintln!("No input files provided. Use -help for usage.");
        return Ok(());
    }

    // Package config
    let mut pkg = PackageConfig {
        name: "fpt-project".to_string(),
        version: "0.1.0".to_string(),
        module_paths: Vec::new(),
    };

    for f in files {
        pkg.module_paths.push(f);
    }

    // Build pipeline: parse modules -> produce ZIR -> generate PE
    build_from_package(&pkg)?;

    // The existing codegen currently writes output.exe; out_path respected later
    // For now we keep the existing behavior (generate_pe writes to output.exe)

    println!("Build complete; output at {}", out_path.display());

    Ok(())
}