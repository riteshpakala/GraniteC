// Reads hex-encoded lines, one per line; writes, per line, what GraniteRust reads it as:
// `ok <hex line it writes back>` or `err <hex sentence>`.
use std::io::{BufRead, Write};

use granite_agents::AgentWire;

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
        let bytes = unhex(line.unwrap().trim());
        match AgentWire::read(&bytes) {
            Ok(message) => writeln!(out, "ok {}", hex(&message.line().unwrap())).unwrap(),
            Err(error) => writeln!(out, "err {}", hex(error.0.as_bytes())).unwrap(),
        }
    }
}
