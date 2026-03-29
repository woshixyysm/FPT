use crate::fpt_static_ptr::ZirId;
use crate::zir_mmap::StringId;

/// 效应输出维度：决定了数据流向观测终端的哪个显示通道
#[repr(u8)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum CoutDimension {
    Value = 0, // 运行时原始值 (Fortran 风格的高速数据通道)
    Expr  = 1, // 源码表达式投影 (Zig 风格的编译期元数据)
    Type  = 2, // 强类型系统投影
    Ctx   = 3, // 上下文/调用栈/位置元数据 (PLC 诊断通道)
}

/// ZIR 层的效应节点记录
/// 它是扁平化的，可以直接存入 Mmap 对象图中
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct ZirCoutEffect {
    pub slot_id: u32,           // cout[x] 的索引位
    pub dim: CoutDimension,     // 维度转换
    pub target_val: ZirId,      // 监视的目标 SSA 变量
    pub metadata_str: StringId, // 静态字符串池索引 (指向 Expr, Type 或 Ctx 的文本)
}


pub struct Cout;

impl Cout {
    /// 模拟 API: cout[x]
    #[inline(always)]
    pub fn index(&self, id: u32) -> CoutSlotProxy {
        CoutSlotProxy { slot_id: id }
    }
}

/// 效应代理：负责生成具有“强指针”特性的 ZIR 节点
pub struct CoutSlotProxy {
    slot_id: u32,
}

impl CoutSlotProxy {
    /// 导出运行时数据值
    pub fn value(self, target: ZirId) -> ZirCoutEffect {
        ZirCoutEffect {
            slot_id: self.slot_id,
            dim: CoutDimension::Value,
            target_val: target,
            metadata_str: StringId { offset: 0, len: 0 },
        }
    }

    /// 导出源码表达式 (如 "matrix_a * matrix_b")
    pub fn expr(self, target: ZirId, expr_str: StringId) -> ZirCoutEffect {
        ZirCoutEffect {
            slot_id: self.slot_id,
            dim: CoutDimension::Expr,
            target_val: target,
            metadata_str: expr_str,
        }
    }

    /// 导出全路径类型信息
    pub fn ty(self, target: ZirId, type_str: StringId) -> ZirCoutEffect {
        ZirCoutEffect {
            slot_id: self.slot_id,
            dim: CoutDimension::Type,
            target_val: target,
            metadata_str: type_str,
        }
    }

    /// 导出调用栈/位置元数据
    /// 在 FPT 规范下，ctx 代表了副作用发生的“地理位置”
    pub fn ctx(self, target: ZirId, ctx_str: StringId) -> ZirCoutEffect {
        ZirCoutEffect {
            slot_id: self.slot_id,
            dim: CoutDimension::Ctx,
            target_val: target,
            metadata_str: ctx_str,
        }
    }
}

#[repr(C)]
pub struct CoutRuntimeTable {
    pub total_slots: u32,
    // 后面紧跟连续的 CoutSnapshot 结构体数组
    // [CoutSnapshot; N]
}
