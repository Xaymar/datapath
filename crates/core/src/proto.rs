// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! The wire format: an 8-byte little-endian length followed by the payload, after
//! a 16-byte handshake in each direction. Identical to the C++ core, so a Rust
//! and a C++ peer can talk to each other.

use crate::error::Error;

/// Size of the length prefix of every message.
pub const FRAME_HEADER_SIZE: usize = 8;
/// Protocol version carried in the handshake.
pub const PROTOCOL_VERSION: u32 = 1;
/// Size of the handshake.
pub const HELLO_SIZE: usize = 16;

/// The frame header for a payload of `length` bytes.
pub fn encode_frame_header(length: u64) -> [u8; FRAME_HEADER_SIZE] {
    length.to_le_bytes()
}

/// The connection handshake: `"DPTH"`, a `u32` version and the `u64` largest
/// message this side accepts, little-endian.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Hello {
    /// Protocol version of the sender.
    pub version: u32,
    /// Largest message the sender accepts.
    pub max_message_size: u64,
}

impl Hello {
    /// A handshake for the current protocol version.
    pub fn new(max_message_size: u64) -> Hello {
        Hello {
            version: PROTOCOL_VERSION,
            max_message_size,
        }
    }

    /// The bytes to send.
    pub fn encode(&self) -> [u8; HELLO_SIZE] {
        let mut out = [0u8; HELLO_SIZE];
        out[..4].copy_from_slice(b"DPTH");
        out[4..8].copy_from_slice(&self.version.to_le_bytes());
        out[8..].copy_from_slice(&self.max_message_size.to_le_bytes());
        out
    }

    /// Parse a received handshake. A wrong magic or a version other than
    /// [`PROTOCOL_VERSION`] is [`Error::VersionMismatch`].
    pub fn decode(bytes: &[u8; HELLO_SIZE]) -> Result<Hello, Error> {
        if &bytes[..4] != b"DPTH" {
            return Err(Error::VersionMismatch);
        }
        let version = u32::from_le_bytes(bytes[4..8].try_into().expect("4 bytes"));
        let max_message_size = u64::from_le_bytes(bytes[8..].try_into().expect("8 bytes"));
        if version != PROTOCOL_VERSION {
            return Err(Error::VersionMismatch);
        }
        Ok(Hello {
            version,
            max_message_size,
        })
    }
}

/// Smallest read the parser asks for before it has seen larger traffic.
const MIN_READ: usize = 64 * 1024;

/// Incremental, zero-copy frame parser for a byte stream.
///
/// ```
/// use datapath_core::proto::{encode_frame_header, FrameParser};
///
/// let mut parser = FrameParser::new(1 << 20, 4096);
/// let mut wire = encode_frame_header(5).to_vec();
/// wire.extend_from_slice(b"hello");
///
/// let room = parser.prepare();           // read up to room.len() bytes into room
/// room[..wire.len()].copy_from_slice(&wire);
/// let mut seen = Vec::new();
/// parser.commit(wire.len(), |msg| { seen.push(msg.to_vec()); true }).unwrap();
/// assert_eq!(seen, [b"hello".to_vec()]);
/// ```
///
/// Small frames are delivered straight out of the receive buffer. A frame larger
/// than the buffer gets an exact-size buffer of its own, which the following
/// reads fill directly and which is released when a small frame arrives again.
pub struct FrameParser {
    buf: Vec<u8>,
    rpos: usize,
    wpos: usize,
    big: Vec<u8>,
    big_filled: usize,
    in_big: bool,
    limit: u64,
    // Adaptive read size: small traffic keeps reads small (large read requests are
    // expensive for Windows pipes even when little data arrives); reads that fill
    // the request completely double it, up to the buffer size.
    read_hint: usize,
    last_request: usize,
}

impl FrameParser {
    /// A parser that rejects frames above `max_message_size` and receives into a
    /// buffer of `buffer_size` bytes (at least 64).
    pub fn new(max_message_size: u64, buffer_size: usize) -> FrameParser {
        let size = buffer_size.max(64);
        FrameParser {
            buf: vec![0; size],
            rpos: 0,
            wpos: 0,
            big: Vec::new(),
            big_filled: 0,
            in_big: false,
            limit: max_message_size,
            read_hint: size.min(MIN_READ),
            last_request: 0,
        }
    }

    /// Change the largest accepted frame (after the handshake negotiated it).
    pub fn set_limit(&mut self, max_message_size: u64) {
        self.limit = max_message_size;
    }

    fn compact(&mut self) {
        if self.rpos == 0 {
            return;
        }
        self.buf.copy_within(self.rpos..self.wpos, 0);
        self.wpos -= self.rpos;
        self.rpos = 0;
    }

    /// Where the next read should go. Pass the number of bytes actually read to
    /// [`commit`](Self::commit).
    pub fn prepare(&mut self) -> &mut [u8] {
        if self.in_big {
            let filled = self.big_filled;
            return &mut self.big[filled..];
        }
        if self.rpos == self.wpos {
            self.rpos = 0;
            self.wpos = 0;
        } else if self.wpos == self.buf.len() || self.buf.len() - self.wpos < FRAME_HEADER_SIZE {
            self.compact();
        }
        let room = self.buf.len() - self.wpos;
        self.last_request = room.min(self.read_hint);
        let start = self.wpos;
        &mut self.buf[start..start + self.last_request]
    }

    /// Account for `n` bytes read into the slice returned by
    /// [`prepare`](Self::prepare) and call `deliver` for every complete frame.
    ///
    /// `deliver` returns `false` to stop parsing (the connection was closed from
    /// a handler), which yields [`Error::Closed`]. A frame above the limit is
    /// [`Error::TooLarge`]; failing to allocate a large frame is
    /// [`Error::OutOfMemory`].
    ///
    /// # Panics
    /// If `n` is larger than the slice `prepare` returned.
    pub fn commit<F>(&mut self, n: usize, mut deliver: F) -> Result<(), Error>
    where
        F: FnMut(&[u8]) -> bool,
    {
        if self.in_big {
            assert!(
                n <= self.big.len() - self.big_filled,
                "commit larger than prepare"
            );
            self.big_filled += n;
            if self.big_filled < self.big.len() {
                return Ok(());
            }
            self.in_big = false;
            self.big_filled = 0;
            return if deliver(&self.big) {
                Ok(())
            } else {
                Err(Error::Closed)
            };
        }

        assert!(n <= self.last_request, "commit larger than prepare");
        self.wpos += n;
        if n == self.last_request && self.read_hint < self.buf.len() {
            self.read_hint = (self.read_hint * 2).min(self.buf.len());
        } else if n < self.read_hint / 8 && self.read_hint > MIN_READ {
            self.read_hint /= 2;
        }

        loop {
            let avail = self.wpos - self.rpos;
            if avail < FRAME_HEADER_SIZE {
                break;
            }
            let header: [u8; FRAME_HEADER_SIZE] = self.buf
                [self.rpos..self.rpos + FRAME_HEADER_SIZE]
                .try_into()
                .expect("8 bytes");
            let len = u64::from_le_bytes(header);
            if len > self.limit {
                return Err(Error::TooLarge);
            }
            // A length that does not fit usize can never fit the buffer either, and
            // is only reachable on 32-bit targets with a limit above 4 GiB.
            let len = usize::try_from(len).map_err(|_| Error::TooLarge)?;
            let have = avail - FRAME_HEADER_SIZE;
            if have >= len {
                if !self.big.is_empty() {
                    // Back to small frames: give the large buffer back.
                    self.big = Vec::new();
                }
                let start = self.rpos + FRAME_HEADER_SIZE;
                self.rpos = start + len;
                if !deliver(&self.buf[start..start + len]) {
                    return Err(Error::Closed);
                }
                continue;
            }
            if FRAME_HEADER_SIZE + len <= self.buf.len() {
                // Fits the buffer; make room for the rest and wait for more data.
                if self.rpos + FRAME_HEADER_SIZE + len > self.buf.len() {
                    self.compact();
                }
                break;
            }
            // Larger than the receive buffer: move to a dedicated buffer, reusing
            // the one of the previous large frame when it is big enough.
            if self.big.capacity() < len {
                self.big = Vec::new();
                self.big
                    .try_reserve_exact(len)
                    .map_err(|_| Error::OutOfMemory)?;
            }
            self.big.clear();
            self.big.resize(len, 0);
            let start = self.rpos + FRAME_HEADER_SIZE;
            self.big[..have].copy_from_slice(&self.buf[start..start + have]);
            self.big_filled = have;
            self.rpos = 0;
            self.wpos = 0;
            self.in_big = true;
            break;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame(payload: &[u8]) -> Vec<u8> {
        let mut v = encode_frame_header(payload.len() as u64).to_vec();
        v.extend_from_slice(payload);
        v
    }

    /// Feed `wire` to the parser in chunks of `chunk` bytes and collect the frames.
    fn feed(parser: &mut FrameParser, wire: &[u8], chunk: usize) -> Result<Vec<Vec<u8>>, Error> {
        let mut out = Vec::new();
        let mut pos = 0;
        while pos < wire.len() {
            let room = parser.prepare();
            let n = room.len().min(chunk).min(wire.len() - pos);
            room[..n].copy_from_slice(&wire[pos..pos + n]);
            pos += n;
            parser.commit(n, |m| {
                out.push(m.to_vec());
                true
            })?;
        }
        Ok(out)
    }

    #[test]
    fn hello_round_trips() {
        let h = Hello::new(64 << 20);
        let bytes = h.encode();
        assert_eq!(&bytes[..4], b"DPTH");
        assert_eq!(Hello::decode(&bytes), Ok(h));
    }

    #[test]
    fn hello_rejects_bad_magic_and_version() {
        let mut bytes = Hello::new(1).encode();
        bytes[0] = b'X';
        assert_eq!(Hello::decode(&bytes), Err(Error::VersionMismatch));

        let bytes = Hello {
            version: PROTOCOL_VERSION + 1,
            max_message_size: 1,
        }
        .encode();
        assert_eq!(Hello::decode(&bytes), Err(Error::VersionMismatch));
    }

    #[test]
    fn hello_is_little_endian() {
        let bytes = Hello {
            version: 1,
            max_message_size: 0x0102,
        }
        .encode();
        assert_eq!(&bytes[4..8], &[1, 0, 0, 0]);
        assert_eq!(&bytes[8..10], &[2, 1]);
    }

    #[test]
    fn frames_split_at_every_byte_offset() {
        let payloads: [&[u8]; 4] = [b"", b"a", b"hello world", &[7u8; 200]];
        let mut wire = Vec::new();
        for p in payloads {
            wire.extend_from_slice(&frame(p));
        }
        // Every way to cut the stream in two reads, then byte-by-byte.
        for cut in 0..=wire.len() {
            let mut parser = FrameParser::new(1 << 20, 64);
            let mut got = feed(&mut parser, &wire[..cut], usize::MAX).unwrap();
            got.extend(feed(&mut parser, &wire[cut..], usize::MAX).unwrap());
            assert_eq!(
                got,
                payloads.iter().map(|p| p.to_vec()).collect::<Vec<_>>(),
                "cut at {cut}"
            );
        }
        let mut parser = FrameParser::new(1 << 20, 64);
        let got = feed(&mut parser, &wire, 1).unwrap();
        assert_eq!(got.len(), 4);
    }

    #[test]
    fn many_small_frames_in_one_read() {
        let mut wire = Vec::new();
        for i in 0..100u8 {
            wire.extend_from_slice(&frame(&[i; 3]));
        }
        let mut parser = FrameParser::new(1 << 20, 4096);
        let got = feed(&mut parser, &wire, usize::MAX).unwrap();
        assert_eq!(got.len(), 100);
        assert_eq!(got[42], vec![42u8; 3]);
    }

    #[test]
    fn frame_larger_than_the_buffer_uses_its_own_buffer() {
        let payload: Vec<u8> = (0..5000u32).map(|i| (i % 251) as u8).collect();
        let mut wire = frame(&payload);
        wire.extend_from_slice(&frame(b"after"));
        for chunk in [1, 7, 64, 1000, usize::MAX] {
            let mut parser = FrameParser::new(1 << 20, 64);
            let got = feed(&mut parser, &wire, chunk).unwrap();
            assert_eq!(
                got,
                vec![payload.clone(), b"after".to_vec()],
                "chunk {chunk}"
            );
        }
    }

    #[test]
    fn large_buffer_is_reused_then_released() {
        let big = vec![1u8; 1000];
        let mut parser = FrameParser::new(1 << 20, 64);
        feed(&mut parser, &frame(&big), usize::MAX).unwrap();
        feed(&mut parser, &frame(&big), usize::MAX).unwrap();
        assert!(parser.big.capacity() >= 1000);
        feed(&mut parser, &frame(b"x"), usize::MAX).unwrap();
        assert_eq!(parser.big.capacity(), 0);
    }

    #[test]
    fn frame_above_the_limit_is_rejected() {
        let mut parser = FrameParser::new(10, 64);
        assert_eq!(
            feed(&mut parser, &frame(&[0u8; 11]), usize::MAX),
            Err(Error::TooLarge)
        );
        // The limit can be lowered after the handshake.
        let mut parser = FrameParser::new(100, 64);
        parser.set_limit(2);
        assert_eq!(
            feed(&mut parser, &frame(b"abc"), usize::MAX),
            Err(Error::TooLarge)
        );
    }

    #[test]
    fn deliver_returning_false_stops_with_closed() {
        let mut wire = frame(b"one");
        wire.extend_from_slice(&frame(b"two"));
        let mut parser = FrameParser::new(1 << 20, 4096);
        let room = parser.prepare();
        room[..wire.len()].copy_from_slice(&wire);
        let mut seen = 0;
        let r = parser.commit(wire.len(), |_| {
            seen += 1;
            false
        });
        assert_eq!(r, Err(Error::Closed));
        assert_eq!(seen, 1);
    }

    #[test]
    fn closing_from_a_large_frame_handler_stops_too() {
        let wire = frame(&[9u8; 300]);
        let mut parser = FrameParser::new(1 << 20, 64);
        let mut pos = 0;
        let mut result = Ok(());
        while pos < wire.len() {
            let room = parser.prepare();
            let n = room.len().min(wire.len() - pos);
            room[..n].copy_from_slice(&wire[pos..pos + n]);
            pos += n;
            result = parser.commit(n, |_| false);
        }
        assert_eq!(result, Err(Error::Closed));
    }

    #[test]
    fn read_size_adapts_to_traffic() {
        let mut parser = FrameParser::new(1 << 20, 256 * 1024);
        assert_eq!(parser.prepare().len(), MIN_READ);
        // A read that fills the request doubles the next one.
        let n = parser.prepare().len();
        parser.commit(n, |_| true).unwrap();
        assert_eq!(parser.prepare().len(), 2 * MIN_READ);
    }

    #[test]
    #[should_panic(expected = "commit larger than prepare")]
    fn commit_larger_than_prepare_panics() {
        let mut parser = FrameParser::new(1 << 20, 64);
        let n = parser.prepare().len();
        let _ = parser.commit(n + 1, |_| true);
    }
}
