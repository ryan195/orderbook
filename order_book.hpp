#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

enum class Side {
    Buy,
    Sell
};

struct Order {
    std::uint64_t id;
    Side side;
    std::int64_t price;
    std::int64_t quantity;
};

struct Trade {
    std::uint64_t buy_order_id;
    std::uint64_t sell_order_id;
    std::int64_t price;
    std::int64_t quantity;
};

struct PriceLevel {
    std::int64_t price;
    std::int64_t quantity;
};

struct BookSnapshot {
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;

    std::optional<std::int64_t> best_bid;
    std::optional<std::int64_t> best_ask;
};

struct OrderResult {
    std::uint64_t order_id;
    std::vector<Trade> trades;
};

class OrderBook {
public:
    // The level cap must be at least the per-order cap. Both apply per stock.
    explicit OrderBook(
        std::int64_t max_order_quantity = 1000000,
        std::int64_t max_level_quantity = std::numeric_limits<std::int64_t>::max()
    );

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    // Add a limit order and return its ID and any resulting trades.
    OrderResult add_order(
        Side side,
        std::int64_t price,
        std::int64_t quantity
    );

    // Cancel an order that is still resting in the book.
    bool cancel_order(std::uint64_t order_id);

    // Return the top N price levels on each side.
    BookSnapshot snapshot(std::size_t levels) const;

private:
    using OrderQueue = std::list<Order>;
    struct RestingLevel {
        OrderQueue orders;
        std::int64_t quantity = 0;
    };
    using BidBook = std::map<std::int64_t, RestingLevel, std::greater<>>;
    using AskBook = std::map<std::int64_t, RestingLevel>;

    struct OrderLocation {
        Side side;
        std::int64_t price;
        OrderQueue::iterator order;
    };

    // Public operations hold this lock; private helpers require it already held.
    mutable std::mutex mutex_;
    const std::int64_t max_order_quantity_;
    const std::int64_t max_level_quantity_;

    BidBook bids_;
    AskBook asks_;

    // Used to locate an order when it is cancelled.
    std::unordered_map<std::uint64_t, OrderLocation> order_locations_;

    std::uint64_t next_order_id_;

    // Both map types share the same matching rules; only price direction differs.
    template <typename BookSide>
    void match_orders(Order& incoming, BookSide& opposite, std::vector<Trade>& trades);

    void rest_order(const Order& order);

};
