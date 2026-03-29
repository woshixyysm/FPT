//! Very small whitespace-sensitive tokenizer tuned for top-level declaration splitting.
//! Purpose: produce token streams and top-level decl boundaries without building AST.

use regex::Regex;

pub fn tokenize(source: &str) -> Vec<String> {
    // For prototype: split by whitespace and punctuation tokens of interest.
    // Keep identifiers, numbers, punctuation as separate tokens.
    // A production tokenizer would handle strings, comments, indentation tokens, etc.
    let token_re = Regex::new(
        r##"[A-Za-z_][A-Za-z0-9_]*|\d+\.\d+|\d+|==|<=|>=|!=|->|\*\*|::|[{}()\[\],.:;+*/%<>='"-]"##
    ).unwrap();
    let mut toks = Vec::new();
    for cap in token_re.captures_iter(source) {
        toks.push(cap.get(0).unwrap().as_str().to_string());
    }
    toks
}

/// Simple helper to find the indentation (off-side rule) of a line (count spaces).
pub fn leading_spaces(line: &str) -> usize {
    line.chars().take_while(|c| *c == ' ').count()
}