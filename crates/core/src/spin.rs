// Low Latency IPC Library for high-speed traffic
// Copyright (C) 2019 - 2026 Narta Xaymar Dirks <info@xaymar.com>
//
// SPDX-License-Identifier: BSD-3-Clause

//! Busy-poll for a short budget, then block.

use std::time::{Duration, Instant};

/// Tell the CPU this is a spin-wait loop (`pause` on x86, `yield` on ARM).
#[inline]
pub fn cpu_relax() {
    std::hint::spin_loop();
}

/// Busy-poll with `poll` until `budget` passes without work, then call `block`
/// once. `poll` returns `true` when it handled work, which restarts the budget.
/// `relax` runs between idle polls; pass [`cpu_relax`], or a function that also
/// yields the CPU.
///
/// A zero `budget` never polls: `block` runs right away, which is what the
/// `spin = 0` option means.
pub fn spin_then_block<P, B, R>(budget: Duration, mut poll: P, block: B, mut relax: R)
where
    P: FnMut() -> bool,
    B: FnOnce(),
    R: FnMut(),
{
    if !budget.is_zero() {
        let mut deadline = Instant::now() + budget;
        loop {
            if poll() {
                deadline = Instant::now() + budget;
                continue;
            }
            if Instant::now() >= deadline {
                break;
            }
            relax();
        }
    }
    block();
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    #[test]
    fn zero_budget_blocks_without_polling() {
        let polls = Cell::new(0);
        let blocks = Cell::new(0);
        spin_then_block(
            Duration::ZERO,
            || {
                polls.set(polls.get() + 1);
                false
            },
            || blocks.set(blocks.get() + 1),
            cpu_relax,
        );
        assert_eq!(polls.get(), 0);
        assert_eq!(blocks.get(), 1);
    }

    #[test]
    fn idle_polls_relax_then_block_once() {
        let polls = Cell::new(0u32);
        let relaxes = Cell::new(0u32);
        let blocks = Cell::new(0);
        let start = Instant::now();
        spin_then_block(
            Duration::from_millis(5),
            || {
                polls.set(polls.get() + 1);
                false
            },
            || blocks.set(blocks.get() + 1),
            || relaxes.set(relaxes.get() + 1),
        );
        assert!(start.elapsed() >= Duration::from_millis(5));
        assert_eq!(blocks.get(), 1);
        assert!(polls.get() >= 1);
        // `relax` runs after every idle poll except the one that ends the budget.
        assert!(relaxes.get() + 1 >= polls.get());
    }

    #[test]
    fn work_restarts_the_budget() {
        // Each poll that finds work takes longer than the budget would allow
        // without the restart, so the loop only ends after the work stops.
        let work_left = Cell::new(5);
        let handled = Cell::new(0);
        spin_then_block(
            Duration::from_millis(20),
            || {
                if work_left.get() > 0 {
                    work_left.set(work_left.get() - 1);
                    handled.set(handled.get() + 1);
                    std::thread::sleep(Duration::from_millis(10));
                    true
                } else {
                    false
                }
            },
            || {},
            cpu_relax,
        );
        assert_eq!(handled.get(), 5);
    }
}
