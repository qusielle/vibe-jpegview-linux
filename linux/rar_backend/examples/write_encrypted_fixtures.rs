use rars::{ArchiveVersion, Builder};
use std::fs;
use std::path::PathBuf;

const PASSWORD: &[u8] = b"test-secret";
const PNG_BASE64: &str =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=";

fn decode_base64(input: &str) -> Vec<u8> {
    let mut output = Vec::with_capacity(input.len() * 3 / 4);
    let mut accumulator = 0u32;
    let mut bits = 0u8;
    for byte in input.bytes() {
        if byte == b'=' {
            break;
        }
        let value = match byte {
            b'A'..=b'Z' => byte - b'A',
            b'a'..=b'z' => byte - b'a' + 26,
            b'0'..=b'9' => byte - b'0' + 52,
            b'+' => 62,
            b'/' => 63,
            _ => panic!("invalid base64 fixture source"),
        };
        accumulator = (accumulator << 6) | u32::from(value);
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            output.push((accumulator >> bits) as u8);
        }
    }
    output
}

fn write_fixture(path: PathBuf, version: ArchiveVersion, header_encryption: bool, png: &[u8]) {
    let mut builder = Builder::new(version)
        .store(true)
        .password(Some(PASSWORD.to_vec()))
        .header_encryption(header_encryption);
    builder
        .add_bytes(b"private.png".to_vec(), png.to_vec(), Some(0), None)
        .expect("queue fixture image");
    let bytes = builder.to_bytes().expect("write encrypted fixture archive");
    fs::write(path, bytes).expect("save encrypted fixture archive");
}

fn main() {
    let output = std::env::args_os()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("../tests/fixtures/encrypted_rar"));
    fs::create_dir_all(&output).expect("create fixture directory");
    let png = decode_base64(PNG_BASE64);
    fs::write(output.join("private.png"), &png).expect("save fixture image source");
    write_fixture(
        output.join("rar4-data-encrypted.rar"),
        ArchiveVersion::Rar30,
        false,
        &png,
    );
    write_fixture(
        output.join("rar4-header-encrypted.rar"),
        ArchiveVersion::Rar30,
        true,
        &png,
    );
    write_fixture(
        output.join("rar5-data-encrypted.rar"),
        ArchiveVersion::Rar50,
        false,
        &png,
    );
    write_fixture(
        output.join("rar5-header-encrypted.rar"),
        ArchiveVersion::Rar50,
        true,
        &png,
    );
}
