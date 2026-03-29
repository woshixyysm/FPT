use crate::zir::{ZirMmapView, ZirNodeId, OpCode};
use std::collections::HashMap;

#[derive(Debug)]
pub enum ExecutorResult {
    Continue,
    Return(f64),
    Halt,
}

pub struct Executor<'a> {
    pub view: &'a ZirMmapView,
    pub pc: ZirNodeId,
    pub values: HashMap<ZirNodeId, f64>,
    pub args: HashMap<u32, f64>,
}

impl<'a> Executor<'a> {
    pub fn new(view: &'a ZirMmapView, start_pc: ZirNodeId, args: HashMap<u32, f64>) -> Self {
        Self { view, pc: start_pc, values: HashMap::new(), args }
    }

    fn find_decl<'b>(view: &'b ZirMmapView, name: &str) -> Option<&'b crate::zir::ZirDeclRecord> {
        view.decls().iter().find(|d| view.get_str(d.decl_name) == name)
    }

    pub fn run(&mut self) -> Result<f64, String> {
        loop {
            match self.step()? {
                ExecutorResult::Return(v) => return Ok(v),
                ExecutorResult::Continue => continue,
                ExecutorResult::Halt => return Err("unexpected halt".to_string()),
            }
        }
    }

    fn eval_node(&mut self, node_id: ZirNodeId) -> Result<f64, String> {
        if let Some(v) = self.values.get(&node_id) {
            return Ok(*v);
        }

        let node = self.view.get_node(node_id).ok_or_else(|| "node not found".to_string())?;

        let value = match node.opcode() {
            OpCode::Const => node.value,
            OpCode::Arg => {
                let arg_index = node.input0;
                *self.args.get(&arg_index).unwrap_or(&0.0)
            }
            OpCode::Add => {
                let lhs = self.eval_node(node.input0)?;
                let rhs = self.eval_node(node.input1)?;
                lhs + rhs
            }
            OpCode::Let => {
                let rhs = self.eval_node(node.input0)?;
                rhs
            }
            OpCode::Call => {
                let offset = node.aux & 0xFFFF;
                let len = (node.aux >> 16) & 0xFFFF;
                let callee_name = self.view.get_str(crate::zir::StringId { offset, len });
                let callee_decl = Self::find_decl(self.view, &callee_name).ok_or_else(|| format!("callee '{}' not found", callee_name))?;
                
                let mut arg_nodes = Vec::new();
                if node.input0 != 0 { arg_nodes.push(node.input0); }
                if node.input1 != 0 { arg_nodes.push(node.input1); }
                if node.input2 != 0 { arg_nodes.push(node.input2); }
                if node.input3 != 0 { arg_nodes.push(node.input3); }
                
                let mut new_args = HashMap::new();
                for (i, &arg_node) in arg_nodes.iter().enumerate() {
                    new_args.insert(i as u32, self.eval_node(arg_node)?);
                }
                
                let mut sub_executor = Executor::new(self.view, callee_decl.node_start, new_args);
                sub_executor.run()?
            }
            OpCode::Jump => 0.0,
            OpCode::Return => self.eval_node(node.input0)?,
            OpCode::Nop => 0.0,
        };

        self.values.insert(node_id, value);
        Ok(value)
    }

    pub fn step(&mut self) -> Result<ExecutorResult, String> {
        let node = self.view.get_node(self.pc).ok_or_else(|| "PC out of bounds".to_string())?;

        match node.opcode() {
            OpCode::Nop => {
                self.pc += 1;
                Ok(ExecutorResult::Continue)
            }
            OpCode::Const | OpCode::Arg | OpCode::Add | OpCode::Let | OpCode::Call => {
                // All value-producing nodes are evaluated lazily via eval_node; don't return directly
                let _ = self.eval_node(self.pc)?;
                self.pc += 1;
                Ok(ExecutorResult::Continue)
            }
            OpCode::Jump => {
                self.pc = node.input0;
                Ok(ExecutorResult::Continue)
            }
            OpCode::Return => {
                let v = self.eval_node(node.input0)?;
                Ok(ExecutorResult::Return(v))
            }
        }
    }
}
