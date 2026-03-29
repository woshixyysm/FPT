use std::fs::{File, OpenOptions};
use std::io::{self, Write};
use std::path::Path;
use memmap2::Mmap;

// 为了安全地在 byte 切片和 #[repr(C)] 结构体之间转换
// 之后需要在 Cargo.toml 引入: bytemuck = { version = "1", features = ["derive"] }
use bytemuck::{Pod, Zeroable};

pub type ZirNodeId = u32;
pub type ZirTypeId = u32;

#[repr(u8)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum OpCode {
    Nop = 0,
    Const = 1,
    Arg = 2,
    Add = 3,
    Call = 4,
    Let = 5,
    Jump = 6,
    Return = 7,
}

impl OpCode {
    pub fn from_u8(v: u8) -> OpCode {
        match v {
            1 => OpCode::Const,
            2 => OpCode::Arg,
            3 => OpCode::Add,
            4 => OpCode::Call,
            5 => OpCode::Let,
            6 => OpCode::Jump,
            7 => OpCode::Return,
            _ => OpCode::Nop,
        }
    }

    pub fn to_u8(self) -> u8 {
        self as u8
    }
}

#[repr(C, packed)]
#[derive(Debug, Copy, Clone, PartialEq, Pod, Zeroable)]
pub struct ZirNode {
    pub value: f64,
    pub input0: ZirNodeId,
    pub input1: ZirNodeId,
    pub input2: ZirNodeId,
    pub input3: ZirNodeId,
    pub aux: u32,
    pub type_id: ZirTypeId,
    pub opcode: u8,
}

impl ZirNode {
    pub fn opcode(&self) -> OpCode {
        OpCode::from_u8(self.opcode)
    }
}


#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq, Pod, Zeroable)]
pub struct StringId {
    pub offset: u32,
    pub len: u32,
}

#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq, Pod, Zeroable)]
pub struct DeclId(pub u32);

impl std::fmt::Display for DeclId {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.0)
    }
}

/// 文件头，用于校验和快速定位数据段
#[repr(C)]
#[derive(Debug, Copy, Clone, Pod, Zeroable)]
pub struct ZirHeader {
    pub magic: [u8; 4], // b"FZIR"
    pub version: u32,
    
    // 各个段的数量（不是字节大小，是元素个数）
    pub decl_count: u32,
    pub node_count: u32,
    pub token_id_count: u32, // token 索引数组的大小
    pub dep_id_count: u32, // dependencies 数组的大小
    pub string_pool_bytes: u32,
}

/// 对应原来的 ZirDecl，但完全没有堆分配
#[repr(C)]
#[derive(Debug, Copy, Clone, Pod, Zeroable)]
pub struct ZirDeclRecord {
    pub module_name: StringId,
    pub decl_name: StringId,
    pub kind: StringId, // 实际工程这里应该优化为 enum (u8)
    
    // 指向 Token_ID 数组的切片范围
    pub token_start_idx: u32,
    pub token_count: u32,
    
    // 源码范围 (如果在源文件中的位置)
    pub span_start: u32,
    pub span_end: u32,
    
    // 来源文件偏移
    pub source_file_offset: u32,
    
    // 节点区间（对应函数/表达式的 ZIR node range）
    pub node_start: u32,
    pub node_count: u32,

    // 指向其他 DeclRecord 的引用 (dependencies)
    pub dep_start_idx: u32,
    pub dep_count: u32,
}

/// 将 mmap 的只读内存包裹起来，提供安全访问的视图
pub struct ZirMmapView {
    _mmap: Mmap, // 保持句柄，防止被释放
    
    // 以下都是对 _mmap 内存的零拷贝切片引用
    header: ZirHeader,
    decls: Vec<ZirDeclRecord>,
    nodes: Vec<ZirNode>,
    token_ids: Vec<StringId>,
    deps: Vec<DeclId>,
    string_pool: Vec<u8>,
}

impl ZirMmapView {
    pub fn open<P: AsRef<Path>>(path: P) -> io::Result<Self> {
        let file = File::open(path)?;
        let mmap = unsafe { Mmap::map(&file)? };
        
        // 1. 验证大小并解析 Header
        if mmap.len() < std::mem::size_of::<ZirHeader>() {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "File too small"));
        }
        let (header_bytes, rest) = mmap.split_at(std::mem::size_of::<ZirHeader>());
        let header: ZirHeader = *bytemuck::from_bytes(header_bytes);
        
        if header.magic != *b"FZIR" {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "Invalid magic number"));
        }

        // 2. 计算 offsets
        let header_size = std::mem::size_of::<ZirHeader>();
        let decl_offset = header_size;
        let node_offset = decl_offset + (header.decl_count as usize) * std::mem::size_of::<ZirDeclRecord>();
        let token_offset = node_offset + (header.node_count as usize) * std::mem::size_of::<ZirNode>();
        let dep_offset = token_offset + (header.token_id_count as usize) * std::mem::size_of::<StringId>();
        let str_offset = dep_offset + (header.dep_id_count as usize) * std::mem::size_of::<DeclId>();

        let string_pool = mmap[str_offset..str_offset + header.string_pool_bytes as usize].to_vec();

        let decls = {
            let mut v = Vec::with_capacity(header.decl_count as usize);
            let size = std::mem::size_of::<ZirDeclRecord>();
            for i in 0..header.decl_count {
                let offset = decl_offset + i as usize * size;
                let bytes = &mmap[offset..offset + size];
                let decl = bytemuck::pod_read_unaligned(bytes);
                v.push(decl);
            }
            v
        };
        let nodes = {
            let mut v = Vec::with_capacity(header.node_count as usize);
            let size = std::mem::size_of::<ZirNode>();
            for i in 0..header.node_count {
                let offset = node_offset + i as usize * size;
                let bytes = &mmap[offset..offset + size];
                let node = bytemuck::pod_read_unaligned(bytes);
                v.push(node);
            }
            v
        };
        let token_ids = {
            let mut v = Vec::with_capacity(header.token_id_count as usize);
            let size = std::mem::size_of::<StringId>();
            for i in 0..header.token_id_count {
                let offset = token_offset + i as usize * size;
                let bytes = &mmap[offset..offset + size];
                let id = bytemuck::pod_read_unaligned(bytes);
                v.push(id);
            }
            v
        };
        let deps = {
            let mut v = Vec::with_capacity(header.dep_id_count as usize);
            let size = std::mem::size_of::<DeclId>();
            for i in 0..header.dep_id_count {
                let offset = dep_offset + i as usize * size;
                let bytes = &mmap[offset..offset + size];
                let id = bytemuck::pod_read_unaligned(bytes);
                v.push(id);
            }
            v
        };

        // 使用 transmute 延长生命周期到 'static 是因为数据被绑定在 _mmap 字段中
        // 只要 ZirMmapView 不被销毁，这些切片就是绝对安全的 (自引用结构的一种安全抽象)
        unsafe {
            Ok(Self {
                _mmap: mmap,
                header,
                decls,
                nodes,
                token_ids,
                deps,
                string_pool,
            })
        }
    }

    /// O(1) 获取字符串，零分配
    pub fn get_str(&self, id: StringId) -> &str {
        let start = id.offset as usize;
        let end = start + (id.len as usize);
        std::str::from_utf8(&self.string_pool[start..end]).unwrap_or("<invalid_utf8>")
    }

    /// 获取所有的声明记录
    pub fn decls(&self) -> &[ZirDeclRecord] {
        &self.decls
    }

    /// 获取某个声明的所有 Token 字符串视图
    pub fn get_tokens_for(&self, decl: &ZirDeclRecord) -> impl Iterator<Item = &str> {
        let start = decl.token_start_idx as usize;
        let end = start + (decl.token_count as usize);
        self.token_ids[start..end].iter().map(|&id| self.get_str(id))
    }

    /// 获取某个声明的依赖列表
    pub fn get_dependencies_for(&self, decl: &ZirDeclRecord) -> &[DeclId] {
        let start = decl.dep_start_idx as usize;
        let end = start + (decl.dep_count as usize);
        &self.deps[start..end]
    }

    /// 获取单个节点
    pub fn get_node(&self, node_id: ZirNodeId) -> Option<&ZirNode> {
        self.nodes.get(node_id as usize)
    }

    /// 根据 block 获取包含节点范围
    pub fn get_nodes_for_block(&self, block: &crate::control::ZirBlock) -> &[ZirNode] {
        let start = block.start_inst as usize;
        let end = (block.start_inst + block.inst_count) as usize;
        &self.nodes[start..end]
    }
}


pub struct ZirMmapBuilder {
    decls: Vec<ZirDeclRecord>,
    nodes: Vec<ZirNode>,
    token_ids: Vec<StringId>,
    deps: Vec<DeclId>,
    string_pool: Vec<u8>,
}

impl ZirMmapBuilder {
    pub fn new() -> Self {
        Self {
            decls: Vec::new(),
            nodes: Vec::new(),
            token_ids: Vec::new(),
            deps: Vec::new(),
            string_pool: Vec::new(),
        }
    }

    ///  intern 字符串，返回胖指针索引
    fn push_str(&mut self, s: &str) -> StringId {
        // 简单的去重可以在这里做 (HashMap<String, StringId>)，此处省略以保持极简
        let offset = self.string_pool.len() as u32;
        let len = s.len() as u32;
        self.string_pool.extend_from_slice(s.as_bytes());
        StringId { offset, len }
    }

    /// 预留下一个 node ID
    pub fn next_node_id(&self) -> ZirNodeId {
        self.nodes.len() as ZirNodeId
    }

    pub fn add_node_const(&mut self, value: f64) -> ZirNodeId {
        let id = self.next_node_id();
        self.nodes.push(ZirNode {
            value,
            input0: 0,
            input1: 0,
            input2: 0,
            input3: 0,
            opcode: OpCode::Const.to_u8(),
            aux: 0,
            type_id: 0,
        });
        id
    }

    pub fn add_node_arg(&mut self, arg_index: u32) -> ZirNodeId {
        let id = self.next_node_id();
        self.nodes.push(ZirNode {
            value: 0.0,
            input0: arg_index,
            input1: 0,
            input2: 0,
            input3: 0,
            opcode: OpCode::Arg.to_u8(),
            aux: 0,
            type_id: 0,
        });
        id
    }

    pub fn add_node_add(&mut self, lhs: ZirNodeId, rhs: ZirNodeId) -> ZirNodeId {
        let id = self.next_node_id();
        self.nodes.push(ZirNode {
            value: 0.0,
            input0: lhs,
            input1: rhs,
            input2: 0,
            input3: 0,
            opcode: OpCode::Add.to_u8(),
            aux: 0,
            type_id: 0,
        });
        id
    }

    pub fn add_node_call(&mut self, callee: &str, args: &[ZirNodeId]) -> ZirNodeId {
        let callee_id = self.push_str(callee);
        let id = self.next_node_id();
        let input0 = args.get(0).copied().unwrap_or(0);
        let input1 = args.get(1).copied().unwrap_or(0);
        let input2 = args.get(2).copied().unwrap_or(0);
        let input3 = args.get(3).copied().unwrap_or(0);
        let aux = (callee_id.offset as u32) | ((callee_id.len as u32) << 16);
        self.nodes.push(ZirNode {
            value: 0.0,
            input0,
            input1,
            input2,
            input3,
            opcode: OpCode::Call.to_u8(),
            aux,
            type_id: 0,
        });
        id
    }

    pub fn add_node_let(&mut self, name: &str, expr_node: ZirNodeId) -> ZirNodeId {
        let name_id = self.push_str(name).offset;
        let id = self.next_node_id();
        self.nodes.push(ZirNode {
            value: 0.0,
            input0: expr_node,
            input1: 0,
            input2: 0,
            input3: 0,
            opcode: OpCode::Let.to_u8(),
            aux: name_id,
            type_id: 0,
        });
        id
    }

    pub fn add_node_return(&mut self, value_node: ZirNodeId) -> ZirNodeId {
        let id = self.next_node_id();
        self.nodes.push(ZirNode {
            value: 0.0,
            input0: value_node,
            input1: 0,
            input2: 0,
            input3: 0,
            opcode: OpCode::Return.to_u8(),
            aux: 0,
            type_id: 0,
        });
        id
    }

    pub fn add_nodes_block(&mut self, nodes: &[ZirNode]) -> (ZirNodeId, u32) {
        let start = self.next_node_id();
        for n in nodes {
            self.nodes.push(*n);
        }
        let count = self.next_node_id() - start;
        (start, count)
    }

    pub fn push_node(&mut self, node: ZirNode) {
        self.nodes.push(node);
    }

    /// 写入一个模块的 ZIR
    pub fn add_decl(&mut self, module: &str, name: &str, kind: &str, tokens: &[String], span: (u32, u32), source_file_offset: u32, dependencies: &[DeclId], node_start: ZirNodeId, node_count: u32) {
        let mod_id = self.push_str(module);
        let name_id = self.push_str(name);
        let kind_id = self.push_str(kind);

        let token_start_idx = self.token_ids.len() as u32;
        let token_count = tokens.len() as u32;

        for t in tokens {
            let t_id = self.push_str(t);
            self.token_ids.push(t_id);
        }

        let dep_start_idx = self.deps.len() as u32;
        let dep_count = dependencies.len() as u32;

        for &dep in dependencies {
            self.deps.push(dep);
        }

        self.decls.push(ZirDeclRecord {
            module_name: mod_id,
            decl_name: name_id,
            kind: kind_id,
            token_start_idx,
            token_count,
            span_start: span.0,
            span_end: span.1,
            source_file_offset,
            node_start,
            node_count,
            dep_start_idx,
            dep_count,
        });
    }

    /// 获取下一个声明 ID
    pub fn next_decl_id(&self) -> DeclId {
        DeclId(self.decls.len() as u32)
    }

    /// 将拍平的数据图一次性 Flush 到磁盘
    pub fn write_to_file<P: AsRef<Path>>(&self, path: P) -> io::Result<()> {
        let mut file = OpenOptions::new().write(true).create(true).truncate(true).open(path)?;

        let header = ZirHeader {
            magic: *b"FZIR",
            version: 1,
            decl_count: self.decls.len() as u32,
            node_count: self.nodes.len() as u32,
            token_id_count: self.token_ids.len() as u32,
            dep_id_count: self.deps.len() as u32,
            string_pool_bytes: self.string_pool.len() as u32,
        };

        // 按顺序直接写入二进制数据
        file.write_all(bytemuck::bytes_of(&header))?;
        file.write_all(bytemuck::cast_slice(&self.decls))?;
        file.write_all(bytemuck::cast_slice(&self.nodes))?;
        file.write_all(bytemuck::cast_slice(&self.token_ids))?;
        file.write_all(bytemuck::cast_slice(&self.deps))?;
        file.write_all(&self.string_pool)?;

        Ok(())
    }
}