// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! Pipe names.

const PREFIX: &str = "\\\\.\\pipe\\";

/// The NUL-terminated UTF-16 pipe path for `path`: a full `\\.\pipe\` path is
/// used as it is, a bare name is placed under it (with `\` turned into `/`).
pub(crate) fn make_pipe_path(path: &str) -> Option<Vec<u16>> {
    let full = if path.starts_with(PREFIX) {
        path.to_owned()
    } else {
        format!("{PREFIX}{}", path.replace('\\', "/"))
    };
    if full.len() == PREFIX.len() || full.len() > 256 {
        return None;
    }
    let mut wide: Vec<u16> = full.encode_utf16().collect();
    if wide.contains(&0) {
        return None;
    }
    wide.push(0);
    Some(wide)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn s(w: Option<Vec<u16>>) -> String {
        String::from_utf16(&w.unwrap()[..])
            .unwrap()
            .trim_end_matches('\0')
            .to_owned()
    }

    #[test]
    fn bare_names_go_under_the_pipe_namespace() {
        assert_eq!(s(make_pipe_path("a\\b")), "\\\\.\\pipe\\a/b");
        assert_eq!(s(make_pipe_path("\\\\.\\pipe\\x")), "\\\\.\\pipe\\x");
    }

    #[test]
    fn empty_and_long_names_fail() {
        assert!(make_pipe_path("").is_none());
        assert!(make_pipe_path("\\\\.\\pipe\\").is_none());
        assert!(make_pipe_path(&"a".repeat(300)).is_none());
    }
}
