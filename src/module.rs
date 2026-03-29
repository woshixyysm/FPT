use serde::{Deserialize, Serialize};
use std::path::PathBuf;

/// Minimal package config used by this prototype.
#[derive(Debug, Clone)]
pub struct PackageConfig {
    pub name: String,
    pub version: String,
    /// file paths to module source files
    pub module_paths: Vec<PathBuf>,
}

/// Representation of a parsed module file (shallow, no AST).
#[derive(Debug, Clone)]
pub struct ParsedModule {
    pub name: String,
    pub path: PathBuf,
    /// top-level textual declarations (we keep source and metadata)
    pub declarations: Vec<Decl>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Decl {
    pub name: String,
    pub kind: DeclKind,
    /// the original source for the declaration (text)
    pub source: String,
    /// token stream (string tokens) — parser supplies
    pub tokens: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub enum DeclKind {
    Function,
    Type,
    Instance,
    Import,
    Comptime,
    Other,
}