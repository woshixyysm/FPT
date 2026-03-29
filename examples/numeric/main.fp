pub module main

import array
import autodiff
import comptime_test

pub fn quadratic(a: f64, b: f64, c: f64, x: f64) -> f64 = a*x**2 + b*x + c

pub fn main() ->
    let a: [f64; 4] = [1.0, 2.0, 3.0, 4.0]
    let b: [f64; 4] = [4.0, 3.0, 2.0, 1.0]
    let c = array.add_vec(a, b)
    // Only when we call gradient will the compiler trigger AD lowering for the provided lambda
    let f = fn(x: f64) -> f64 = x * x + 3.0 * x + 2.0
    let g = autodiff.gradient(f, 2.0)
    // Test comptime functionality
    let result = comptime_test.test_comptime()
    result