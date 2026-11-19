// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

use core::ffi::CStr;
use core::fmt;

/// Result of a DataPath call.
pub type Result<T> = core::result::Result<T, Error>;

/// Errors reported by DataPath.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum Error {
    /// The library returned a code this crate does not know.
    Unknown,
    /// Generic failure; details were not mappable to a specific code.
    Failure,
    CriticalFailure,
    /// The connection or server is closed.
    Closed,
    TimedOut,
    /// The path is malformed, too long for the platform, or contains a NUL byte.
    InvalidPath,
    NotSupported,
    /// The message exceeds the negotiated maximum message size.
    TooLarge,
    /// The send queue is full; retry later.
    WouldBlock,
    /// The peer speaks a different protocol version.
    VersionMismatch,
    /// Nothing is listening at the path.
    NotFound,
    /// Another server already hosts this path.
    AddressInUse,
    AccessDenied,
    /// An option was out of range.
    InvalidArgument,
    OutOfMemory,
}

impl Error {
    pub(crate) fn from_code(code: sys::datapath_error) -> Self {
        match code {
            sys::DATAPATH_ERROR_FAILURE => Error::Failure,
            sys::DATAPATH_ERROR_CRITICAL_FAILURE => Error::CriticalFailure,
            sys::DATAPATH_ERROR_CLOSED => Error::Closed,
            sys::DATAPATH_ERROR_TIMED_OUT => Error::TimedOut,
            sys::DATAPATH_ERROR_INVALID_PATH => Error::InvalidPath,
            sys::DATAPATH_ERROR_NOT_SUPPORTED => Error::NotSupported,
            sys::DATAPATH_ERROR_TOO_LARGE => Error::TooLarge,
            sys::DATAPATH_ERROR_WOULD_BLOCK => Error::WouldBlock,
            sys::DATAPATH_ERROR_VERSION_MISMATCH => Error::VersionMismatch,
            sys::DATAPATH_ERROR_NOT_FOUND => Error::NotFound,
            sys::DATAPATH_ERROR_ADDRESS_IN_USE => Error::AddressInUse,
            sys::DATAPATH_ERROR_ACCESS_DENIED => Error::AccessDenied,
            sys::DATAPATH_ERROR_INVALID_ARGUMENT => Error::InvalidArgument,
            sys::DATAPATH_ERROR_OUT_OF_MEMORY => Error::OutOfMemory,
            _ => Error::Unknown,
        }
    }

    /// The C API's numeric code for this error.
    pub fn code(self) -> i32 {
        match self {
            Error::Unknown => sys::DATAPATH_ERROR_UNKNOWN,
            Error::Failure => sys::DATAPATH_ERROR_FAILURE,
            Error::CriticalFailure => sys::DATAPATH_ERROR_CRITICAL_FAILURE,
            Error::Closed => sys::DATAPATH_ERROR_CLOSED,
            Error::TimedOut => sys::DATAPATH_ERROR_TIMED_OUT,
            Error::InvalidPath => sys::DATAPATH_ERROR_INVALID_PATH,
            Error::NotSupported => sys::DATAPATH_ERROR_NOT_SUPPORTED,
            Error::TooLarge => sys::DATAPATH_ERROR_TOO_LARGE,
            Error::WouldBlock => sys::DATAPATH_ERROR_WOULD_BLOCK,
            Error::VersionMismatch => sys::DATAPATH_ERROR_VERSION_MISMATCH,
            Error::NotFound => sys::DATAPATH_ERROR_NOT_FOUND,
            Error::AddressInUse => sys::DATAPATH_ERROR_ADDRESS_IN_USE,
            Error::AccessDenied => sys::DATAPATH_ERROR_ACCESS_DENIED,
            Error::InvalidArgument => sys::DATAPATH_ERROR_INVALID_ARGUMENT,
            Error::OutOfMemory => sys::DATAPATH_ERROR_OUT_OF_MEMORY,
        }
    }

    /// Name of the error as the library spells it, e.g. `"WouldBlock"`.
    pub fn as_str(self) -> &'static str {
        // SAFETY: the library returns a static, NUL-terminated string, never NULL.
        unsafe { CStr::from_ptr(sys::datapath_error_string(self.code())) }
            .to_str()
            .unwrap_or("Unknown")
    }
}

/// Turn a C return code into a `Result`.
pub(crate) fn check(code: sys::datapath_error) -> Result<()> {
    if code == sys::DATAPATH_SUCCESS {
        Ok(())
    } else {
        Err(Error::from_code(code))
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

impl std::error::Error for Error {}

impl From<Error> for std::io::Error {
    fn from(e: Error) -> Self {
        use std::io::ErrorKind as K;
        let kind = match e {
            Error::Closed => K::BrokenPipe,
            Error::TimedOut => K::TimedOut,
            Error::InvalidPath | Error::InvalidArgument | Error::TooLarge => K::InvalidInput,
            Error::NotSupported => K::Unsupported,
            Error::WouldBlock => K::WouldBlock,
            Error::NotFound => K::NotFound,
            Error::AddressInUse => K::AddrInUse,
            Error::AccessDenied => K::PermissionDenied,
            Error::OutOfMemory => K::OutOfMemory,
            _ => K::Other,
        };
        std::io::Error::new(kind, e)
    }
}
