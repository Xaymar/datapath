// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! Access control of a hosted pipe.

use crate::options::Permissions;
use std::ffi::c_void;
use windows_sys::Win32::Foundation::{CloseHandle, LocalFree};
use windows_sys::Win32::Security::Authorization::{
    ConvertSidToStringSidW, ConvertStringSecurityDescriptorToSecurityDescriptorW, SDDL_REVISION_1,
};
use windows_sys::Win32::Security::{GetTokenInformation, TokenUser, TOKEN_QUERY, TOKEN_USER};
use windows_sys::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};

/// The SDDL string for a permission level. The owner and SYSTEM get full
/// access; others get read and write (enough to connect, not enough to create
/// competing pipe instances).
pub(crate) fn make_sddl(level: Permissions, user_sid: &str) -> String {
    let mut sddl = format!("D:P(A;;GA;;;{user_sid})(A;;GA;;;SY)");
    match level {
        Permissions::User => {}
        Permissions::Group => sddl.push_str("(A;;GRGW;;;BU)"),
        Permissions::World => sddl.push_str("(A;;GRGW;;;WD)"),
    }
    sddl
}

/// The string form of the current user's SID.
fn current_user_sid() -> Option<String> {
    // SAFETY: plain Win32 calls; every buffer passed is sized as the API asked.
    unsafe {
        let mut token = 0;
        if OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) == 0 {
            return None;
        }
        let mut size = 0u32;
        GetTokenInformation(token, TokenUser, std::ptr::null_mut(), 0, &mut size);
        // u64 words keep the TOKEN_USER the call writes aligned.
        let mut buf = vec![0u64; ((size as usize + 7) / 8).max(1)];
        let mut result = None;
        if GetTokenInformation(token, TokenUser, buf.as_mut_ptr().cast(), size, &mut size) != 0 {
            let user = &*(buf.as_ptr() as *const TOKEN_USER);
            let mut str_ptr: *mut u16 = std::ptr::null_mut();
            if ConvertSidToStringSidW(user.User.Sid, &mut str_ptr) != 0 {
                let mut len = 0;
                while *str_ptr.add(len) != 0 {
                    len += 1;
                }
                result = String::from_utf16(std::slice::from_raw_parts(str_ptr, len)).ok();
                LocalFree(str_ptr as _);
            }
        }
        CloseHandle(token);
        result
    }
}

/// An owned security descriptor, freed on drop.
pub(crate) struct SecurityDescriptor(*mut c_void);

// SAFETY: the descriptor is immutable memory owned by this value.
unsafe impl Send for SecurityDescriptor {}
unsafe impl Sync for SecurityDescriptor {}

impl SecurityDescriptor {
    pub fn new(level: Permissions) -> Option<SecurityDescriptor> {
        let sddl: Vec<u16> = make_sddl(level, &current_user_sid()?)
            .encode_utf16()
            .chain(Some(0))
            .collect();
        let mut sd: *mut c_void = std::ptr::null_mut();
        // SAFETY: `sddl` is NUL-terminated; `sd` receives a LocalAlloc'd block.
        let ok = unsafe {
            ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.as_ptr(),
                SDDL_REVISION_1,
                &mut sd,
                std::ptr::null_mut(),
            )
        };
        (ok != 0).then_some(SecurityDescriptor(sd))
    }

    pub fn as_ptr(&self) -> *mut c_void {
        self.0
    }
}

impl Drop for SecurityDescriptor {
    fn drop(&mut self) {
        // SAFETY: allocated by the conversion call above.
        unsafe { LocalFree(self.0 as _) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn levels_widen_access() {
        assert_eq!(
            make_sddl(Permissions::User, "S-1"),
            "D:P(A;;GA;;;S-1)(A;;GA;;;SY)"
        );
        assert!(make_sddl(Permissions::Group, "S-1").ends_with("(A;;GRGW;;;BU)"));
        assert!(make_sddl(Permissions::World, "S-1").ends_with("(A;;GRGW;;;WD)"));
    }
}
