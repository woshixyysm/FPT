pub module array

// add_vec: element-wise vector addition (shape checked by compiler)
pub fn add_vec[N: usize](a: [f64; N], b: [f64; N]) -> [f64; N] =
    a + b

pub fn dot[N: usize](a: [f64; N], b: [f64; N]) -> f64 =
    sum = 0.0
    for i in 0..N do
        sum = sum + a[i] * b[i]
    sum