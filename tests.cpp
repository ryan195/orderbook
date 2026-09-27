#include "order_book.hpp"
#include "exchange.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

// Unlike assert(), CHECK remains active in builds compiled with -DNDEBUG.
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)

void check_trade(const Trade& t, std::uint64_t buy, std::uint64_t sell,
                 std::int64_t price, std::int64_t quantity) {
    CHECK(t.buy_order_id == buy);
    CHECK(t.sell_order_id == sell);
    CHECK(t.price == price);
    CHECK(t.quantity == quantity);
}

void empty_book() {
    OrderBook b;
    auto s = b.snapshot(10);
    CHECK(s.bids.empty() && s.asks.empty());
    CHECK(!s.best_bid && !s.best_ask);
    CHECK(!b.cancel_order(0));
    CHECK(!b.cancel_order(999));
}

void assignment_example() {
    OrderBook b;
    auto s1 = b.add_order(Side::Sell, 101, 5);
    auto s2 = b.add_order(Side::Sell, 101, 3);
    auto s3 = b.add_order(Side::Sell, 102, 2);
    auto s4 = b.add_order(Side::Sell, 103, 4);
    auto buy = b.add_order(Side::Buy, 102, 12);
    CHECK(buy.trades.size() == 3);
    check_trade(buy.trades[0], buy.order_id, s1.order_id, 101, 5);
    check_trade(buy.trades[1], buy.order_id, s2.order_id, 101, 3);
    check_trade(buy.trades[2], buy.order_id, s3.order_id, 102, 2);
    auto s = b.snapshot(10);
    CHECK(s.best_bid == 102 && s.best_ask == 103);
    CHECK(s.bids.size() == 1 && s.bids[0].quantity == 2);
    CHECK(s.asks.size() == 1 && s.asks[0].quantity == 4);
    CHECK(!b.cancel_order(s1.order_id));
    CHECK(!b.cancel_order(s2.order_id));
    CHECK(!b.cancel_order(s3.order_id));
    CHECK(b.cancel_order(buy.order_id));
    CHECK(b.cancel_order(s4.order_id));
}

// Run the same behavioral checks in both directions to catch asymmetric bugs.
void price_priority(Side resting) {
    OrderBook b;
    const bool asks = resting == Side::Sell;
    const Side incoming = asks ? Side::Buy : Side::Sell;
    const auto worse = b.add_order(resting, asks ? 102 : 100, 3);
    const auto better = b.add_order(resting, 101, 2);
    const auto untouched = b.add_order(resting, asks ? 103 : 99, 7);
    const auto r = b.add_order(incoming, asks ? 102 : 100, 8);
    CHECK(r.trades.size() == 2);
    check_trade(r.trades[0], asks ? r.order_id : better.order_id,
                asks ? better.order_id : r.order_id, 101, 2);
    check_trade(r.trades[1], asks ? r.order_id : worse.order_id,
                asks ? worse.order_id : r.order_id, asks ? 102 : 100, 3);
    auto s = b.snapshot(10);
    CHECK(s.bids.size() == 1 && s.asks.size() == 1);
    CHECK((asks ? s.bids[0].quantity : s.asks[0].quantity) == 3);
    CHECK((asks ? s.asks[0].quantity : s.bids[0].quantity) == 7);
    CHECK(b.cancel_order(untouched.order_id));
    CHECK(b.cancel_order(r.order_id));
}

void fifo_and_partial_fill(Side resting) {
    OrderBook b;
    const bool asks = resting == Side::Sell;
    const Side incoming = asks ? Side::Buy : Side::Sell;
    auto first = b.add_order(resting, 100, 5);
    auto second = b.add_order(resting, 100, 4);
    auto r1 = b.add_order(incoming, asks ? 110 : 90, 2);
    CHECK(r1.trades.size() == 1);
    check_trade(r1.trades[0], asks ? r1.order_id : first.order_id,
                asks ? first.order_id : r1.order_id, 100, 2);
    CHECK(!b.cancel_order(r1.order_id));
    auto r2 = b.add_order(incoming, 100, 7);
    CHECK(r2.trades.size() == 2);
    check_trade(r2.trades[0], asks ? r2.order_id : first.order_id,
                asks ? first.order_id : r2.order_id, 100, 3);
    check_trade(r2.trades[1], asks ? r2.order_id : second.order_id,
                asks ? second.order_id : r2.order_id, 100, 4);
    auto s = b.snapshot(10);
    CHECK(s.bids.empty() && s.asks.empty());
    CHECK(!s.best_bid && !s.best_ask);
    CHECK(!b.cancel_order(first.order_id));
    CHECK(!b.cancel_order(second.order_id));
}

void cancellation(Side side) {
    // Cancel at every queue position; remaining orders must retain FIFO priority.
    for (int removed = 0; removed < 3; ++removed) {
        OrderBook b;
        OrderResult orders[3];
        for (auto& order : orders) order = b.add_order(side, 100, 2);
        CHECK(b.cancel_order(orders[removed].order_id));
        CHECK(!b.cancel_order(orders[removed].order_id));
        auto s = b.snapshot(1);
        CHECK((side == Side::Buy ? s.bids[0].quantity : s.asks[0].quantity) == 4);
        auto r = b.add_order(side == Side::Buy ? Side::Sell : Side::Buy, 100, 4);
        CHECK(r.trades.size() == 2);
        std::size_t trade = 0;
        for (int i = 0; i < 3; ++i) {
            if (i == removed) continue;
            check_trade(r.trades[trade++], side == Side::Buy ? orders[i].order_id : r.order_id,
                        side == Side::Sell ? orders[i].order_id : r.order_id, 100, 2);
        }
    }
    OrderBook b;
    auto order = b.add_order(side, 100, 5);
    b.add_order(side == Side::Buy ? Side::Sell : Side::Buy, 100, 2);
    CHECK(b.cancel_order(order.order_id)); // Cancel the unfilled remainder.
    auto s = b.snapshot(10);
    CHECK(!s.best_bid && !s.best_ask);
    CHECK(s.bids.empty() && s.asks.empty());
}

void snapshots_and_non_crossing() {
    OrderBook b;
    CHECK(b.add_order(Side::Buy, 98, 2).trades.empty());
    CHECK(b.add_order(Side::Buy, 99, 3).trades.empty());
    CHECK(b.add_order(Side::Buy, 99, 4).trades.empty());
    CHECK(b.add_order(Side::Sell, 102, 5).trades.empty());
    auto best = b.add_order(Side::Sell, 101, 6);
    CHECK(best.trades.empty());
    auto s = b.snapshot(10);
    CHECK(s.bids.size() == 2 && s.asks.size() == 2);
    CHECK(s.bids[0].price == 99 && s.bids[0].quantity == 7);
    CHECK(s.bids[1].price == 98 && s.bids[1].quantity == 2);
    CHECK(s.asks[0].price == 101 && s.asks[0].quantity == 6);
    CHECK(s.asks[1].price == 102 && s.asks[1].quantity == 5);
    CHECK(b.snapshot(1).bids.size() == 1 && b.snapshot(1).asks.size() == 1);
    auto zero = b.snapshot(0);
    CHECK(zero.bids.empty() && zero.asks.empty());
    CHECK(zero.best_bid == 99 && zero.best_ask == 101);
    CHECK(b.cancel_order(best.order_id));
    CHECK(b.snapshot(1).best_ask == 102);
    CHECK(s.best_ask == 101); // Snapshots are independent values.
}

void validation_and_ids() {
    OrderBook b;
    auto first = b.add_order(Side::Buy, 90, 1);
    for (Side side : {Side::Buy, Side::Sell}) {
        for (auto invalid : {0, -1}) {
            for (bool invalid_price : {false, true}) {
                bool threw = false;
                try { b.add_order(side, invalid_price ? invalid : 100,
                                  invalid_price ? 1 : invalid); }
                catch (const std::invalid_argument&) { threw = true; }
                CHECK(threw);
            }
        }
    }
    auto second = b.add_order(Side::Sell, 100, 1);
    CHECK(first.order_id == 1 && second.order_id == 2);
    CHECK(b.cancel_order(first.order_id));
    CHECK(b.add_order(Side::Buy, 90, 1).order_id == 3);
    auto s = b.snapshot(10);
    CHECK(s.bids.size() == 1 && s.bids[0].quantity == 1);
    CHECK(s.asks.size() == 1 && s.asks[0].quantity == 1);
}

void immediate_or_cancel(Side side) {
    const Side opposite = side == Side::Buy ? Side::Sell : Side::Buy;
    for (int available : {0, 3, 5, 8}) {
        OrderBook b;
        std::uint64_t resting_id = 0;
        if (available) resting_id = b.add_order(opposite, 100, available).order_id;
        auto r = b.add_order(side, 100, 5, TimeInForce::ImmediateOrCancel);
        CHECK(!b.cancel_order(r.order_id));
        CHECK(r.trades.size() == (available ? 1u : 0u));
        if (available) check_trade(r.trades[0], side == Side::Buy ? r.order_id : resting_id,
                                  side == Side::Sell ? r.order_id : resting_id,
                                  100, available < 5 ? available : 5);
        auto s = b.snapshot(10);
        const auto& own = side == Side::Buy ? s.bids : s.asks;
        const auto& other = side == Side::Buy ? s.asks : s.bids;
        CHECK(own.empty());
        CHECK(other.size() == (available > 5 ? 1u : 0u));
        if (available > 5) CHECK(other[0].quantity == 3);
    }
    OrderBook b;
    auto first = b.add_order(opposite, 100, 2);
    auto second = b.add_order(opposite, 100, 1);
    const auto next_price = side == Side::Buy ? 101 : 99;
    auto third = b.add_order(opposite, next_price, 2);
    auto beyond = b.add_order(opposite, side == Side::Buy ? 102 : 98, 4);
    auto r = b.add_order(side, next_price, 10, TimeInForce::ImmediateOrCancel);
    CHECK(r.trades.size() == 3);
    const std::uint64_t ids[] = {first.order_id, second.order_id, third.order_id};
    for (std::size_t i = 0; i < 3; ++i)
        check_trade(r.trades[i], side == Side::Buy ? r.order_id : ids[i],
                    side == Side::Sell ? r.order_id : ids[i], i == 2 ? next_price : 100,
                    i == 1 ? 1 : 2);
    CHECK(!b.cancel_order(r.order_id));
    auto blocked = b.add_order(side, 100, 1, TimeInForce::ImmediateOrCancel);
    CHECK(blocked.trades.empty());
    CHECK(!b.cancel_order(blocked.order_id));
    CHECK(b.cancel_order(beyond.order_id));
    CHECK(b.snapshot(10).bids.empty() && b.snapshot(10).asks.empty());
}

void invalid_enums() {
    OrderBook b;
    bool rejected = false;
    try { b.add_order(static_cast<Side>(99), 100, 1); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    rejected = false;
    try { b.add_order(Side::Buy, 100, 1, static_cast<TimeInForce>(99)); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    CHECK(b.add_order(Side::Buy, 100, 1).order_id == 1);
}

void quantity_boundaries() {
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    for (Side side : {Side::Buy, Side::Sell}) {
        OrderBook b(maximum);
        auto first = b.add_order(side, 100, maximum - 1);
        b.add_order(side, 100, 1);
        auto snapshot = b.snapshot(1);
        CHECK((side == Side::Buy ? snapshot.bids[0].quantity : snapshot.asks[0].quantity) == maximum);
        bool rejected = false;
        try { b.add_order(side, 100, 1); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        snapshot = b.snapshot(1);
        CHECK((side == Side::Buy ? snapshot.bids[0].quantity : snapshot.asks[0].quantity) == maximum);
        auto fill = b.add_order(side == Side::Buy ? Side::Sell : Side::Buy, 100, maximum);
        CHECK(fill.order_id == 3); // Rejected addition consumed no ID.
        CHECK(fill.trades.size() == 2);
        CHECK(fill.trades[0].quantity == maximum - 1 && fill.trades[1].quantity == 1);
        CHECK(!b.cancel_order(first.order_id));
        CHECK(!b.snapshot(1).best_bid && !b.snapshot(1).best_ask);
    }
}

void price_level_limits() {
    for (Side side : {Side::Buy, Side::Sell}) {
        OrderBook book(10, 12);
        auto first = book.add_order(side, 100, 8);
        book.add_order(side, 100, 4);
        bool rejected = false;
        try { book.add_order(side, 100, 1); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        auto depth = book.snapshot(1);
        CHECK((side == Side::Buy ? depth.bids[0].quantity : depth.asks[0].quantity) == 12);
        // An IOC order never rests, so the full same-side level must not reject it.
        auto ioc = book.add_order(side, 100, 10, TimeInForce::ImmediateOrCancel);
        CHECK(ioc.order_id == 3 && ioc.trades.empty());
        const auto opposite = side == Side::Buy ? Side::Sell : Side::Buy;
        book.add_order(opposite, 100, 3);
        depth = book.snapshot(1);
        CHECK((side == Side::Buy ? depth.bids[0].quantity : depth.asks[0].quantity) == 9);
        book.add_order(side, 100, 3); // A fill restored capacity.
        CHECK(book.cancel_order(first.order_id)); // Remove its remaining five.
        book.add_order(side, 100, 5); // Cancellation restored capacity too.
        depth = book.snapshot(1);
        CHECK((side == Side::Buy ? depth.bids[0].quantity : depth.asks[0].quantity) == 12);
        book.add_order(side, side == Side::Buy ? 99 : 101, 10); // Independent level.
    }
    for (auto invalid : {-1, 0, 9}) {
        bool rejected = false;
        try { OrderBook book(10, invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        rejected = false;
        try { Exchange exchange(10, invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    }
    Exchange exchange(10, 12);
    exchange.add_stock("AAPL");
    exchange.add_stock("MSFT");
    exchange.add_order("AAPL", Side::Buy, 100, 10);
    bool rejected = false;
    try { exchange.add_order("AAPL", Side::Buy, 100, 3); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    CHECK(exchange.add_order("MSFT", Side::Buy, 100, 10).order_id == 1);
    CHECK(exchange.add_order("AAPL", Side::Buy, 100, 2).order_id == 2);
}

void quantity_limits() {
    OrderBook b;
    bool rejected = false;
    try { b.add_order(Side::Buy, 100, 1000001); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    CHECK(b.add_order(Side::Buy, 100, 1000000).order_id == 1);
    OrderBook small(10);
    CHECK(small.add_order(Side::Sell, 100, 10).order_id == 1);
    rejected = false;
    try { small.add_order(Side::Buy, 100, 11, TimeInForce::ImmediateOrCancel); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    CHECK(small.snapshot(1).asks[0].quantity == 10);
    CHECK(small.add_order(Side::Buy, 100, 1).order_id == 2);
    for (auto limit : {0, -1}) {
        rejected = false;
        try { OrderBook invalid(limit); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    }
}

void multiple_stocks() {
    Exchange exchange;
    CHECK(exchange.add_stock("AAPL"));
    CHECK(exchange.add_stock("MSFT"));
    auto sell = exchange.add_order("AAPL", Side::Sell, 100, 5);
    auto buy = exchange.add_order("MSFT", Side::Buy, 100, 5);
    CHECK(sell.order_id == 1 && buy.order_id == 1);
    CHECK(buy.trades.empty()); // Equal prices in different stocks never cross.
    CHECK(!exchange.add_stock("AAPL")); // Must not reset an existing book.
    CHECK(exchange.snapshot("AAPL", 1).asks[0].quantity == 5);
    CHECK(exchange.cancel_order("MSFT", buy.order_id));
    CHECK(exchange.snapshot("AAPL", 1).asks[0].quantity == 5);
    auto fill = exchange.add_order("AAPL", Side::Buy, 100, 7, TimeInForce::ImmediateOrCancel);
    CHECK(fill.trades.size() == 1 && fill.trades[0].quantity == 5);
    CHECK(!exchange.cancel_order("AAPL", fill.order_id));
    CHECK(exchange.snapshot("AAPL", 1).asks.empty());
    CHECK(exchange.symbols() == std::vector<std::string>({"AAPL", "MSFT"}));
}

void stock_validation() {
    Exchange exchange(10);
    for (const auto* symbol : {"", "aapl", "BAD STOCK", "1ABC", "ABCDEFGHIJKLMNOPQ"}) {
        bool rejected = false;
        try { exchange.add_stock(symbol); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    }
    CHECK(exchange.symbols().empty());
    for (int operation = 0; operation < 3; ++operation) {
        bool rejected = false;
        try {
            if (operation == 0) exchange.add_order("UNKNOWN", Side::Buy, 1, 1);
            if (operation == 1) exchange.cancel_order("UNKNOWN", 1);
            if (operation == 2) exchange.snapshot("UNKNOWN", 1);
        } catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    }
    CHECK(exchange.symbols().empty());
    CHECK(exchange.add_stock("BRK.B"));
    bool rejected = false;
    try { exchange.add_order("BRK.B", Side::Buy, 100, 11); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    CHECK(exchange.add_order("BRK.B", Side::Buy, 100, 10).order_id == 1);
}

int main() {
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        {"empty book", empty_book},
        {"independent stock books", multiple_stocks},
        {"stock registration and validation", stock_validation},
        {"configurable quantity limits", quantity_limits},
        {"price-level caps and cached totals", price_level_limits},
        {"64-bit quantity boundaries and overflow detection", quantity_boundaries},
        {"buy IOC", [] { immediate_or_cancel(Side::Buy); }},
        {"sell IOC", [] { immediate_or_cancel(Side::Sell); }},
        {"invalid enum values", invalid_enums},
        {"assignment example", assignment_example},
        {"buy price priority and remainder", [] { price_priority(Side::Sell); }},
        {"sell price priority and remainder", [] { price_priority(Side::Buy); }},
        {"buy FIFO and partial fills", [] { fifo_and_partial_fill(Side::Sell); }},
        {"sell FIFO and partial fills", [] { fifo_and_partial_fill(Side::Buy); }},
        {"cancel bids", [] { cancellation(Side::Buy); }},
        {"cancel asks", [] { cancellation(Side::Sell); }},
        {"snapshots and non-crossing orders", snapshots_and_non_crossing},
        {"validation and IDs", validation_and_ids},
    };
    int failed = 0;
    for (const auto& test : tests) {
        try { test.run(); std::cout << "PASS: " << test.name << '\n'; }
        catch (const std::exception& e) {
            ++failed;
            std::cerr << "FAIL: " << test.name << " - " << e.what() << '\n';
        }
    }
    std::cout << (sizeof(tests) / sizeof(tests[0])) - failed << "/"
              << sizeof(tests) / sizeof(tests[0]) << " tests passed\n";
    return failed == 0 ? 0 : 1;
}
