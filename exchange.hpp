#pragma once

#include "order_book.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

// Routes requests to independent single-instrument books. No delisting is
// supported, so books remain alive until the Exchange itself is destroyed.
class Exchange {
public:
    explicit Exchange(
        std::int64_t max_order_quantity = 1000000,
        std::int64_t max_level_quantity = std::numeric_limits<std::int64_t>::max()
    ) : max_order_quantity_(max_order_quantity), max_level_quantity_(max_level_quantity) {
        if (max_order_quantity <= 0) {
            throw std::invalid_argument("Maximum order quantity must be positive");
        }
        if (max_level_quantity < max_order_quantity) {
            throw std::invalid_argument("Level quantity cap must be at least the per-order cap");
        }
    }

    // Symbols are case-sensitive uppercase identifiers, e.g. AAPL or BRK.B.
    // Registration is explicit so a typo in an API order cannot create a stock.
    bool add_stock(const std::string& symbol) {
        validate_symbol(symbol);
        const std::lock_guard<std::mutex> lock(mutex_);
        if (books_.count(symbol)) {
            return false;
        }
        books_.emplace(symbol, std::make_unique<OrderBook>(max_order_quantity_, max_level_quantity_));
        return true;
    }

    std::vector<std::string> symbols() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result;
        result.reserve(books_.size());
        for (const auto& [symbol, book] : books_) {
            result.push_back(symbol);
        }
        return result;
    }

    OrderResult add_order(const std::string& symbol, Side side, std::int64_t price,
                          std::int64_t quantity) {
        return find_book(symbol).add_order(side, price, quantity);
    }

    bool cancel_order(const std::string& symbol, std::uint64_t order_id) {
        return find_book(symbol).cancel_order(order_id);
    }

    BookSnapshot snapshot(const std::string& symbol, std::size_t levels) const {
        return find_book(symbol).snapshot(levels);
    }

private:
    static void validate_symbol(const std::string& symbol) {
        if (symbol.empty() || symbol.size() > 16 || symbol.front() < 'A' || symbol.front() > 'Z') {
            throw std::invalid_argument("Symbol must start with A-Z and have 1-16 characters");
        }
        for (char character : symbol) {
            const bool letter = character >= 'A' && character <= 'Z';
            const bool digit = character >= '0' && character <= '9';
            if (!letter && !digit && character != '.' && character != '-') {
                throw std::invalid_argument("Symbol may contain only A-Z, 0-9, dot or hyphen");
            }
        }
    }

    OrderBook& find_book(const std::string& symbol) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        auto found = books_.find(symbol);
        if (found == books_.end()) {
            throw std::invalid_argument("Unknown stock: " + symbol);
        }
        // Release registry lock before taking a book lock. Registry insertions do
        // not move heap-allocated books, and the API never removes them.
        return *found->second;
    }

    const std::int64_t max_order_quantity_;
    const std::int64_t max_level_quantity_;
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<OrderBook>> books_;
};
