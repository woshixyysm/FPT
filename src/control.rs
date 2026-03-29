// FPT 控制流系统：严格区分“有界计算(repeat)”与“无界状态轮询(loop)”
// 规范：基于扁平化 Mmap 架构，无嵌套指针，使用 Block 切片索引。

use crate::zir::ZirNodeId as ZirId;

/// ZIR 的基本块 (Basic Block)。
/// 在 mmap 内存图中，它不包含任何指针，只是指向指令数组的一个切片视图。
#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct ZirBlock {
    pub start_inst: u32, // 块内第一条指令的索引
    pub inst_count: u32, // 块内指令的总数
}

/// 在 ZIR 层面将控制流拍平，不使用树形 AST 嵌套，
/// 而是使用结构化的跳转指令，确保内存连续和 Cache 友好。
#[derive(Debug, Clone)]
pub enum ZirControlFlow {
    /// repeat (n) { ... }
    /// 特性：n 在进入前只求值一次。绝对安全，不会死循环（除非 n 溢出，但在 FPT 中 n 通常是 usize）。
    Repeat {
        count_val: ZirId,          // 循环次数的 SSA ID (仅在进入循环前读取一次)
        induction_var: ZirId,      // 隐藏的/显式的迭代器变量 i (0..n)
        body: ZirBlock,            // 循环体代码块
    },

    /// loop (cond) { ... }
    /// 特性：每次迭代都必须重新执行 cond_block，重新评估 cond_val。
    Loop {
        cond_block: ZirBlock,      // 条件求值块 (每次循环前执行)
        cond_val: ZirId,           // 条件块产出的布尔值 SSA ID
        body: ZirBlock,            // 循环体代码块
    },
    
    // （好好想一下，或者还可以加入类似 Watchdog 的硬中断支持）
}

/// 负责将前端的 repeat/loop 语法 lowering（降级）到扁平的 ZIR
pub struct ControlFlowBuilder {
    // 当前正在构建的指令流（会被打包进 Mmap）
    current_inst_offset: u32,
}

impl ControlFlowBuilder {
    pub fn new(start_offset: u32) -> Self {
        Self {
            current_inst_offset: start_offset,
        }
    }

    /// 构建一个 `repeat (n)` 节点
    pub fn build_repeat(
        &mut self, 
        count: ZirId, 
        build_body: impl FnOnce(&mut Self) -> ZirBlock
    ) -> ZirControlFlow {
        // 分配一个新的 SSA 寄存器作为内部索引 i
        let induction_var = self.allocate_ssa(); 
        
        // 闭包执行，将 body 的指令追加到全局指令流，并返回块的范围
        let body_block = build_body(self);

        ZirControlFlow::Repeat {
            count_val: count,
            induction_var,
            body: body_block,
        }
    }

    /// 构建一个 `loop (cond)` 节点
    pub fn build_loop(
        &mut self,
        build_cond: impl FnOnce(&mut Self) -> (ZirBlock, ZirId),
        build_body: impl FnOnce(&mut Self) -> ZirBlock
    ) -> ZirControlFlow {
        // 先生成条件块的指令
        let (cond_block, cond_val) = build_cond(self);
        
        // 再生成循环体的指令
        let body_block = build_body(self);

        ZirControlFlow::Loop {
            cond_block,
            cond_val,
            body: body_block,
        }
    }

    // --- 内部辅助方法 ---
    fn allocate_ssa(&mut self) -> ZirId {
        // 实际工程中，这里会向符号表或当前函数分配一个新的虚拟寄存器
        999
    }
}
