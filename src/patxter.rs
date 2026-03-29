//! Parser that does NOT construct a full AST.
//! Instead it finds top-level declarations and emits Decl objects containing raw source and token lists.
//! This implements the "no global AST, distributed ZIR per declaration" philosophy.

use crate::module::{Decl, DeclKind, ParsedModule};
use crate::tokenizer::{leading_spaces, tokenize};
use anyhow::{anyhow, Result};
use std::fs;
use std::path::Path;

/// Heuristic: top-level decl starts with "fn", "type", "instance", "pub module", "import"
pub fn parse_module_file(path: &Path) -> Result<ParsedModule> {
    let text = fs::read_to_string(path)?;
    // module name heuristic: file stem
    let name = path
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("unknown")
        .to_string();

    let mut declarations = Vec::new();

    // Split source into lines and group top-level declarations by indentation.
    // Off-side: top-level has indent 0.
    let mut lines = text.lines().enumerate().peekable();
    while let Some((idx, line)) = lines.next() {
        let trimmed = line.trim_start();
        if trimmed.is_empty() {
            continue;
        }
        let indent = leading_spaces(line);
        if indent != 0 {
            // skip inner lines until we've handled a previous top-level decl
            continue;
        }

        // Detect start tokens
        let starts = vec!["fn ", "type ", "instance ", "pub module", "module ", "import "];
        let is_decl_start = starts.iter().any(|s| trimmed.starts_with(s));
        if !is_decl_start {
            // skip top-level non-decl (could be comments or pragmas)
            continue;
        }

        // capture this declaration block: include following indented lines
        let mut decl_lines = vec![line.to_string()];
        while let Some((_, next_line)) = lines.peek() {
            let next_indent = leading_spaces(next_line);
            if next_indent > 0 {
                // included
                let (_, l) = lines.next().unwrap();
                decl_lines.push(l.to_string());
            } else {
                break;
            }
        }

        let decl_src = decl_lines.join("\n");
        let tokens = tokenize(&decl_src);

        // Name heuristic
        let name = extract_decl_name(&tokens).unwrap_or_else(|| "anon".to_string());
        let kind = detect_kind(&tokens);

        declarations.push(Decl {
            name,
            kind,
            source: decl_src,
            tokens,
        });
    }

    Ok(ParsedModule {
        name,
        path: path.to_path_buf(),
        declarations,
    })
}

fn detect_kind(tokens: &[String]) -> DeclKind {
    if tokens.is_empty() {
        return DeclKind::Other;
    }
    match tokens[0].as_str() {
        "fn" => DeclKind::Function,
        "type" => DeclKind::Type,
        "instance" => DeclKind::Instance,
        "import" => DeclKind::Import,
        "pub" => {
            if tokens.len() > 1 && tokens[1] == "module" {
                DeclKind::Other
            } else {
                DeclKind::Other
            }
        }
        _ => DeclKind::Other,
    }
}

fn extract_decl_name(tokens: &[String]) -> Option<String> {
    // Very small heuristic: after 'fn' comes the name, or after 'type' etc.
    if tokens.len() < 2 {
        return None;
    }
    match tokens[0].as_str() {
        "fn" => Some(tokens[1].clone()),
        "type" => Some(tokens[1].clone()),
        "instance" => {
            if tokens.len() >= 3 {
                Some(tokens[2].clone())
            } else {
                None
            }
        }
        _ => None,
    }
}