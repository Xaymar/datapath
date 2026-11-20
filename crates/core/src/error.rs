// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! Error codes. The numeric values are the ones of `datapath_error` in the C API
//! (`datapath.h`) and of `datapath::error` in the C++ API, so they can be passed
//! across the C ABI unchanged.

use std::fmt;

/// Why an operation failed. `Success` (0) is not an error and has no variant.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(i32)]
pub enum Error {
    /// Generic failure; details were not mappable to a specific code.
    Failure = 1,
    /// A failure the library cannot recover from.
    CriticalFailure = 2,
    /// The connection or server is closed.
    Closed = 3,
    /// The operation did not finish in time.
    TimedOut = 4,
    /// The path is malformed or too long for the platform.
    InvalidPath = 5,
    /// The platform does not support this.
    NotSupported = 6,
    /// The message exceeds the negotiated maximum message size.
    TooLarge = 7,
    /// The send queue is full; retry later.
    WouldBlock = 8,
    /// The peer speaks a different protocol version.
    VersionMismatch = 9,
    /// Nothing is listening at the path.
    NotFound = 10,
    /// Another server already hosts this path.
    AddressInUse = 11,
    /// The operating system denied access.
    AccessDenied = 12,
    /// A required value was missing or out of range.
    InvalidArgument = 13,
    /// Memory allocation failed.
    OutOfMemory = 14,
}

impl Error {
    /// The C API code of this error.
    pub fn code(self) -> i32 {
        self as i32
    }

    /// The error for a C API code; `None` for 0 (success), -1 (unknown) and codes
    /// this version does not know.
    pub fn from_code(code: i32) -> Option<Error> {
        Some(match code {
            1 => Error::Failure,
            2 => Error::CriticalFailure,
            3 => Error::Closed,
            4 => Error::TimedOut,
            5 => Error::InvalidPath,
            6 => Error::NotSupported,
            7 => Error::TooLarge,
            8 => Error::WouldBlock,
            9 => Error::VersionMismatch,
            10 => Error::NotFound,
            11 => Error::AddressInUse,
            12 => Error::AccessDenied,
            13 => Error::InvalidArgument,
            14 => Error::OutOfMemory,
            _ => return None,
        })
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Error::Failure => "operation failed",
            Error::CriticalFailure => "critical failure",
            Error::Closed => "connection closed",
            Error::TimedOut => "timed out",
            Error::InvalidPath => "invalid path",
            Error::NotSupported => "not supported",
            Error::TooLarge => "message too large",
            Error::WouldBlock => "send queue full",
            Error::VersionMismatch => "protocol version mismatch",
            Error::NotFound => "nothing is listening at the path",
            Error::AddressInUse => "address already in use",
            Error::AccessDenied => "access denied",
            Error::InvalidArgument => "invalid argument",
            Error::OutOfMemory => "out of memory",
        })
    }
}

impl std::error::Error for Error {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn codes_round_trip() {
        for code in 1..=14 {
            let e = Error::from_code(code).expect("known code");
            assert_eq!(e.code(), code);
        }
        assert_eq!(Error::from_code(0), None);
        assert_eq!(Error::from_code(15), None);
        assert_eq!(Error::from_code(-1), None);
    }

    #[test]
    fn codes_match_the_c_api() {
        assert_eq!(Error::Closed.code(), 3);
        assert_eq!(Error::WouldBlock.code(), 8);
        assert_eq!(Error::NotFound.code(), 10);
    }
}
