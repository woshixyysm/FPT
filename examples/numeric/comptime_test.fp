pub module comptime_test

import array

// Test comptime ZIR access and node generation
pub comptime fn generate_adder(n: i32) -> fn(i32) -> i32 =
    // This would generate a new ZIR node for a function that adds n
    // For now, just return a lambda
    fn(x: i32) -> i32 = x + n

pub fn test_comptime() -> i32 =
    let add5 = generate_adder(5)
    add5(10)  // Should return 15