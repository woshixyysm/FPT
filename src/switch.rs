// FPT Switch 路由控制：专为 Mmap 零拷贝与 O(1) 跳转表优化
// 规范：拍平 Vec 与 Option，彻底消除堆分配。

use crate::control::ZirBlock;

/// ZIR 中的单个 Switch Case 分支。
/// #[repr(C)] 保证它在内存中是紧凑排列的，可以直接从磁盘零拷贝读取。
#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct ZirSwitchCase {
    pub match_val: i64,  // 匹配的常量值 (支持整数、枚举的底层 Tag 等)
    pub block: ZirBlock, // 对应的执行块
}

#[derive(Debug, Clone)]
pub enum ZirControlFlow {
    // ... Repeat 和 Loop ...

    /// 拍平后的 Switch 节点
    Switch {
        selector: ZirId,          // 用于判断的 SSA 变量 ID
        cases_start_idx: u32,     // 指向全局 ZirSwitchCase 数组的起始索引
        cases_count: u32,         // 分支的数量
        default_block: ZirBlock,  // 默认分支。如果 inst_count == 0，表示没有 default
    },
}

pub struct ControlFlowBuilder {
    current_inst_offset: u32,
    
    // 全局的 cases 缓冲池，会被一并写入 Mmap 数据段
    global_cases_pool: Vec<ZirSwitchCase>, 
}

impl ControlFlowBuilder {
    pub fn new(start_offset: u32) -> Self {
        Self {
            current_inst_offset: start_offset,
            global_cases_pool: Vec::new(),
        }
    }

    /// 接收前端传来的结构化数据，将其“拍平”为底层的连续内存结构
    pub fn build_switch(
        &mut self,
        selector: ZirId,
        // 前端传过来的确实可以是 Vec，但在这里会被打散并存入全局池
        frontend_cases: Vec<(i64, ZirBlock)>,
        frontend_default: Option<ZirBlock>,
    ) -> ZirControlFlow {
        
        let cases_start_idx = self.global_cases_pool.len() as u32;
        let cases_count = frontend_cases.len() as u32;

        // 拍平 cases 并追加到全局池
        for (val, block) in frontend_cases {
            self.global_cases_pool.push(ZirSwitchCase {
                match_val: val,
                block,
            });
        }

        // 处理 Option 默认块 (Zig 哲学：显式化)
        // 如果没有 default，我们构造一个 inst_count = 0 的空块。
        // Sema 阶段将负责验证，如果没有 default 块，前面的 cases 必须覆盖所有可能的枚举值。
        let default_block = frontend_default.unwrap_or(ZirBlock {
            start_inst: 0,
            inst_count: 0, 
        });

        ZirControlFlow::Switch {
            selector,
            cases_start_idx,
            cases_count,
            default_block,
        }
    }
}
