#include "order_book.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>

OrderBook::OrderBook(std::int64_t max_order_quantity, std::int64_t max_level_quantity)
    : max_order_quantity_(max_order_quantity), max_level_quantity_(max_level_quantity),
      next_order_id_(1) {
    if (max_order_quantity <= 0) {
        throw std::invalid_argument("Maximum order quantity must be positive");
    }
    if (max_level_quantity < max_order_quantity) {
        throw std::invalid_argument("Level quantity cap must be at least the per-order cap");
    }
}

// Prices are integer cents: 10125 means $101.25.
OrderResult OrderBook::add_order(
    Side side,
    std::int64_t price,
    std::int64_t quantity
) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (side != Side::Buy && side != Side::Sell) {
        throw std::invalid_argument("Invalid side");
    }
    if (price <= 0) {
        throw std::invalid_argument("Price must be positive");
    }

    if (quantity <= 0) {
        throw std::invalid_argument("Quantity must be positive");
    }

    if (quantity > max_order_quantity_) {
        throw std::invalid_argument("Quantity exceeds configured per-order limit");
    }

    const auto check_capacity = [&](const auto& same_side) {
        const auto level = same_side.find(price);
        if (level != same_side.end() && quantity > max_level_quantity_ - level->second.quantity) {
            throw std::invalid_argument("Order would exceed the price-level quantity cap");
        }
    };
    // An existing same-side level cannot cross the opposite book, so an
    // order at this exact price would rest in full. At a new level, the
    // per-order cap already guarantees that any remainder fits.
    if (side == Side::Buy) {
        check_capacity(bids_);
    } else {
        check_capacity(asks_);
    }

    // Unsigned increment wraps to zero after the final valid ID. Never reuse it.
    if (next_order_id_ == 0) {
        throw std::overflow_error("Order IDs exhausted");
    }

    Order incoming{
        next_order_id_++,
        side,
        price,
        quantity
    };

    std::vector<Trade> trades;

    if (side == Side::Buy) {
        match_orders(incoming, asks_, trades);
    } else {
        match_orders(incoming, bids_, trades);
    }

    // Every unfilled remainder rests until matched or cancelled.
    if (incoming.quantity > 0) {
        rest_order(incoming);
    }

    return {
        incoming.id,
        std::move(trades)
    };
}

template <typename BookSide>
void OrderBook::match_orders(Order& incoming, BookSide& opposite, std::vector<Trade>& trades) {
    const bool buying = incoming.side == Side::Buy;
    while (incoming.quantity > 0 && !opposite.empty()) {
        // Each side's map sorts its best price first.
        auto best_level = opposite.begin();
        const auto price = best_level->first;
        const bool crosses = buying ? incoming.price >= price : incoming.price <= price;
        if (!crosses) {
            break;
        }

        auto& level = best_level->second;
        auto& orders = level.orders;
        while (incoming.quantity > 0 && !orders.empty()) {
            // Matching the front preserves FIFO, including after partial fills.
            Order& resting = orders.front();
            const auto quantity = std::min(incoming.quantity, resting.quantity);
            trades.push_back({
                buying ? incoming.id : resting.id,
                buying ? resting.id : incoming.id,
                resting.price,
                quantity
            });
            incoming.quantity -= quantity;
            resting.quantity -= quantity;
            level.quantity -= quantity;

            if (resting.quantity == 0) {
                order_locations_.erase(resting.id);
                orders.pop_front();
            }
        }
        if (orders.empty()) {
            opposite.erase(best_level);
        }
    }
}

void OrderBook::rest_order(const Order& order) {
    const auto insert_into = [&](auto& side) {
        auto [position, created] = side.try_emplace(order.price);
        auto& level = position->second;
        try {
            level.orders.push_back(order);
        } catch (...) {
            if (created) side.erase(position);
            throw;
        }
        try {
            order_locations_.emplace(order.id,
                OrderLocation{order.side, order.price, std::prev(level.orders.end())});
        } catch (...) {
            // Keep the list, index and cached quantity consistent if allocation fails.
            level.orders.pop_back();
            if (created) side.erase(position);
            throw;
        }
        level.quantity += order.quantity; // Capacity was checked before matching.
    };
    if (order.side == Side::Buy) {
        insert_into(bids_);
    } else {
        insert_into(asks_);
    }
}

bool OrderBook::cancel_order(std::uint64_t order_id) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto location = order_locations_.find(order_id);
    if (location == order_locations_.end()) {
        return false;
    }

    // List iterators remain valid when other orders are added or removed.
    // The index therefore identifies the exact node without scanning the queue.
    const auto remove_from = [&](auto& side) {
        auto level = side.find(location->second.price);
        level->second.quantity -= location->second.order->quantity;
        level->second.orders.erase(location->second.order);
        if (level->second.orders.empty()) {
            side.erase(level);
        }
    };
    if (location->second.side == Side::Buy) {
        remove_from(bids_);
    } else {
        remove_from(asks_);
    }
    order_locations_.erase(location);
    return true;
}

BookSnapshot OrderBook::snapshot(std::size_t levels) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    BookSnapshot result;
    const auto collect_levels = [levels](const auto& side, auto& output, auto& best) {
        if (!side.empty()) {
            best = side.begin()->first;
        }
        for (const auto& [price, level] : side) {
            if (output.size() == levels) {
                break;
            }
            output.push_back({price, level.quantity});
        }
    };
    collect_levels(bids_, result.bids, result.best_bid);
    collect_levels(asks_, result.asks, result.best_ask);
    return result;
}
