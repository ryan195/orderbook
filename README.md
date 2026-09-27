# Order Book API

A C++17 limit order book for one instrument, with an `Exchange` wrapper for
multiple independent stocks. The engine matches orders using
price-time priority and exposes a programmatic API; it needs no server or external
libraries. Matching logic is implemented directly with standard-library containers.

## Build and run

You need a C++17 compiler (Clang or GCC). From the project directory:

```sh
mkdir -p build
c++ -std=c++17 -pthread -Wall -Wextra -Wpedantic cli.cpp order_book.cpp -o build/orderbook-cli
./build/orderbook-cli
```

Type `help` to list commands. To replay the assignment example:

```sh
./build/orderbook-cli examples/assignment.txt
```

The replay buys 12 at a limit of 102, executes 5 at 101, 3 at 101 and 2 at 102,
and leaves a bid of 2 at 102 and an ask of 4 at 103. Prices are raw integer cents.
The CLI is the demo entry point; a separate hard-coded demo is unnecessary.

## Run the correctness tests

```sh
mkdir -p build
c++ -std=c++17 -pthread -Wall -Wextra -Wpedantic tests.cpp order_book.cpp -o build/tests
./build/tests
```

The runner prints a result for each of sixteen test groups and exits with a nonzero
status if any check fails. Checks stay active even with `-DNDEBUG`. No test framework
installation is needed.

Coverage includes:

- The assignment's exact trades and remaining book.
- Both matching directions, best price before arrival time across different levels,
  and stopping at the incoming limit price.
- FIFO within a price level, resting-price execution, partial fills that retain
  priority, exact fills, and incoming remainders.
- Cancellation at the front, middle and back of both queues, cancellation after a
  partial fill, and rejection of unknown, filled or already cancelled IDs.
- Empty books, non-crossing orders, sorted and aggregated depth, top-N truncation,
  zero-depth snapshots, best-price updates, and independent snapshot values.
- Invalid prices/quantities, sequential IDs, and no ID consumption on validation failure.

Optional undefined-behavior checking with a compiler that supports it:

```sh
c++ -std=c++17 -pthread -g -fsanitize=undefined tests.cpp order_book.cpp -o build/tests-ubsan
./build/tests-ubsan
```

## API example

```cpp
#include "order_book.hpp"

int main() {
    OrderBook book;
    auto sell = book.add_order(Side::Sell, 10125, 5); // 5 units at $101.25
    auto buy = book.add_order(Side::Buy, 10200, 2);   // Buy up to $102.00

    // buy.trades contains one trade:
    // {buy.order_id, sell.order_id, 10125, 2}
    auto depth = book.snapshot(5);
    // depth.best_bid is empty; depth.best_ask is 10125.
    // depth.asks contains {10125, 3}.

    bool cancelled = book.cancel_order(sell.order_id); // true: removes remaining 3
    return cancelled && buy.trades.size() == 1 && depth.best_ask == 10125 ? 0 : 1;
}
```

Compile your caller together with `order_book.cpp` and include `order_book.hpp`.

| Operation | Result |
| --- | --- |
| `add_order(side, price, quantity)` | An `OrderResult` containing the new ID and all trades in execution order. Any unfilled remainder rests until fully matched or explicitly cancelled. |
| `cancel_order(id)` | `true` if a resting order was removed, otherwise `false`. |
| `snapshot(N)` | Up to N aggregated price levels per side, best bids first and best asks first, plus optional best bid/ask prices. |

Every trade includes buy ID, sell ID, resting price and executed quantity.

## Choices and behavior

- **Prices:** positive signed 64-bit integers, interpreted as cents (10125 means
  $101.25). This avoids floating-point rounding. The assignment demo uses its raw
  values 101, 102 and 103 unchanged; these are integer units, not formatted dollars.
- **Quantities:** positive signed 64-bit integers representing whole units, capped
  at 1,000,000 units per order by default. Configure this with `OrderBook book(50000)`
  for a 50,000-unit cap. Non-positive constructor limits and orders above the cap
  throw `std::invalid_argument`; rejected orders do not consume IDs or liquidity.
  Zero or negative prices/quantities throw `std::invalid_argument` before changing
  the book or assigning an ID. Callers must use `Side::Buy` or `Side::Sell`.
- **IDs:** sequential unsigned 64-bit integers starting at 1 per `OrderBook`.
  Successful orders receive an ID even if immediately filled; cancelled IDs are
  not reused. IDs are not globally unique across separate books.
- **Priority:** lowest ask/highest bid first; within a level, serialized processing order
  determines FIFO priority. Concurrent requests are ordered by mutex acquisition,
  not by network arrival time. Partial fills keep the original queue position.
- **Execution:** all orders are limit orders. They match immediately when prices
  cross, at the resting order's price. Any unfilled quantity always stays in the
  book until fully matched or explicitly cancelled. There is no expiry or
  configurable time-in-force policy. Invalid side values throw `std::invalid_argument`.
- **Cancellation:** removes the entire remaining quantity. Unknown, fully filled,
  and already cancelled IDs all return `false`.
- **Snapshots:** owned copies, with quantities summed across orders at each level.
  Empty sides have `std::nullopt` best prices. `snapshot(0)` returns empty depth
  arrays but still reports best bid/ask.
- **Scope:** one instrument, in-memory state. Concurrent public operations on a
  shared book are serialized by one mutex. No persistence,
  network API, account ownership or self-trade prevention is implemented.

## Implementation guide

`add_order` validates input, assigns an ID, invokes `match_orders`, and rests a
remainder when appropriate. `match_orders` is shared by buys and sells: its map
argument provides best-price ordering, and the incoming side determines the limit
comparison and trade IDs. This keeps FIFO and fill accounting in one place.
`rest_order` copies the small order record into its list and indexes its position.
`cancel_order` uses that index; `snapshot` collects both sides through one local
helper. Each internal price level caches its remaining quantity; adds, fills and
cancellations update that total while holding the same book mutex.

The CLI separates number parsing, display, order submission, command dispatch,
and the input/replay loop. The randomized test generator is separate from the
reference engine; production matching helpers are deliberately not shared with
the reference, to preserve the value of comparing independent implementations.

## Data structures and trade-offs

Each side uses a `std::map` from price to a level containing a `std::list<Order>`
and its cached total quantity. Bids sort descending;
asks sort ascending. This makes the best price directly accessible and supports
FIFO insertion/removal at queue ends. An `std::unordered_map` maps order IDs to
side, price and a stable list iterator for direct cancellation.

Let L be the number of price levels on a side, M the number of matched resting
orders, and D the total number of price levels returned in a snapshot:

| Operation | Expected / amortized cost |
| --- | --- |
| Rest a non-crossing order | O(log L), with expected O(1) ID-index insertion |
| Match an incoming order | O(M + log L) including the level-cap lookup and any resting insertion; map erasure by iterator is amortized constant |
| Cancel an order | O(log L): locate the level, then erase the indexed list node in O(1) |
| Snapshot | O(D): read cached totals at the returned levels |
| Storage | O(number of resting orders + price levels) |

Hash-table operations have linear worst cases. Snapshot costs include output
allocation; best-price lookup itself is constant time.

List iterators stay valid when unrelated orders are added or removed. The ID index
points directly to a node, so cancellation does not scan a crowded level. A filled
or cancelled order is removed from the index before the public operation releases
the lock. The trade-off is a node allocation per order and poorer cache locality
than a deque. Cached totals make snapshots independent of the number of orders
at each returned price; tests compare them against independently recomputed totals.

All costs above exclude mutex waiting time. An entire add, cancel or snapshot
holds one book-wide lock. No output or network operations run under that lock.

## Limitations and extras

The project implements the required core plus an interactive command-line demo,
file replay, multiple stocks, and randomized reference testing.

Individual quantities and price-level totals use standard `int64_t`. The default
per-order cap is 1,000,000 and the default per-level cap is `INT64_MAX`. Configure
both with `OrderBook book(1000, 100000)` or `Exchange exchange(1000, 100000)`.
Both must be positive, and the level cap must be at least the per-order cap.
The level cap applies independently to each price and side in each stock.

Before assigning an ID or matching, an ordinary limit order that would overfill
an existing same-side level is rejected with `std::invalid_argument`, using the
safe check `quantity > cap - current_total`. An existing same-side level cannot
cross the opposite book, so such an order would rest in full. For new levels, the
per-order cap guarantees the remainder fits. Fills and cancellations
restore level capacity. Snapshots therefore cannot overflow from accepted totals.

Tests cover exact-cap acceptance, rejection without state/ID changes, restored
capacity, independent stock caps, concurrent admission, and `INT64_MAX`.
The randomized suite compares cached totals with sums from its reference model.
Per-player balances, ownership, request rates and open-order limits belong in the
game/server layer. Boundary stress tests explicitly raise the per-order cap.
After the last `uint64_t` ID is assigned, further additions throw
`std::overflow_error` before matching, rather than wrapping into reused IDs.
Allocation failures during mutations do not provide transactional rollback;
callers should not assume an allocation-failed operation left the book unchanged.
The test suite covers the listed normal and edge cases, not every possible input.

Possible future extensions include market orders and order amendments with
explicit rules for retaining or losing queue priority. Account integration is
outlined below.

## Files

- `order_book.hpp`: single-stock types and API, plus private container definitions.
- `exchange.hpp`: header-only stock registry and routing API.
- `order_book.cpp`: matching, cancellation and snapshots.
- `tests.cpp`: automated correctness tests.
- `randomized_tests.cpp`: independent reference model and reproducible randomized checks.
- `concurrency_tests.cpp`: competing buyers, snapshots, and cancellation races.
- `cli.cpp`: interactive command parser and replay runner.
- `examples/`: replayable command files.

## Interactive demo and replay

```sh
mkdir -p build
c++ -std=c++17 -pthread -Wall -Wextra -Wpedantic cli.cpp order_book.cpp -o build/orderbook-cli
./build/orderbook-cli
```

Enter commands such as:

```text
sell 101 5
buy 102 2
book 5
cancel 1
buy 102 10
quit
```

Commands are lowercase. The demo starts in stock `DEMO`, preserving existing
single-stock replay files. `stock AAPL` creates/selects a demo stock; `stocks` lists
registered symbols. Buy, sell, cancel and book commands apply to the selected stock.
Stock symbols are uppercase and case-sensitive. `buy PRICE QUANTITY` and
`sell PRICE QUANTITY` accept positive integer cents and whole quantities.
Every unfilled remainder stays in the book. After each add/cancel, the demo
shows trades (if any), best prices and up to five levels.
`book [N]` selects a depth (default five); `help` lists commands.

Save these same commands in a text file to replay them against a fresh empty book:

```sh
./build/orderbook-cli examples/assignment.txt
```

Replay prints each command before executing it. Blank lines and `#` comments are
allowed. Invalid commands report their line number. Interactive mode lets you
correct them; replay stops with a nonzero exit status at the first error. A missing
resting ID on cancellation is a valid operation, reported without stopping replay.
Replay files use the deterministic IDs assigned from 1 at the start of each run.
They are scenarios, not persistent snapshots or automatic session recordings.

## Randomized reference testing

```sh
c++ -std=c++17 -pthread -Wall -Wextra -Wpedantic randomized_tests.cpp order_book.cpp -o build/randomized-tests
./build/randomized-tests
```

The test runs 140,000 generated operations across seven workload profiles, each
with 20 fixed seeds: balanced, crowded single-price queues, wide books, extreme
64-bit values, buy-heavy, sell-heavy, and cancellation-heavy. Loading
phases ensure crowded queues and more than 100 price levels actually occur.
Coverage checks require totals exceeding `INT64_MAX / 1000`, at least 300 orders
at one price, and more than 100 levels on a side. Extreme prices reach `INT64_MAX`;
randomized quantities are bounded to keep conservation arithmetic within 64 bits.
The deterministic suite separately exercises the exact quantity limit and rejects
additions that would exceed the level cap.

After each operation it compares exact trades, IDs, cancellation results, full
untruncated depth and selected snapshot depths against an independent reference.
Additional checks verify quantity conservation, execution
limits, positive quantities, sorted levels, and an uncrossed book. Invalid prices,
quantities and enum values must leave the book and ID sequence unchanged. Cancels
target front, middle, back, random, and non-resting IDs; survivors are drained in
shuffled order with a comparison after every cancellation.

The reference uses an arrival-ordered flat vector and scans all orders to select
each match, rather than sharing the engine's maps and queues. This makes it slow
but easy to inspect and provides a second implementation of the rules. Fixed seeds
make failures reproducible; failures report the profile, seed, zero-based operation
index and current operation. This increases coverage but does not constitute a proof of correctness.

## Thread safety and contention tests

The simplest synchronization is one `std::mutex` per book. `add_order`,
`cancel_order` and `snapshot` hold it for their entire operation using an RAII
lock guard. Checking liquidity and reducing it occur within the same critical
section: a second buyer sees the remaining quantity, so it cannot execute against
liquidity already consumed by the first. Snapshots observe a consistent state.
Private helpers assume their caller already holds the lock and do not lock again.

Concurrent requests are processed in mutex acquisition order; the mutex does not
promise fairness or network-arrival ordering. Return values own their data and can
be processed after unlocking. Separate calls are not a transaction: a snapshot can
become stale before the caller submits an order. The actual matching operation
always rechecks the live book. Copying and moving `OrderBook` are disabled because
it owns a mutex and stores iterators into its own lists. The caller must keep it
alive until all threads finish using it.

For a 1,000-player game this is a simple starting point, not a capacity guarantee.
Load depends on requests per second, book depth and match sizes. Large sweeps or
snapshots block other callers until they finish. Use a small server worker pool;
there is no need for one thread or mutex per player. Player balances and account
updates need their own consistent integration outside this single-instrument API.

```sh
c++ -std=c++17 -pthread -O2 -Wall -Wextra -Wpedantic concurrency_tests.cpp order_book.cpp -o build/concurrency-tests
./build/concurrency-tests
```

The first test uses eight workers to submit 20,000 one-unit limit orders (20 each
for 1,000 logical players), while another thread requests snapshots. Only 1,000
units are available: it verifies exactly 1,000 execute, the remaining 19,000 rest,
IDs remain unique, and snapshots remain consistent. The second test races cancellation against matching
and checks that executed plus cancelled sell quantities equal the starting
liquidity, while unmatched buy quantities remain in the book.
Additional tests verify concurrent level-cap enforcement and multi-stock isolation.
The printed elapsed time is a local smoke measurement, not server latency or a
production benchmark; most buys in the first case find no sellers and become resting orders.

## Multiple stocks

A normal order book represents one instrument. `Exchange` owns a separate
`OrderBook` for each registered stock. Orders never match across stocks, even
when prices are equal. The original single-stock API remains usable on its own.

```cpp
#include "exchange.hpp"

int main() {
    Exchange exchange;
    exchange.add_stock("AAPL");
    exchange.add_stock("MSFT");
    auto apple = exchange.add_order("AAPL", Side::Sell, 100, 5);
    auto microsoft = exchange.add_order("MSFT", Side::Buy, 100, 5);
    // These orders do not match: they refer to different stocks.
    auto depth = exchange.snapshot("AAPL", 5);
    exchange.cancel_order("MSFT", microsoft.order_id);
    return depth.asks[0].quantity == 5 && apple.order_id == 1 ? 0 : 1;
}
```

`Exchange` is header-only; compile callers together with `order_book.cpp` and
`-pthread`. `Exchange(max_order_quantity, max_level_quantity)` configures the same
per-order and per-level caps for all its stocks. The second argument is optional. Registration returns `true` for a new symbol and `false` if already
registered, without resetting its book. Symbols must be 1-16 characters, start
with A-Z, and contain only A-Z, digits, dot or hyphen. Unknown symbols in order,
cancellation and snapshot calls throw `std::invalid_argument`; they do not create
books. `symbols()` returns the registered names in sorted order.

IDs are unique **within each stock**, so an order is identified by `(symbol, ID)`.
For example, AAPL #1 and MSFT #1 are different orders. Trade results belong to the
symbol passed to the call; retain that symbol when forwarding results to clients.
The CLI labels trades by stock, and `cancel 1` applies to the selected stock.

A short registry mutex protects lookup and registration, and is released before
entering an order book. Each book has its own mutex, so matching one stock does
not hold another stock's matching lock. There are no atomic cross-stock trades
or combined snapshots. Delisting is not supported; books live until the exchange
is destroyed, after callers have finished. Stock creation in this demo is for
experimentation; a server should expose registration only to authorized game
administration, not arbitrary players.

Try the saved scenario:

```sh
./build/orderbook-cli examples/multiple_stocks.txt
```

Tests cover independent books, overlapping IDs, stock-specific cancellation,
unknown/invalid symbols, duplicate registration, shared quantity limits, and
concurrent registration and trading across nine books.

## Why retain one mutex per stock?

We considered the delay caused by an order sweeping 10,000 resting orders. A
local optimized C++17 build (`-O2 -pthread`) was measured on the development Mac
on 2026-09-27. Each sample created a fresh book, inserted 10,000 one-unit sells
outside the timed region, then timed one buy matching all 10,000. Timing included
trade-result allocation, matching, quantity updates and removal of filled orders.
There was one warm-up and 30 measured runs per case, with no concurrent workload.

| Layout | Median | Minimum | Maximum |
| --- | --- | --- | --- |
| 10,000 sells at one price | 0.516 ms | 0.422 ms | 0.721 ms |
| 10,000 sells across 10,000 prices | 0.757 ms | 0.752 ms | 0.836 ms |

These measurements support keeping the straightforward mutex design for this
assignment and the anticipated game, rather than adding complicated concurrency.
They are local sweep durations, not a guarantee of server response time. A waiting
request must wait for the remaining operation and possibly other queued requests;
network latency, OS scheduling, different hardware and heavier workloads add time.
One thousand players alone does not determine load: request rate and order sizes
matter. Revisit this decision only if realistic server measurements show a problem.
Snapshots already use cached totals, and cancellation avoids queue scans. Matching
10,000 separate orders still needs O(10,000) work to emit the individual trades.

## Possible extension: player accounts and settlement

Funds, holdings and order ownership are deliberately outside the current
assignment's matching-engine requirements. The engine accepts valid orders
without knowing whether a player has cash or shares. A playable server would add
player IDs, order ownership checks, cash and share reservations, and settlement.
For example, a buy of ten shares at a $100 limit would reserve $1,000 before order
submission; fills debit their actual cost, and cancellation releases
unused reservations. Sell orders would reserve shares (unless the game explicitly
supports short selling). Cancellation would require the owning player's authority.

Reservation, matching results and account updates must be coordinated so concurrent
requests cannot spend the same funds twice. Rejection must release reservations;
settlement must avoid duplicate application. This is a separate game-server layer
and a useful future extension, rather than extra scope for this onboarding task.
