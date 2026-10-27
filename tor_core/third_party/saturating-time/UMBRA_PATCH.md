# Local patch of saturating-time 0.5.0

Upstream: https://gitlab.torproject.org/tpo/core/arti (crates/saturating-time),
vendored from crates.io 0.5.0 with one change in `src/internal.rs`
(marked `[umbra]`).

**Bug:** `find_limit()` halves its step until `checked_sub`/`checked_add`
returns `None` at a 1ns step. On Windows `SystemTime` has 100ns resolution, so
a 1ns step returns `Some(unchanged)` forever and the loop never ends. Arti hits
this on the first consensus it parses (`Preamble::validity_time_range` ->
`SaturatingTime::min_value()`), so bootstrap hangs at 15% with one Tokio
worker spinning. Linux has 1ns resolution, which is why tests there pass.

**Fix:** treat a step that does not move the value like `None` (requires
`PartialEq`, which `SystemTime` and `Instant` implement).

On Windows two of the crate's own tests still fail, as they would upstream
once the loop terminates: they assume 1ns clock resolution
(`max + 1ns == None`) and a SystemTime range wider than `Duration::MAX`.
Both hold on Linux, not on Windows; the limits found are correct
(`max` = `i64::MAX` intervals).

Remove the `[patch.crates-io]` entry in `tor_core/Cargo.toml` and this
directory once upstream ships a fixed release.

## License

`saturating-time` 0.5.0 is part of Arti by the Tor Project
(<https://gitlab.torproject.org/tpo/core/arti>), licensed MIT OR Apache-2.0
(see `Cargo.toml`). This copy is the crates.io release plus the one change
above, under the same terms.
