//! Minimal ELF64 writer skeleton for prototyping.
//! Emits a simple ELF with a single code segment and an `_start` that `ret`s.

use anyhow::Result;
use std::fs::File;
use std::io::Write;
use std::path::Path;

pub fn write_elf64<P: AsRef<Path>>(out: P, text: &[u8], data: &[u8]) -> Result<()> {
    let mut file = File::create(out)?;

    // Very small ELF header for prototyping; not production-ready.
    // e_ident (16)
    let mut ident = [0u8; 16];
    ident[0] = 0x7f;
    ident[1] = b'E';
    ident[2] = b'L';
    ident[3] = b'F';
    ident[4] = 2; // EI_CLASS = ELF64
    ident[5] = 1; // EI_DATA = little endian
    ident[6] = 1; // EI_VERSION
    file.write_all(&ident)?;

    // e_type, e_machine, e_version
    file.write_all(&2u16.to_le_bytes())?; // ET_EXEC
    file.write_all(&0x3Eu16.to_le_bytes())?; // EM_X86_64
    file.write_all(&1u32.to_le_bytes())?; // EV_CURRENT

    // e_entry, e_phoff, e_shoff
    file.write_all(&0u64.to_le_bytes())?; // e_entry (placeholder)
    file.write_all(&0u64.to_le_bytes())?; // e_phoff
    file.write_all(&0u64.to_le_bytes())?; // e_shoff

    // e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx
    file.write_all(&0u32.to_le_bytes())?;
    file.write_all(&64u16.to_le_bytes())?; // e_ehsize
    file.write_all(&56u16.to_le_bytes())?; // e_phentsize
    file.write_all(&0u16.to_le_bytes())?; // e_phnum
    file.write_all(&0u16.to_le_bytes())?; // e_shentsize
    file.write_all(&0u16.to_le_bytes())?; // e_shnum
    file.write_all(&0u16.to_le_bytes())?; // e_shstrndx

    // program segment (just write text)
    file.write_all(text)?;
    file.write_all(data)?;

    Ok(())
}
