//! More complete PE writer for prototyping emitters.
//!
//! This module provides functions to write a PE image given prepared `.text` and
//! `.rdata` blobs and an entrypoint RVA. It intentionally keeps relocations and
//! imports minimal for now but computes sizes and alignments correctly.

use anyhow::Result;
use std::fs::File;
use std::io::{Write, Seek};
use std::path::Path;

fn align_up(x: usize, align: usize) -> usize {
    ((x + align - 1) / align) * align
}

pub fn write_pe<P: AsRef<Path>>(out: P, text: &[u8], rdata: &[u8], entry_rva: u32) -> Result<()> {
    let mut file = File::create(out)?;

    // Constants
    let file_alignment = 0x200usize;
    let section_alignment = 0x1000usize;
    let image_base: u64 = 0x140000000u64;

    // Headers sizes
    let dos_stub_size = 0x40usize;
    let coff_header_size = 20usize;
    let optional_header_size = 240usize; // PE32+
    let section_header_size = 40usize;
    let num_sections = 1usize;

    let headers_size = dos_stub_size + 4 + coff_header_size + optional_header_size + (section_header_size * num_sections);
    let size_of_headers = align_up(headers_size, file_alignment);

    // Section raw sizes
    let text_raw_size = align_up(text.len(), file_alignment);
    let rdata_raw_size = align_up(rdata.len(), file_alignment);

    let pointer_to_raw_data = size_of_headers as u32;
    let text_virtual_size = align_up(text.len(), section_alignment);
    let rdata_virtual_size = align_up(rdata.len(), section_alignment);

    let size_of_image = align_up(0x1000 + text_virtual_size + rdata_virtual_size, section_alignment) as u32;

    // --- DOS Header (MZ) ---
    file.write_all(b"MZ")?;
    file.write_all(&vec![0u8; dos_stub_size - 2])?;
    // e_lfanew = 0x40
    file.seek(std::io::SeekFrom::Start(0x3C))?;
    file.write_all(&0x40u32.to_le_bytes())?;
    file.seek(std::io::SeekFrom::Start(dos_stub_size as u64))?;

    // PE Signature
    file.write_all(b"PE\0\0")?;

    // COFF Header
    file.write_all(&0x8664u16.to_le_bytes())?; // Machine AMD64
    file.write_all(&(num_sections as u16).to_le_bytes())?; // NumberOfSections
    file.write_all(&0u32.to_le_bytes())?; // TimeDateStamp
    file.write_all(&0u32.to_le_bytes())?; // PointerToSymbolTable
    file.write_all(&0u32.to_le_bytes())?; // NumberOfSymbols
    file.write_all(&(optional_header_size as u16).to_le_bytes())?; // SizeOfOptionalHeader
    file.write_all(&0x0022u16.to_le_bytes())?; // Characteristics

    // Optional Header (PE32+)
    file.write_all(&0x20Bu16.to_le_bytes())?; // Magic
    file.write_all(&1u8.to_le_bytes())?; // MajorLinkerVersion
    file.write_all(&0u8.to_le_bytes())?; // MinorLinkerVersion
    file.write_all(&(text.len() as u32).to_le_bytes())?; // SizeOfCode
    file.write_all(&0u32.to_le_bytes())?; // SizeOfInitializedData
    file.write_all(&0u32.to_le_bytes())?; // SizeOfUninitializedData
    file.write_all(&entry_rva.to_le_bytes())?; // AddressOfEntryPoint
    file.write_all(&0x1000u32.to_le_bytes())?; // BaseOfCode
    file.write_all(&image_base.to_le_bytes())?; // ImageBase (8 bytes)
    file.write_all(&(section_alignment as u32).to_le_bytes())?; // SectionAlignment
    file.write_all(&(file_alignment as u32).to_le_bytes())?; // FileAlignment
    file.write_all(&6u16.to_le_bytes())?; // MajorOS
    file.write_all(&0u16.to_le_bytes())?; // MinorOS
    file.write_all(&0u16.to_le_bytes())?; // MajorImage
    file.write_all(&0u16.to_le_bytes())?; // MinorImage
    file.write_all(&6u16.to_le_bytes())?; // MajorSubsystem
    file.write_all(&0u16.to_le_bytes())?; // MinorSubsystem
    file.write_all(&0u32.to_le_bytes())?; // Win32Version
    file.write_all(&size_of_image.to_le_bytes())?; // SizeOfImage
    file.write_all(&(size_of_headers as u32).to_le_bytes())?; // SizeOfHeaders
    file.write_all(&0u32.to_le_bytes())?; // CheckSum
    file.write_all(&3u16.to_le_bytes())?; // Subsystem = CONSOLE
    file.write_all(&0x8160u16.to_le_bytes())?; // DllCharacteristics
    file.write_all(&0x100000u64.to_le_bytes())?; // SizeOfStackReserve
    file.write_all(&0x1000u64.to_le_bytes())?; // SizeOfStackCommit
    file.write_all(&0x100000u64.to_le_bytes())?; // SizeOfHeapReserve
    file.write_all(&0x1000u64.to_le_bytes())?; // SizeOfHeapCommit
    file.write_all(&0u32.to_le_bytes())?; // LoaderFlags
    file.write_all(&16u32.to_le_bytes())?; // NumberOfRvaAndSizes

    // Data directories (zero)
    for _ in 0..16 { file.write_all(&0u64.to_le_bytes())?; }

    // Section header: .text
    file.write_all(b".text\0\0\0")?;
    file.write_all(&(text_virtual_size as u32).to_le_bytes())?; // VirtualSize
    file.write_all(&0x1000u32.to_le_bytes())?; // VirtualAddress
    file.write_all(&(text_raw_size as u32).to_le_bytes())?; // SizeOfRawData
    file.write_all(&pointer_to_raw_data.to_le_bytes())?; // PointerToRawData
    file.write_all(&0u32.to_le_bytes())?; // PointerToRelocations
    file.write_all(&0u32.to_le_bytes())?; // PointerToLinenumbers
    file.write_all(&0u16.to_le_bytes())?; // NumberOfRelocations
    file.write_all(&0u16.to_le_bytes())?; // NumberOfLinenumbers
    file.write_all(&0x60000020u32.to_le_bytes())?; // Characteristics

    // Pad to start of raw data
    let current = file.stream_position()? as usize;
    if pointer_to_raw_data as usize > current {
        file.write_all(&vec![0u8; pointer_to_raw_data as usize - current])?;
    }

    // Write text raw data (aligned)
    file.write_all(text)?;
    if text.len() < text_raw_size { file.write_all(&vec![0u8; text_raw_size - text.len()])?; }

    // Next section raw data (rdata) appended after text_raw
    file.write_all(rdata)?;
    if rdata.len() < rdata_raw_size { file.write_all(&vec![0u8; rdata_raw_size - rdata.len()])?; }

    Ok(())
}
