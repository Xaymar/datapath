// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! One I/O thread: a poller, the targets registered with it, and a queue of
//! notifications posted from other threads.

use super::context::ContextShared;
use super::platform::apply_thread_priority;
use super::poller::{Events, Poller, WAKE_KEY};
use super::{key_of, Target};
use crate::conn::lock;
use crate::spin::spin_then_block;
use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::os::fd::AsFd;
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::Duration;

thread_local! {
    /// The context whose I/O thread is running on this thread, if any.
    static CURRENT_CONTEXT: Cell<usize> = const { Cell::new(0) };
}

/// True on any I/O thread of the context with this id.
pub(crate) fn on_io_thread_of(ctx_id: usize) -> bool {
    CURRENT_CONTEXT.with(|c| c.get() == ctx_id)
}

struct Posts {
    /// `None` target = quit.
    items: Vec<(Option<Arc<dyn Target>>, i32)>,
    signaled: bool,
    /// The thread has been joined: posts are refused.
    dead: bool,
}

pub(crate) struct Loop {
    poller: Poller,
    posts: Mutex<Posts>,
    registered: Mutex<HashMap<u64, Arc<dyn Target>>>,
    thread: Mutex<Option<JoinHandle<()>>>,
}

impl Loop {
    pub fn new() -> std::io::Result<Arc<Loop>> {
        Ok(Arc::new(Loop {
            poller: Poller::new()?,
            posts: Mutex::new(Posts {
                items: Vec::new(),
                signaled: false,
                dead: false,
            }),
            registered: Mutex::new(HashMap::new()),
            thread: Mutex::new(None),
        }))
    }

    /// Start the thread. It keeps `ctx` alive, so a context is never destroyed
    /// on one of its own I/O threads.
    pub fn start(self: &Arc<Self>, ctx: Arc<ContextShared>) -> std::io::Result<()> {
        let me = self.clone();
        let handle = std::thread::Builder::new()
            .name("datapath-io".into())
            .spawn(move || {
                CURRENT_CONTEXT.with(|c| c.set(ctx.id()));
                apply_thread_priority(ctx.options().priority.level());
                me.run(&ctx);
            })?;
        *lock(&self.thread) = Some(handle);
        Ok(())
    }

    /// Register `fd` (edge-triggered) for `events`. The loop keeps `target`
    /// alive until [`unregister`](Self::unregister).
    pub fn add(&self, fd: impl AsFd, events: u32, target: Arc<dyn Target>) -> bool {
        let key = key_of(&target);
        lock(&self.registered).insert(key, target);
        if self.poller.add(fd, events, key) {
            true
        } else {
            lock(&self.registered).remove(&key);
            false
        }
    }

    pub fn modify(&self, fd: impl AsFd, old_events: u32, events: u32, key: u64) -> bool {
        self.poller.modify(fd, old_events, events, key)
    }

    pub fn remove_fd(&self, fd: impl AsFd) {
        self.poller.remove(fd);
    }

    /// Drop the loop's reference to the target with this key.
    pub fn unregister(&self, key: u64) {
        let dying = lock(&self.registered).remove(&key);
        // `dying` may be the last reference; it is released outside the lock.
        drop(dying);
    }

    /// Run `target.on_post(kind)` on this loop's thread. False if the thread is gone.
    pub fn post(&self, target: Option<Arc<dyn Target>>, kind: i32) -> bool {
        let signal;
        {
            let mut p = lock(&self.posts);
            if p.dead {
                return false;
            }
            p.items.push((target, kind));
            signal = !p.signaled;
            p.signaled = true;
        }
        if signal {
            self.poller.wake();
        }
        true
    }

    fn drain_posts(&self, quit: &Cell<bool>) -> bool {
        let items = {
            let mut p = lock(&self.posts);
            p.signaled = false;
            std::mem::take(&mut p.items)
        };
        let any = !items.is_empty();
        for (target, kind) in items {
            match target {
                None => quit.set(true),
                Some(t) => t.on_post(kind),
            }
        }
        any
    }

    /// Wait up to `timeout` ms and dispatch. False if nothing happened.
    fn pump(
        &self,
        timeout: i32,
        events: &RefCell<(Events, Vec<(u64, u32)>)>,
        quit: &Cell<bool>,
    ) -> bool {
        let mut state = events.borrow_mut();
        let (evs, out) = &mut *state;
        if self.poller.wait(timeout, evs, out) == 0 {
            return false;
        }
        let mut posts = false;
        for &(key, bits) in out.iter() {
            if key == WAKE_KEY {
                posts = true;
                continue;
            }
            let target = lock(&self.registered).get(&key).cloned();
            if let Some(t) = target {
                t.on_event(bits);
            }
        }
        drop(state);
        if posts {
            self.poller.drain_wake();
            self.drain_posts(quit);
        }
        true
    }

    fn run(&self, ctx: &ContextShared) {
        let events = RefCell::new((Poller::events(), Vec::new()));
        let quit = Cell::new(false);
        let spin: Duration = ctx.options().spin;
        let yield_cpu = ctx.options().yield_cpu;
        while !quit.get() {
            spin_then_block(
                spin,
                || !quit.get() && self.pump(0, &events, &quit),
                || {
                    if !quit.get() {
                        self.pump(-1, &events, &quit);
                    }
                },
                || {
                    if yield_cpu {
                        std::thread::yield_now();
                    } else {
                        crate::spin::cpu_relax();
                    }
                },
            );
        }
    }

    /// Ask the thread to quit after the notifications queued so far.
    pub fn request_quit(&self) {
        self.post(None, 0);
    }

    /// Join the thread, then run notifications that raced the quit request
    /// (for example `on_close`), and drop every registration to break
    /// context <-> target reference cycles.
    pub fn finish(&self) {
        let handle = lock(&self.thread).take();
        if let Some(h) = handle {
            let _ = h.join();
        }
        let items = {
            let mut p = lock(&self.posts);
            p.dead = true;
            std::mem::take(&mut p.items)
        };
        for (target, kind) in items {
            if let Some(t) = target {
                t.on_post(kind);
            }
        }
        let regs = std::mem::take(&mut *lock(&self.registered));
        drop(regs);
    }
}
