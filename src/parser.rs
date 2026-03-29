use crate::module::{ParsedModule, Decl, DeclKind};
use crate::tokenizer::tokenize;
use crate::zir::{ZirMmapBuilder, ZirNodeId};
use anyhow::Result;
use std::collections::HashMap;
use std::fs;
use std::path::Path;

/// 从 Token list 创建一个表达式图，并写入 ZirMmapBuilder。支持 x+n、let、call、数字字面量。
pub fn parse_expr(tokens: &[String], builder: &mut ZirMmapBuilder) -> ZirNodeId {
    let mut variables: HashMap<String, ZirNodeId> = HashMap::new();
    let mut arg_index = 0;
    parse_expr_internal(tokens, builder, &mut variables, &mut arg_index)
}

fn parse_expr_internal(tokens: &[String], builder: &mut ZirMmapBuilder, variables: &mut HashMap<String, ZirNodeId>, arg_index: &mut u32) -> ZirNodeId {
    if tokens.is_empty() {
        return builder.add_node_const(0.0);
    }

    // let binding: let x = expr; rest
    if tokens.len() >= 4 && tokens[0] == "let" && tokens[2] == "=" {
        let name = tokens[1].clone();
        // find semicolon or end
        let mut end = tokens.len();
        if let Some(semicolon_pos) = tokens.iter().position(|t| t == ";") {
            end = semicolon_pos;
        }

        let rhs = parse_expr_internal(&tokens[3..end], builder, variables, arg_index);
        let binding = builder.add_node_let(&name, rhs);
        variables.insert(name.clone(), binding);

        if end + 1 < tokens.len() {
            return parse_expr_internal(&tokens[end + 1..], builder, variables, arg_index);
        }

        return binding;
    }

    // function call: name(arg1,arg2,...)
    if tokens.len() >= 4 && tokens[1] == "(" && tokens.last().map(String::as_str) == Some(")") {
        let name = tokens[0].clone();
        let mut args = vec![];
        let mut arg_tokens = vec![];
        let mut paren_depth = 0;
        for t in tokens.iter().skip(2).take(tokens.len() - 3) {
            if t == "," && paren_depth == 0 {
                if !arg_tokens.is_empty() {
                    let arg_node = parse_expr_internal(&arg_tokens, builder, variables, arg_index);
                    args.push(arg_node);
                    arg_tokens.clear();
                }
            } else {
                if t == "(" {
                    paren_depth += 1;
                }
                if t == ")" && paren_depth > 0 {
                    paren_depth -= 1;
                }
                arg_tokens.push(t.clone());
            }
        }
        if !arg_tokens.is_empty() {
            let arg_node = parse_expr_internal(&arg_tokens, builder, variables, arg_index);
            args.push(arg_node);
        }
        return builder.add_node_call(&name, &args);
    }

    // binary add (left-associative)
    if let Some(idx) = tokens.iter().position(|t| t == "+") {
        let lhs_node = parse_expr_internal(&tokens[..idx], builder, variables, arg_index);
        let rhs_node = parse_expr_internal(&tokens[idx+1..], builder, variables, arg_index);
        return builder.add_node_add(lhs_node, rhs_node);
    }

    // parenthesized expression
    if tokens.first() == Some(&"(".to_string()) && tokens.last() == Some(&")".to_string()) {
        return parse_expr_internal(&tokens[1..tokens.len()-1], builder, variables, arg_index);
    }

    // number literal
    if tokens.len() == 1 {
        if let Ok(v) = tokens[0].parse::<f64>() {
            return builder.add_node_const(v);
        }
        if let Some(&id) = variables.get(&tokens[0]) {
            return id;
        }
        // unknown name -> treat as argument placeholder node (for x, n etc.)
        let arg_id = builder.add_node_arg(*arg_index);
        *arg_index += 1;
        variables.insert(tokens[0].clone(), arg_id);
        return arg_id;
    }

    // fallback
    builder.add_node_const(0.0)
}

/// 解析模块并直接构建 ZIR 结构。
/// Decl 依然返回，但也将相关表达式节点写入 builder。
pub fn parse_module_file<P: AsRef<Path>>(path: P, builder: &mut ZirMmapBuilder) -> Result<ParsedModule> {
    let source = fs::read_to_string(&path)?;
    let tokens = tokenize(&source);

    // Extract module name from path (simplified)
    let module_name = path.as_ref().file_stem().unwrap().to_str().unwrap().to_string();

    // Split declarations: look for 'fn', 'type', etc. at top level
    let mut declarations = Vec::new();
    let mut current_decl = Vec::new();
    let mut in_decl = false;

    for token in tokens {
        if matches!(token.as_str(), "fn" | "type" | "instance" | "import" | "comptime") {
            if in_decl {
                if let Some(decl) = parse_decl(&current_decl) {
                    let (node_start, node_count) = emit_decl_nodes(&module_name, &decl, builder);
                    builder.add_decl(
                        &module_name,
                        &decl.name,
                        &format!("{:?}", decl.kind),
                        &decl.tokens,
                        (0, 0),
                        0,
                        &[],
                        node_start,
                        node_count,
                    );
                    declarations.push(decl);
                }
                current_decl.clear();
            }
            in_decl = true;
        }
        if in_decl {
            current_decl.push(token);
        }
    }

    if in_decl && !current_decl.is_empty() {
        if let Some(decl) = parse_decl(&current_decl) {
            let (node_start, node_count) = emit_decl_nodes(&module_name, &decl, builder);
            builder.add_decl(
                &module_name,
                &decl.name,
                &format!("{:?}", decl.kind),
                &decl.tokens,
                (0, 0),
                0,
                &[],
                node_start,
                node_count,
            );
            declarations.push(decl);
        }
    }

    Ok(ParsedModule {
        name: module_name,
        path: path.as_ref().to_path_buf(),
        declarations,
    })
}

fn extract_fn_body_and_args(tokens: &[String]) -> (Vec<String>, Vec<String>) {
    let mut args = Vec::new();
    let mut body = Vec::new();

    if tokens.len() < 2 {
        return (args, body);
    }

    // find argument list
    let mut i = 2;
    if i < tokens.len() && tokens[i] == "(" {
        i += 1;
        while i < tokens.len() && tokens[i] != ")" {
            if tokens[i] != "," {
                args.push(tokens[i].clone());
            }
            i += 1;
        }
        if i < tokens.len() && tokens[i] == ")" {
            i += 1;
        }
    }

    // find body: support both brace-delimited `{ ... }` and ::-delimited `:: ... ::`
    if i < tokens.len() && tokens[i] == "{" {
        // old-style brace block
        i += 1;
        let mut depth = 1;
        while i < tokens.len() && depth > 0 {
            if tokens[i] == "{" {
                depth += 1;
            } else if tokens[i] == "}" {
                depth -= 1;
                if depth == 0 {
                    break;
                }
            }

            if depth > 0 {
                body.push(tokens[i].clone());
            }
            i += 1;
        }
    } else if i < tokens.len() && tokens[i] == "::" {
        // new-style :: delimited blocks. Support nesting: an opener :: that
        // follows a block-introducing keyword (fn, repeat, if, else, comptime)
        // increases depth; a bare :: acts as a closer.
        i += 1; // skip the initial ::
        let mut depth: i32 = 1;
        while i < tokens.len() && depth > 0 {
            if tokens[i] == "::" {
                // look back for a previous non-empty token to see if this ::
                // functions as an opener (follows a block-start keyword)
                let mut prev = i as isize - 1;
                let mut prev_tok = "";
                while prev >= 0 {
                    prev_tok = &tokens[prev as usize];
                    if !prev_tok.is_empty() { break; }
                    prev -= 1;
                }
                match prev_tok {
                    "fn" | "repeat" | "if" | "else" | "comptime" => {
                        // opener for a nested block
                        depth += 1;
                        i += 1; // do not include the :: marker itself
                        continue;
                    }
                    _ => {
                        // closing marker
                        depth -= 1;
                        i += 1;
                        if depth == 0 { break; }
                        continue;
                    }
                }
            }

            // regular token inside the body
            body.push(tokens[i].clone());
            i += 1;
        }
    }

    (args, body)
}

fn emit_decl_nodes(module_name: &str, decl: &Decl, builder: &mut ZirMmapBuilder) -> (u32, u32) {
    let node_start = builder.next_node_id();
    let mut node_count = 0;

    if matches!(decl.kind, DeclKind::Function | DeclKind::Comptime) {
        let (args, body_tokens) = extract_fn_body_and_args(&decl.tokens);
        let mut variables: HashMap<String, ZirNodeId> = HashMap::new();

        for (index, arg) in args.iter().enumerate() {
            let arg_node = builder.add_node_arg(index as u32);
            variables.insert(arg.clone(), arg_node);
        }

        let mut arg_index = args.len() as u32;
        let root = if body_tokens.is_empty() {
            builder.add_node_const(0.0)
        } else {
            parse_expr_internal(&body_tokens, builder, &mut variables, &mut arg_index)
        };

        let _ret = builder.add_node_return(root);
        node_count = builder.next_node_id() - node_start;
    }

    (node_start, node_count)
}

fn parse_decl(tokens: &[String]) -> Option<Decl> {
    if tokens.is_empty() {
        return None;
    }

    let kind = match tokens[0].as_str() {
        "fn" => DeclKind::Function,
        "type" => DeclKind::Type,
        "instance" => DeclKind::Instance,
        "import" => DeclKind::Import,
        "comptime" => DeclKind::Comptime,
        _ => DeclKind::Other,
    };

    let name = tokens.get(1).cloned().unwrap_or_else(|| "unknown".to_string());
    let source = tokens.join(" ");

    Some(Decl {
        name,
        kind,
        source,
        tokens: tokens.to_vec(),
    })
}