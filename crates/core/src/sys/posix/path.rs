// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! Server paths to socket addresses.

use rustix::net::SocketAddrUnix;

/// A resolved socket address.
pub(crate) struct SocketAddress {
    pub addr: SocketAddrUnix,
    /// The socket file, for filesystem sockets.
    pub filesystem_path: Option<String>,
}

impl SocketAddress {
    pub fn is_abstract(&self) -> bool {
        self.filesystem_path.is_none()
    }
}

/// An absolute path is used as it is. A bare name lives in the abstract
/// namespace on Linux and Android, and in `/tmp/datapath.<name>` elsewhere
/// (which every user shares, like the abstract namespace; the per-user
/// `$TMPDIR` would hide a server from other users' clients).
pub(crate) fn make_socket_address(path: &str) -> Option<SocketAddress> {
    if path.is_empty() || path.contains('\0') {
        return None;
    }
    if path.starts_with('/') {
        let addr = SocketAddrUnix::new(path).ok()?;
        return Some(SocketAddress {
            addr,
            filesystem_path: Some(path.to_owned()),
        });
    }
    #[cfg(any(target_os = "linux", target_os = "android"))]
    {
        let name = format!("datapath.{path}");
        let addr = SocketAddrUnix::new_abstract_name(name.as_bytes()).ok()?;
        Some(SocketAddress {
            addr,
            filesystem_path: None,
        })
    }
    #[cfg(not(any(target_os = "linux", target_os = "android")))]
    {
        make_socket_address(&format!("/tmp/datapath.{path}"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn absolute_paths_are_filesystem_sockets() {
        let a = make_socket_address("/tmp/x.sock").unwrap();
        assert_eq!(a.filesystem_path.as_deref(), Some("/tmp/x.sock"));
        assert!(!a.is_abstract());
    }

    #[test]
    fn rejects_empty_and_too_long_paths() {
        assert!(make_socket_address("").is_none());
        assert!(make_socket_address(&format!("/{}", "a".repeat(200))).is_none());
        assert!(make_socket_address("bad\0name").is_none());
    }

    #[cfg(any(target_os = "linux", target_os = "android"))]
    #[test]
    fn bare_names_are_abstract_on_linux() {
        assert!(make_socket_address("my-service").unwrap().is_abstract());
    }
}
