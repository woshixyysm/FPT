use std::mem;
use std::ptr::{self, NonNull};
use std::marker::PhantomData;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Ty {
    // 基础数值类型
    F64,
    I32,
    
    // 定长数组 [N]T
    Array(Box<Ty>, usize),

    // 单体静态引用 (Zig: *T)
    Ref(Box<Ty>),       // ref[T] (intent(in))
    MutRef(Box<Ty>),    // mutref[T] (intent(inout))

    // 切片静态引用 (Zig: []T, Fortran: Array Sections)
    Slice(Box<Ty>),     // slice[T]
    MutSlice(Box<Ty>),  // mutslice[T]
}

impl Ty {
    pub fn ref_of(inner: Ty) -> Ty { Ty::Ref(Box::new(inner)) }
    pub fn mutref_of(inner: Ty) -> Ty { Ty::MutRef(Box::new(inner)) }
    pub fn slice_of(inner: Ty) -> Ty { Ty::Slice(Box::new(inner)) }
    pub fn mutslice_of(inner: Ty) -> Ty { Ty::MutSlice(Box::new(inner)) }
}

#[derive(Debug, Copy, Clone, PartialEq, Eq, Hash)]
pub struct ZirId(pub u32);

#[derive(Debug)]
pub enum ZirExpr {
    // 取址: &x -> ref[T] 或 mutref[T]
    AddrOf { target: ZirId, is_mut: bool },

    // 读取: *p
    RefLoad { target: ZirId },

    // 写入: *p = v
    RefStore { target: ZirId, value: ZirId },

    // 索引: y[i] -> T (带边界检查语义)
    Index { target: ZirId, index: ZirId },

    // 切片投影: y[begin..end] -> slice[T]
    Slice {
        target: ZirId,
        begin: Option<ZirId>, // None 表示从头开始
        end: Option<ZirId>,   // None 表示到底
    },
}

pub struct Arena {
    blocks: Vec<Box<[u8]>>,
    current_block: *mut u8,
    current_offset: usize,
    block_size: usize,
}

impl Arena {
    pub fn new(block_size: usize) -> Self {
        let mut arena = Arena {
            blocks: Vec::new(),
            current_block: ptr::null_mut(),
            current_offset: 0,
            block_size: block_size.max(4096), // 默认 4KB 页级别
        };
        arena.grow();
        arena
    }

    fn grow(&mut self) {
        let mut block = vec![0u8; self.block_size].into_boxed_slice();
        self.current_block = block.as_mut_ptr();
        self.current_offset = 0;
        self.blocks.push(block);
    }

    /// 分配单个标量/结构体
    pub fn alloc<T>(&mut self, value: T) -> &mut T {
        let align = mem::align_of::<T>();
        let size = mem::size_of::<T>();

        // 计算满足对齐要求的 offset
        let aligned_offset = (self.current_offset + (align - 1)) & !(align - 1);

        if aligned_offset + size > self.block_size {
            self.grow();
            return self.alloc(value); // 重试分配到新 block
        }

        unsafe {
            let ptr = self.current_block.add(aligned_offset) as *mut T;
            ptr.write(value);
            self.current_offset = aligned_offset + size;
            &mut *ptr
        }
    }

    /// 批量分配连续内存并初始化（模拟 Fortran 数组）
    pub fn alloc_slice<T: Clone>(&mut self, len: usize, default: T) -> &mut [T] {
        if len == 0 {
            return &mut [];
        }
        
        let align = mem::align_of::<T>();
        let size = mem::size_of::<T>() * len;
        let aligned_offset = (self.current_offset + (align - 1)) & !(align - 1);

        if aligned_offset + size > self.block_size {
            self.grow();
            // 如果单个切片超过 block_size，需要大对象分配策略（这里简单扩容 block_size）
            if size > self.block_size {
                self.block_size = size.next_power_of_two();
            }
            return self.alloc_slice(len, default);
        }

        unsafe {
            let ptr = self.current_block.add(aligned_offset) as *mut T;
            for i in 0..len {
                ptr.add(i).write(default.clone());
            }
            self.current_offset = aligned_offset + size;
            std::slice::from_raw_parts_mut(ptr, len)
        }
    }
}

/// 单体只读引用: Zig `*const T`, Fortran `intent(in)`
#[derive(Debug)]
pub struct Ref<'a, T: 'a> {
    ptr: NonNull<T>, // 使用 NonNull 方便编译器做布局优化
    _marker: PhantomData<&'a T>,
}

impl<'a, T> Copy for Ref<'a, T> {}
impl<'a, T> Clone for Ref<'a, T> {
    fn clone(&self) -> Self { *self }
}

impl<'a, T> Ref<'a, T> {
    pub fn new(r: &'a T) -> Self {
        Ref {
            ptr: NonNull::from(r),
            _marker: PhantomData,
        }
    }
    #[inline(always)]
    pub fn get(&self) -> &'a T {
        unsafe { self.ptr.as_ref() }
    }
}

/// 单体可变引用: Zig `*T`, Fortran `intent(inout)`
#[derive(Debug)]
pub struct MutRef<'a, T: 'a> {
    ptr: NonNull<T>,
    _marker: PhantomData<&'a mut T>,
}

impl<'a, T> MutRef<'a, T> {
    pub fn new(r: &'a mut T) -> Self {
        MutRef {
            ptr: NonNull::from(r),
            _marker: PhantomData,
        }
    }
    #[inline(always)]
    pub fn get(&self) -> &'a mut T {
        unsafe { &mut *self.ptr.as_ptr() }
    }
    // 降级为只读引用的安全操作 (ZIR 隐式转换)
    pub fn as_ref(&self) -> Ref<'_, T> {
        Ref::new(self.get())
    }
}

/// 切片引用: Zig `[]const T`, 带边界信息
#[derive(Debug)]
pub struct SliceRef<'a, T: 'a> {
    ptr: NonNull<T>,
    pub len: usize,
    _marker: PhantomData<&'a [T]>,
}

impl<'a, T> Copy for SliceRef<'a, T> {}
impl<'a, T> Clone for SliceRef<'a, T> {
    fn clone(&self) -> Self { *self }
}

impl<'a, T> SliceRef<'a, T> {
    pub fn new(slice: &'a [T]) -> Self {
        // 允许空切片，但 NonNull 不能为 null，使用 dangling
        let ptr = if slice.is_empty() { NonNull::dangling() } else { NonNull::new(slice.as_ptr() as *mut T).unwrap() };
        SliceRef { ptr, len: slice.len(), _marker: PhantomData }
    }

    pub fn get_index(&self, index: usize) -> Option<Ref<'a, T>> {
        if index < self.len {
            unsafe { Some(Ref::new(&*self.ptr.as_ptr().add(index))) }
        } else {
            None
        }
    }

    pub fn sub_slice(&self, start: usize, end: usize) -> Option<SliceRef<'a, T>> {
        if start <= end && end <= self.len {
            unsafe {
                let new_ptr = self.ptr.as_ptr().add(start);
                Some(SliceRef::new(std::slice::from_raw_parts(new_ptr, end - start)))
            }
        } else {
            None
        }
    }
}

/// 可变切片引用: Zig `[]T`
#[derive(Debug)]
pub struct MutSliceRef<'a, T: 'a> {
    ptr: NonNull<T>,
    pub len: usize,
    _marker: PhantomData<&'a mut [T]>,
}

impl<'a, T> MutSliceRef<'a, T> {
    pub fn new(slice: &'a mut [T]) -> Self {
        let len = slice.len();
        let ptr = if slice.is_empty() { NonNull::dangling() } else { NonNull::new(slice.as_mut_ptr()).unwrap() };
        MutSliceRef { ptr, len, _marker: PhantomData }
    }

    pub fn get_index_mut(&mut self, index: usize) -> Option<MutRef<'_, T>> {
        if index < self.len {
            unsafe { Some(MutRef::new(&mut *self.ptr.as_ptr().add(index))) }
        } else {
            None
        }
    }
}
