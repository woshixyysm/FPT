use crate::zir::ZirMmapView;
use anyhow::Result;
use std::fs::File;
use std::io::Write;
use std::path::Path;

/// Generate a PE executable from ZIR
pub fn generate_pe(_zir_path: &str, output_path: &Path) -> Result<()> {
    // Default fallback: produce a trivial PE with a single `ret` in .text
    generate_pe_with_sections(output_path, &[0xC3], &[])
}

pub fn generate_pe_with_sections<P: AsRef<Path>>(output_path: P, text: &[u8], rdata: &[u8]) -> Result<()> {
    let mut file = File::create(output_path)?;

    // DOS Header
    file.write_all(b"MZ")?;
    file.write_all(&[0u8; 58])?;
    file.write_all(&0x40u32.to_le_bytes())?; // e_lfanew

    // PE Signature
    file.write_all(b"PE\x00\x00")?;

    // COFF Header
    file.write_all(&0x8664u16.to_le_bytes())?; // Machine = AMD64
    file.write_all(&1u16.to_le_bytes())?; // NumberOfSections = 1
    file.write_all(&0u32.to_le_bytes())?; // TimeDateStamp
    file.write_all(&0u32.to_le_bytes())?; // PointerToSymbolTable
    file.write_all(&0u32.to_le_bytes())?; // NumberOfSymbols
    file.write_all(&240u16.to_le_bytes())?; // SizeOfOptionalHeader
    file.write_all(&0x0022u16.to_le_bytes())?; // Characteristics

    // Optional Header (PE32+)
    file.write_all(&0x020bu16.to_le_bytes())?; // Magic
    file.write_all(&1u8.to_le_bytes())?; // MajorLinkerVersion
    file.write_all(&0u8.to_le_bytes())?; // MinorLinkerVersion

    let combined_size = ((text.len() + rdata.len()) as u32).max(1);
    file.write_all(&combined_size.to_le_bytes())?; // SizeOfCode
    file.write_all(&0u32.to_le_bytes())?; // SizeOfInitializedData
    file.write_all(&0u32.to_le_bytes())?; // SizeOfUninitializedData

    file.write_all(&0x1000u32.to_le_bytes())?; // AddressOfEntryPoint
    file.write_all(&0x1000u32.to_le_bytes())?; // BaseOfCode
    file.write_all(&0x140000000u64.to_le_bytes())?; // ImageBase
    file.write_all(&0x1000u32.to_le_bytes())?; // SectionAlignment
    file.write_all(&0x200u32.to_le_bytes())?; // FileAlignment

    file.write_all(&6u16.to_le_bytes())?; // MajorOS
    file.write_all(&0u16.to_le_bytes())?; // MinorOS
    file.write_all(&0u16.to_le_bytes())?; // MajorImage
    file.write_all(&0u16.to_le_bytes())?; // MinorImage
    file.write_all(&6u16.to_le_bytes())?; // MajorSubsystem
    file.write_all(&0u16.to_le_bytes())?; // MinorSubsystem
    file.write_all(&0u32.to_le_bytes())?; // Win32VersionValue

    file.write_all(&0x2000u32.to_le_bytes())?; // SizeOfImage
    file.write_all(&0x400u32.to_le_bytes())?; // SizeOfHeaders

    file.write_all(&0u32.to_le_bytes())?; // CheckSum
    file.write_all(&3u16.to_le_bytes())?; // Subsystem = CONSOLE
    file.write_all(&0x8160u16.to_le_bytes())?; // DllCharacteristics
    file.write_all(&0x100000u64.to_le_bytes())?; // SizeOfStackReserve
    file.write_all(&0x1000u64.to_le_bytes())?; // SizeOfStackCommit
    file.write_all(&0x100000u64.to_le_bytes())?; // SizeOfHeapReserve
    file.write_all(&0x1000u64.to_le_bytes())?; // SizeOfHeapCommit
    file.write_all(&0u32.to_le_bytes())?; // LoaderFlags
    file.write_all(&16u32.to_le_bytes())?; // NumberOfRvaAndSizes

    // Data directories
    for _ in 0..16 { file.write_all(&0u64.to_le_bytes())?; }

    // Section Header (.text)
    file.write_all(b".text\x00\x00\x00")?;
    file.write_all(&0x200u32.to_le_bytes())?; // VirtualSize
    file.write_all(&0x1000u32.to_le_bytes())?; // VirtualAddress
    file.write_all(&0x200u32.to_le_bytes())?; // SizeOfRawData
    file.write_all(&0x400u32.to_le_bytes())?; // PointerToRawData
    file.write_all(&0u32.to_le_bytes())?; // PointerToRelocations
    file.write_all(&0u32.to_le_bytes())?; // PointerToLinenumbers
    file.write_all(&0u16.to_le_bytes())?; // NumberOfRelocations
    file.write_all(&0u16.to_le_bytes())?; // NumberOfLinenumbers
    file.write_all(&0x60000020u32.to_le_bytes())?; // Characteristics

    // Pad to file alignment (0x400)
    let current_pos = 0x170;
    let target_pos = 0x400;
    if target_pos > current_pos { file.write_all(&vec![0u8; target_pos - current_pos])?; }

    // Combine text+rdata and pad to SizeOfRawData
    let mut section_data = Vec::new();
    section_data.extend_from_slice(text);
    section_data.extend_from_slice(rdata);
    if section_data.len() > 0x200 { section_data.truncate(0x200); }
    if section_data.len() < 0x200 { section_data.extend_from_slice(&vec![0u8; 0x200 - section_data.len()]); }
    file.write_all(&section_data)?;

    Ok(())
}