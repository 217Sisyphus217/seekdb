// Copyright (c) 2026 OceanBase.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

pub(crate) fn space(byte: u8) -> bool {
    matches!(byte, b' ' | b'\t' | b'\n' | b'\r' | 0x0b | 0x0c)
}

pub(crate) fn identifier(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'$' | 0x80..=0xff)
}

pub(crate) fn quoted_end(input: &[u8], start: usize, backslash_escapes: bool) -> Option<usize> {
    let quote = input[start];
    let mut pos = start + 1;
    while let Some(&byte) = input.get(pos) {
        pos += 1;
        if byte == quote {
            if input.get(pos) == Some(&quote) {
                pos += 1;
            } else {
                return Some(pos);
            }
        } else if byte == b'\\' && backslash_escapes {
            pos += 1;
        }
    }
    None
}

pub(crate) fn decode_string(text: &str, no_backslash_escapes: bool) -> String {
    let mut result = Vec::with_capacity(text.len() - 2);
    decode_string_into(text, no_backslash_escapes, &mut result);
    String::from_utf8(result).unwrap()
}

pub(crate) fn decode_string_into(text: &str, no_backslash_escapes: bool, result: &mut Vec<u8>) {
    let quote = text.as_bytes()[0];
    let bytes = &text.as_bytes()[1..text.len() - 1];
    let mut pos = 0;
    while pos < bytes.len() {
        let byte = bytes[pos];
        pos += 1;
        if byte == quote {
            pos += 1;
            result.push(byte);
        } else if byte == b'\\' && !no_backslash_escapes {
            let escaped = bytes[pos];
            pos += 1;
            let decoded = match escaped {
                b'0' => 0,
                b'b' => 8,
                b'n' => b'\n',
                b'r' => b'\r',
                b't' => b'\t',
                b'Z' => 26,
                b'%' | b'_' => {
                    result.push(b'\\');
                    escaped
                }
                byte => byte,
            };
            result.push(decoded);
        } else {
            result.push(byte);
        }
    }
}
