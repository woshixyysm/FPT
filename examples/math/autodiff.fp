pub module autodiff

// lazy AD API surface — the Sema/Autodiff passes will only run when generate_ad_for is invoked.
pub fn gradient(f: fn(f64) -> f64, x: f64) -> f64 =
    autodiff.reverse(f, x)