# Encrypted RAR regression fixtures

The four archives below were generated locally by the fixture writer at
`linux/rar_backend/examples/write_encrypted_fixtures.rs`, using the exact
Apache-2.0 `bitplane/rars` source revision `afc60e4c669ba1fe6a18748b16b08164e69c5e44`.
Each contains the same 1×1 PNG as `private.png`. The test-only password is
`test-secret`; no production credential or user data is present.

Regenerate them from the repository root with the pinned toolchain:

```sh
RUSTUP_HOME=/tmp/jpegview-rustup CARGO_HOME=/tmp/jpegview-cargo \
  CARGO_TARGET_DIR=/tmp/jpegview-rar-target \
  /tmp/jpegview-cargo/bin/cargo run --manifest-path linux/rar_backend/Cargo.toml \
  --release --locked --example write_encrypted_fixtures -- linux/tests/fixtures/encrypted_rar
```

| Fixture | Format | Encryption | SHA-256 |
|---|---|---|---|
| `rar4-data-encrypted.rar` | RAR 3.0 / RAR4 family | member data | `fd6539d79bcb14fa8a68c63f925a8d660a497d0c836a807b87fe0a8847b9343d` |
| `rar4-header-encrypted.rar` | RAR 3.0 / RAR4 family | headers and member data | `c4dc89b912e7f57e0ba8c4fae4a785f310dff6db0339af47a0a48a56cdd6c53e` |
| `rar5-data-encrypted.rar` | RAR5 | member data | `c246ad28eea6309dac0b7975a94abd633c8e612286ef69ea629bd33d6e91b937` |
| `rar5-header-encrypted.rar` | RAR5 | headers and member data | `1162239f9d62374b8060d155abb5931e95332a75a662ca0f8838a8ec1206cfbd` |
