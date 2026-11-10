// Reads hex-encoded cases, one per line; writes, per case, what serde_json makes of it:
// `ok <hex compact> <hex pretty>` or `err <hex message>`.
use std::io::{BufRead, Write};

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn unhex(text: &str) -> Vec<u8> {
    (0..text.len()).step_by(2).map(|i| u8::from_str_radix(&text[i..i + 2], 16).unwrap()).collect()
}

fn main() {
    let stdin = std::io::stdin();
    let mut out = std::io::stdout().lock();
    for line in stdin.lock().lines() {
        let line = line.unwrap();
        let bytes = unhex(line.trim());
        match serde_json::from_slice::<serde_json::Value>(&bytes) {
            Ok(value) => {
                let compact = serde_json::to_vec(&value).unwrap();
                let pretty = serde_json::to_vec_pretty(&value).unwrap();
                writeln!(out, "ok {} {}", hex(&compact), hex(&pretty)).unwrap();
            }
            Err(error) => writeln!(out, "err {}", hex(error.to_string().as_bytes())).unwrap(),
        }
    }
}
