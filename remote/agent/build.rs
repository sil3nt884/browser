// Copyright 2026 The Umbra Authors.
// Builds the C transport (remote/net, shared with the Android app).

fn main() {
  println!("cargo:rerun-if-changed=../net/umbranet.c");
  println!("cargo:rerun-if-changed=../net/umbranet.h");
  cc::Build::new().file("../net/umbranet.c").include("../net").opt_level(2).warnings(true).compile("umbranet");
  for lib in ["ws2_32", "winmm", "qwave"] {
    println!("cargo:rustc-link-lib={lib}");
  }
}
