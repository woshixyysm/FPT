//! x86_64 codegen backend skeleton
//!
//! This module will traverse ZIR decls and emit x86_64 machine code into a .text section.
//! For now it contains a minimal skeleton and logging hooks to expand later.

use anyhow::Result;
use log::info;
use std::path::Path;
use crate::zir::ZirMmapView;

pub fn generate_x86_from_zir(_zir_path: &str, _output: &Path) -> Result<()> {
    info!("(codegen_x86) generate_x86_from_zir called for {}", _zir_path);
    // Open the ZIR view and enumerate declarations, lowering a small set of opcodes
    let mut text: Vec<u8> = Vec::new();
    let mut rdata: Vec<u8> = Vec::new();

    // Use RttiBuilder to create a small descriptor table
    let mut rtb = crate::rtti::RttiBuilder::new();
    let f64_idx = rtb.register_primitive("f64", 8);
    let rtti_prefix = rtb.emit_rdata();
    let prefix_start = rdata.len();
    rdata.extend_from_slice(&rtti_prefix);
    let descriptor_pos_f64 = prefix_start + rtb.types()[f64_idx].desc_offset;
    let name_offset_f64 = prefix_start + rtb.types()[f64_idx].name_offset;
    let mut patches: Vec<(usize, usize)> = Vec::new(); // (pos_in_text, rdata_offset)

    if let Ok(view) = ZirMmapView::open(_zir_path) {
        for (i, decl) in view.decls().iter().enumerate() {
            let module = view.get_str(decl.module_name);
            let name = view.get_str(decl.decl_name);
            let kind = view.get_str(decl.kind);
            info!("ZIR decl[{}] = {}::{} ({}) nodes={}", i, module, name, kind, decl.node_count);

            if decl.node_count == 0 { continue; }

            // Map node ids to rdata offsets for Const and Arg
            use std::collections::HashMap;
            let mut const_map: HashMap<u32, usize> = HashMap::new();
            let mut arg_map: HashMap<u32, usize> = HashMap::new();

            let start = decl.node_start as usize;
            let end = start + decl.node_count as usize;

            for nid in start..end {
                if let Some(node) = view.get_node(nid as u32) {
                    match node.opcode() {
                        crate::zir::OpCode::Const => {
                            if !const_map.contains_key(&(nid as u32)) {
                                let off = rdata.len();
                                rdata.extend_from_slice(&node.value.to_le_bytes());
                                const_map.insert(nid as u32, off);
                            }
                        }
                        crate::zir::OpCode::Arg => {
                            if !arg_map.contains_key(&(nid as u32)) {
                                let off = rdata.len();
                                // placeholder for arg slot (8 bytes)
                                rdata.extend_from_slice(&[0u8; 8]);
                                arg_map.insert(nid as u32, off);
                            }
                        }
                        _ => {}
                    }
                }
            }

            // Lower nodes sequentially into code bytes. We'll keep results in XMM0.
            for nid in start..end {
                if let Some(node) = view.get_node(nid as u32) {
                    match node.opcode() {
                        crate::zir::OpCode::Const => {
                            // load constant into xmm0: mov rax, imm64; movsd xmm0, [rax]
                            if let Some(&r_off) = const_map.get(&(nid as u32)) {
                                // mov rax, imm64
                                text.extend_from_slice(&[0x48, 0xB8]);
                                let pos = text.len();
                                text.extend_from_slice(&[0u8; 8]);
                                patches.push((pos, r_off));
                                // movsd xmm0, [rax]
                                text.extend_from_slice(&[0xF2, 0x0F, 0x10, 0x00]);
                            }
                        }
                        crate::zir::OpCode::Arg => {
                            // load arg slot into xmm0 similar to const
                            if let Some(&r_off) = arg_map.get(&(nid as u32)) {
                                text.extend_from_slice(&[0x48, 0xB8]);
                                let pos = text.len();
                                text.extend_from_slice(&[0u8; 8]);
                                patches.push((pos, r_off));
                                text.extend_from_slice(&[0xF2, 0x0F, 0x10, 0x00]);
                            }
                        }
                        crate::zir::OpCode::Add => {
                            // binary add: load lhs into xmm0, rhs into xmm1, addsd xmm0,xmm1
                            let a = node.input0 as usize;
                            let b = node.input1 as usize;
                            // For simplicity assume inputs were constants or args already placed in rdata_map
                            if let Some(&a_off) = const_map.get(&(a as u32)).or_else(|| arg_map.get(&(a as u32))) {
                                // mov rax, imm64; movsd xmm0, [rax]
                                text.extend_from_slice(&[0x48, 0xB8]);
                                let pos = text.len();
                                text.extend_from_slice(&[0u8;8]);
                                patches.push((pos, a_off));
                                text.extend_from_slice(&[0xF2,0x0F,0x10,0x00]);
                            }
                            if let Some(&b_off) = const_map.get(&(b as u32)).or_else(|| arg_map.get(&(b as u32))) {
                                // mov rax, imm64; movsd xmm1, [rax]
                                text.extend_from_slice(&[0x48, 0xB8]);
                                let pos = text.len();
                                text.extend_from_slice(&[0u8;8]);
                                patches.push((pos, b_off));
                                text.extend_from_slice(&[0xF2,0x0F,0x10,0x08]);
                            }
                            // addsd xmm0, xmm1
                            text.extend_from_slice(&[0xF2, 0x0F, 0x58, 0xC1]);
                        }
                        crate::zir::OpCode::Return => {
                            // return; assume value is in xmm0; emit ret
                            text.push(0xC3);
                        }
                        _ => {
                            // unhandled opcode -> nop
                            text.push(0x90);
                        }
                    }
                }
            }
        }
    }

    // If nothing produced, emit a ret
    if text.is_empty() {
        text.push(0xC3);
    }

    // Patch movabs imm64 placeholders with computed VA pointing into section (text followed by rdata)
    let image_base: u64 = 0x140000000u64;
    let text_va_base: u64 = 0x1000u64;
    let text_size = text.len() as u64;
    for (pos, r_off) in patches {
        let const_va = image_base + text_va_base + text_size + (r_off as u64);
        let imm_bytes = const_va.to_le_bytes();
        for j in 0..8 {
            text[pos + j] = imm_bytes[j];
        }
    }

    // Fill RTTI descriptor table with actual RVAs for the current layout.
    let text_virtual_size = ((text.len() + 0xFFF) / 0x1000) * 0x1000;
    let rdata_section_rva = 0x1000u64 + text_virtual_size as u64;
    rtb.write_descriptors(&mut rdata[prefix_start..], image_base, rdata_section_rva);

    // Emit PE using the combined blobs via the more complete PE writer
    crate::codegen_pe_full::write_pe(_output, &text, &rdata, 0x1000)?;
    Ok(())
}
