use std::collections::HashMap;

pub struct RttiType {
    pub name: String,
    pub flags: u32,
    pub size: u32,
    pub field_table_rva: u32,
    pub name_offset: usize,
    pub desc_offset: usize,
}

pub struct RttiBuilder {
    types: Vec<RttiType>,
    name_index: HashMap<String, usize>,
}

impl RttiBuilder {
    pub fn new() -> Self {
        Self { types: Vec::new(), name_index: HashMap::new() }
    }

    pub fn register_primitive(&mut self, name: &str, size: u32) -> usize {
        if let Some(&idx) = self.name_index.get(name) {
            return idx;
        }
        let idx = self.types.len();
        self.types.push(RttiType {
            name: name.to_string(),
            flags: 0,
            size,
            field_table_rva: 0,
            name_offset: 0,
            desc_offset: 0,
        });
        self.name_index.insert(name.to_string(), idx);
        idx
    }

    pub fn emit_rdata(&mut self) -> Vec<u8> {
        let mut out = Vec::new();

        // Emit type names first, recording offsets
        for ty in &mut self.types {
            ty.name_offset = out.len();
            out.extend_from_slice(ty.name.as_bytes());
            out.push(0);
        }

        // Align descriptor area to 8 bytes
        while out.len() % 8 != 0 {
            out.push(0);
        }

        // Record descriptor offset and write placeholder bytes
        for ty in &mut self.types {
            ty.desc_offset = out.len();
            out.extend_from_slice(&[0u8; 16]);
        }

        out
    }

    pub fn write_descriptors(&self, out: &mut [u8], image_base: u64, data_section_rva: u64) {
        for ty in &self.types {
            let name_rva = (image_base + data_section_rva + ty.name_offset as u64) as u32;
            let top = ty.desc_offset;
            out[top..top+4].copy_from_slice(&name_rva.to_le_bytes());
            out[top+4..top+8].copy_from_slice(&ty.flags.to_le_bytes());
            out[top+8..top+12].copy_from_slice(&ty.size.to_le_bytes());
            out[top+12..top+16].copy_from_slice(&ty.field_table_rva.to_le_bytes());
        }
    }

    pub fn type_rva(&self, name: &str, image_base: u64, data_section_rva: u64) -> Option<u64> {
        self.name_index.get(name).map(|&idx| {
            let ty = &self.types[idx];
            image_base + data_section_rva + ty.desc_offset as u64
        })
    }

    pub fn types(&self) -> &[RttiType] { &self.types }
}
