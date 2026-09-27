#include "order_book.hpp"
#include "exchange.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// A small server worker pool handles requests from 1,000 logical players.
// Each player submits 20 limit buys, but only 1,000 units exist to trade.
void competing_buyers() {
    OrderBook book;
    const auto supply = book.add_order(Side::Sell, 100, 1000);
    constexpr unsigned workers = 8;
    constexpr unsigned requests_per_worker = 2500;
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<bool> bad_snapshot{false};
    std::vector<OrderResult> results[workers];
    std::vector<std::thread> threads;
    for (unsigned worker = 0; worker < workers; ++worker) {
        results[worker].reserve(requests_per_worker);
        threads.emplace_back([&, worker] {
            while (!start.load()) std::this_thread::yield();
            for (unsigned i = 0; i < requests_per_worker; ++i)
                results[worker].push_back(book.add_order(Side::Buy, 100, 1));
        });
    }
    std::thread reader([&] {
        while (!start.load()) std::this_thread::yield();
        while (!done.load()) {
            auto s = book.snapshot(1);
            if (s.bids.size() > 1 || s.asks.size() > 1 ||
                (!s.bids.empty() && !s.asks.empty()) ||
                (!s.bids.empty() && (s.bids[0].quantity <= 0 || s.bids[0].quantity > 19000)) ||
                (!s.asks.empty() && (s.asks[0].quantity <= 0 || s.asks[0].quantity > 1000)))
                bad_snapshot = true;
            std::this_thread::yield();
        }
    });
    const auto begin = std::chrono::steady_clock::now();
    start = true;
    for (auto& thread : threads) thread.join();
    done = true;
    reader.join();
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    std::set<std::uint64_t> ids;
    std::int64_t executed = 0;
    for (const auto& batch : results) for (const auto& result : batch) {
        require(ids.insert(result.order_id).second, "duplicate order ID under contention");
        require(result.trades.size() <= 1, "one-unit order executed more than once");
        for (auto trade : result.trades) {
            require(trade.sell_order_id == supply.order_id && trade.buy_order_id == result.order_id
                    && trade.price == 100 && trade.quantity == 1, "invalid concurrent trade");
            executed += trade.quantity;
        }
    }
    require(executed == 1000, "competing buyers overfilled or underfilled available liquidity");
    require(!bad_snapshot, "inconsistent concurrent snapshot");
    auto final = book.snapshot(1);
    require(final.asks.empty() && !final.best_ask && final.bids.size() == 1
            && final.best_bid == 100 && final.bids[0].quantity == 19000, "unfilled buys did not rest");
    std::cout << "PASS: 20,000 competing limit orders, exactly 1,000 units filled ("
              << elapsed << " ms locally, including concurrent snapshots)\n";
}

void cancellation_vs_matching() {
    OrderBook book;
    for (unsigned i = 0; i < 1000; ++i) book.add_order(Side::Sell, 100, 1);
    std::atomic<bool> start{false};
    std::atomic<unsigned> cancelled{0};
    std::vector<Trade> trades[4];
    std::vector<std::thread> threads;
    for (unsigned worker = 0; worker < 4; ++worker) {
        threads.emplace_back([&, worker] {
            while (!start.load()) std::this_thread::yield();
            for (unsigned i = worker; i < 1000; i += 4)
                if (book.cancel_order(i + 1)) ++cancelled;
        });
        threads.emplace_back([&, worker] {
            while (!start.load()) std::this_thread::yield();
            for (unsigned i = 0; i < 250; ++i) {
                auto result = book.add_order(Side::Buy, 100, 1);
                trades[worker].insert(trades[worker].end(), result.trades.begin(), result.trades.end());
            }
        });
    }
    start = true;
    for (auto& thread : threads) thread.join();
    std::set<std::uint64_t> filled_ids;
    for (const auto& batch : trades) for (auto trade : batch) {
        require(trade.quantity == 1 && trade.price == 100, "incorrect execution");
        require(filled_ids.insert(trade.sell_order_id).second, "resting order filled twice");
    }
    require(filled_ids.size() + cancelled.load() == 1000, "same liquidity cancelled and traded, or lost");
    auto s = book.snapshot(10);
    require(s.asks.empty(), "sell liquidity left after concurrent drain");
    const auto resting_buys = s.bids.empty() ? 0 : s.bids[0].quantity;
    require(resting_buys == cancelled.load(), "unfilled buys did not conserve quantity");
    std::cout << "PASS: cancellation races with matching without double-consuming liquidity\n";
}

void concurrent_stocks() {
    Exchange exchange;
    std::atomic<unsigned> registrations{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    for (unsigned worker = 0; worker < 8; ++worker) {
        threads.emplace_back([&, worker] {
            while (!start.load()) std::this_thread::yield();
            if (exchange.add_stock("SHARED")) ++registrations;
            const auto symbol = "STOCK" + std::to_string(worker);
            exchange.add_stock(symbol);
            for (unsigned i = 0; i < 100; ++i) {
                exchange.add_order("SHARED", Side::Buy, 100, 1);
                exchange.add_order(symbol, Side::Sell, 100, 1);
                exchange.snapshot(symbol, 1);
                exchange.symbols();
            }
        });
    }
    start = true;
    for (auto& thread : threads) thread.join();
    require(registrations == 1, "same stock registered more than once");
    require(exchange.symbols().size() == 9, "missing stocks");
    require(exchange.snapshot("SHARED", 1).bids[0].quantity == 800, "shared stock lost orders");
    for (unsigned worker = 0; worker < 8; ++worker)
        require(exchange.snapshot("STOCK" + std::to_string(worker), 1).asks[0].quantity == 100,
                "orders crossed between stocks");
    std::cout << "PASS: concurrent stock registration and isolated trading across nine books\n";
}

void concurrent_level_cap() {
    OrderBook book(1, 1000);
    std::atomic<bool> start{false};
    std::atomic<unsigned> accepted{0};
    std::atomic<unsigned> rejected{0};
    std::vector<std::thread> threads;
    for (unsigned worker = 0; worker < 8; ++worker) {
        threads.emplace_back([&] {
            while (!start.load()) std::this_thread::yield();
            for (unsigned i = 0; i < 250; ++i) {
                try {
                    book.add_order(Side::Buy, 100, 1);
                    ++accepted;
                } catch (const std::invalid_argument&) {
                    ++rejected;
                }
            }
        });
    }
    start = true;
    for (auto& thread : threads) thread.join();
    require(accepted == 1000 && rejected == 1000, "concurrent orders exceeded level cap");
    auto snapshot = book.snapshot(1);
    require(snapshot.bids.size() == 1 && snapshot.bids[0].quantity == 1000, "cached total differs");
    require(book.cancel_order(1), "could not restore capacity");
    require(book.add_order(Side::Buy, 100, 1).order_id == 1001, "rejected orders consumed IDs");
    std::cout << "PASS: concurrent additions enforce the level cap without consuming rejected IDs\n";
}

int main() {
    try {
        competing_buyers();
        concurrent_level_cap();
        concurrent_stocks();
        cancellation_vs_matching();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
