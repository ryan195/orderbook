#include "order_book.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

// Deliberately slow reference: an arrival-ordered flat vector, with a full scan
// for every match. It does not share the engine's price maps or matching code.
class ReferenceBook {
public:
    std::vector<Order> orders;
    std::uint64_t next_id = 1;

    OrderResult add(Side side, std::int64_t price, std::int64_t quantity) {
        Order incoming{next_id++, side, price, quantity};
        OrderResult result{incoming.id, {}};
        while (incoming.quantity > 0) {
            auto best = orders.end();
            for (auto it = orders.begin(); it != orders.end(); ++it) {
                if (it->side == side) continue;
                if (side == Side::Buy ? it->price > price : it->price < price) continue;
                if (best == orders.end() || (side == Side::Buy ? it->price < best->price : it->price > best->price))
                    best = it; // Equal prices preserve arrival order.
            }
            if (best == orders.end()) break;
            auto quantity_traded = std::min(incoming.quantity, best->quantity);
            result.trades.push_back({side == Side::Buy ? incoming.id : best->id,
                                     side == Side::Sell ? incoming.id : best->id,
                                     best->price, quantity_traded});
            incoming.quantity -= quantity_traded;
            best->quantity -= quantity_traded;
            if (!best->quantity) orders.erase(best);
        }
        if (incoming.quantity > 0) orders.push_back(incoming);
        return result;
    }

    bool cancel(std::uint64_t id) {
        auto it = std::find_if(orders.begin(), orders.end(), [id](auto o) { return o.id == id; });
        if (it == orders.end()) return false;
        orders.erase(it);
        return true;
    }

    BookSnapshot snapshot(std::size_t depth) const {
        BookSnapshot result;
        for (Side side : {Side::Buy, Side::Sell}) {
            std::vector<Order> sorted;
            for (auto o : orders) if (o.side == side) sorted.push_back(o);
            std::sort(sorted.begin(), sorted.end(), [side](auto a, auto b) {
                return side == Side::Buy ? a.price > b.price : a.price < b.price;
            });
            auto& levels = side == Side::Buy ? result.bids : result.asks;
            for (auto o : sorted) {
                if (levels.empty() || levels.back().price != o.price) levels.push_back({o.price, 0});
                levels.back().quantity += o.quantity;
            }
            auto& best = side == Side::Buy ? result.best_bid : result.best_ask;
            if (!levels.empty()) best = levels.front().price;
            if (levels.size() > depth) levels.resize(depth);
        }
        return result;
    }
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void compare(const BookSnapshot& a, const BookSnapshot& b) {
    require(a.best_bid == b.best_bid && a.best_ask == b.best_ask, "best price differs");
    for (bool bids : {true, false}) {
        const auto& x = bids ? a.bids : a.asks;
        const auto& y = bids ? b.bids : b.asks;
        require(x.size() == y.size(), "depth size differs");
        for (std::size_t i = 0; i < x.size(); ++i)
            require(x[i].price == y[i].price && x[i].quantity == y[i].quantity, "level differs");
    }
    require(!a.best_bid || !a.best_ask || *a.best_bid < *a.best_ask, "book is crossed");
}


// Each profile stresses a different shape of book, rather than only increasing
// the number of samples drawn from one small distribution.
enum class Profile { Balanced, Crowded, Wide, Extremes, BuyHeavy, SellHeavy, CancelHeavy };
const char* names[] = {"balanced", "crowded", "wide", "extremes", "buy-heavy",
                       "sell-heavy", "cancel-heavy"};
constexpr auto max_quantity = std::numeric_limits<std::int64_t>::max();
constexpr auto full_depth = std::numeric_limits<std::size_t>::max();

std::int64_t resting_quantity(const BookSnapshot& snapshot) {
    std::int64_t total = 0;
    for (auto level : snapshot.bids) total += level.quantity;
    for (auto level : snapshot.asks) total += level.quantity;
    return total;
}

void invariants(const BookSnapshot& snapshot) {
    for (bool bids : {true, false}) {
        const auto& levels = bids ? snapshot.bids : snapshot.asks;
        for (std::size_t i = 0; i < levels.size(); ++i) {
            require(levels[i].quantity > 0 && levels[i].price > 0, "non-positive level");
            if (i) require(bids ? levels[i - 1].price > levels[i].price
                                : levels[i - 1].price < levels[i].price, "levels not strictly sorted");
        }
    }
}

void reject_invalid(OrderBook& actual, const ReferenceBook& reference, std::mt19937& rng) {
    const auto side = rng() % 2 ? Side::Buy : Side::Sell;
    bool rejected = false;
    try {
        switch (rng() % 5) {
        case 0: actual.add_order(side, 0, 1); break;
        case 1: actual.add_order(side, 1, 0); break;
        case 2: actual.add_order(side, std::numeric_limits<std::int64_t>::min(), 1); break;
        case 3: actual.add_order(side, 1, -1); break;
        case 4: actual.add_order(static_cast<Side>(77), 1, 1); break;
        }
    } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "invalid input accepted");
    compare(actual.snapshot(full_depth), reference.snapshot(full_depth));
}

struct GeneratedOrder {
    Side side;
    std::int64_t price;
    std::int64_t quantity;
};

GeneratedOrder generate_order(Profile profile, unsigned seed, unsigned step,
                              bool loading, std::mt19937& rng) {
    auto side = rng() % 2 ? Side::Buy : Side::Sell;
    if (profile == Profile::BuyHeavy) side = rng() % 10 ? Side::Buy : Side::Sell;
    if (profile == Profile::SellHeavy) side = rng() % 10 ? Side::Sell : Side::Buy;
    if (loading) side = seed % 2 ? Side::Buy : Side::Sell;
    std::int64_t price = 90 + rng() % 21;
    std::int64_t quantity = 1 + rng() % 30;
    if (profile == Profile::Crowded || profile == Profile::CancelHeavy) price = 100;
    if (profile == Profile::Wide) price = loading ? 1 + step * 1000 : 1 + rng() % 300001;
    if (profile == Profile::Extremes) {
        const std::int64_t prices[] = {1, 2, max_quantity / 2, max_quantity - 1, max_quantity};
        // Keep whole-book totals and conservation arithmetic within int64_t.
        // The deterministic suite separately checks INT64_MAX and overflow.
        const std::int64_t quantities[] = {1, 2, max_quantity / 100000,
                                           max_quantity / 20000, max_quantity / 10000};
        price = prices[rng() % 5];
        quantity = quantities[rng() % 5];
    }
    return {side, price, quantity};
}

int main() {
    constexpr unsigned seeds = 20;
    constexpr unsigned steps = 1000;
    std::size_t operations = 0;
    bool saw_large_total = false;
    bool saw_many_levels = false;
    bool saw_crowded_level = false;
    for (unsigned profile_index = 0; profile_index < 7; ++profile_index) {
        const auto profile = static_cast<Profile>(profile_index);
        for (unsigned seed = 1; seed <= seeds; ++seed) {
            std::mt19937 rng(seed);
            // Raise the admission limit explicitly for boundary stress only.
            OrderBook actual(profile == Profile::Extremes ? max_quantity : 1000000);
            ReferenceBook reference;
            unsigned step = 0;
            std::string operation;
            try {
                for (; step < steps; ++step) {
                    // Profiles accumulate first, then exercise their resting orders.
                    const bool loading = step < 300 &&
                        (profile == Profile::Crowded || profile == Profile::Wide || profile == Profile::Extremes);
                    const unsigned action = loading ? 0 : rng() % 100;
                    const unsigned cancel_threshold = profile == Profile::CancelHeavy ? 40 : 75;
                    auto before = actual.snapshot(full_depth);
                    if (action >= 95) {
                        operation = "invalid input";
                        reject_invalid(actual, reference, rng);
                    } else if (action >= cancel_threshold) {
                        auto id = static_cast<std::uint64_t>(rng() % (reference.next_id + 10));
                        if (!reference.orders.empty() && rng() % 4 != 0) {
                            // Specifically hit queue front, middle and back as well as random positions.
                            auto index = rng() % reference.orders.size();
                            switch (rng() % 4) {
                            case 0: index = 0; break;
                            case 1: index = reference.orders.size() / 2; break;
                            case 2: index = reference.orders.size() - 1; break;
                            }
                            id = reference.orders[index].id;
                        }
                        operation = "cancel " + std::to_string(id);
                        std::int64_t removed = 0;
                        for (auto o : reference.orders) if (o.id == id) removed = o.quantity;
                        require(actual.cancel_order(id) == reference.cancel(id), "cancellation differs");
                        require(resting_quantity(before) == resting_quantity(actual.snapshot(full_depth)) + removed,
                                "cancellation quantity conservation failed");
                        require(!actual.cancel_order(id), "repeated cancellation accepted");
                    } else {
                        const auto [side, price, quantity] =
                            generate_order(profile, seed, step, loading, rng);
                        operation = std::string(side == Side::Buy ? "buy " : "sell ") + std::to_string(price)
                            + " " + std::to_string(quantity);
                        auto a = actual.add_order(side, price, quantity);
                        auto b = reference.add(side, price, quantity);
                        require(a.order_id == b.order_id, "ID differs (possibly consumed by invalid input)");
                        require(a.trades.size() == b.trades.size(), "trade count differs");
                        std::int64_t filled = 0;
                        for (std::size_t i = 0; i < a.trades.size(); ++i) {
                            auto x = a.trades[i]; auto y = b.trades[i];
                            require(x.buy_order_id == y.buy_order_id && x.sell_order_id == y.sell_order_id
                                    && x.price == y.price && x.quantity == y.quantity, "trade differs");
                            require(x.quantity > 0, "non-positive execution");
                            require(side == Side::Buy ? x.price <= price : x.price >= price, "limit violated");
                            filled += x.quantity;
                        }
                        require(filled <= quantity, "incoming order overfilled");
                        require(resting_quantity(before) + quantity ==
                                resting_quantity(actual.snapshot(full_depth)) + 2 * filled,
                                "add quantity conservation failed");
                    }
                    auto full = actual.snapshot(full_depth);
                    compare(full, reference.snapshot(full_depth));
                    invariants(full);
                    const std::size_t depths[] = {0, 1, 2, 5, 100, full_depth};
                    auto depth = depths[rng() % 6];
                    compare(actual.snapshot(depth), reference.snapshot(depth));
                    if (full.bids.size() > 100 || full.asks.size() > 100) saw_many_levels = true;
                    for (auto level : full.bids) if (level.quantity > max_quantity / 1000) saw_large_total = true;
                    for (auto level : full.asks) if (level.quantity > max_quantity / 1000) saw_large_total = true;
                    if (profile == Profile::Crowded && reference.orders.size() >= 300) saw_crowded_level = true;
                    ++operations;
                }
                // Drain in shuffled order and compare after every removal. This
                // catches stale IDs and quantity errors hidden by aggregate depth.
                auto survivors = reference.orders;
                std::shuffle(survivors.begin(), survivors.end(), rng);
                for (auto order : survivors) {
                    operation = "final drain cancel " + std::to_string(order.id);
                    require(actual.cancel_order(order.id), "survivor missing from ID index");
                    require(reference.cancel(order.id), "reference survivor missing");
                    require(!actual.cancel_order(order.id), "duplicate cancellation accepted");
                    compare(actual.snapshot(full_depth), reference.snapshot(full_depth));
                }
                // Even if the last generated action was invalid, detect ID consumption.
                require(actual.add_order(Side::Buy, 1, 1).order_id == reference.next_id, "final ID differs");
            } catch (const std::exception& e) {
                std::cerr << "FAIL profile=" << names[profile_index] << " seed=" << seed
                          << " step=" << step << " operation=" << operation << ": " << e.what() << '\n';
                return 1;
            }
        }
        std::cout << "PASS: " << names[profile_index] << " (" << seeds * steps << " operations)\n";
    }
    if (!saw_large_total || !saw_many_levels || !saw_crowded_level) {
        std::cerr << "FAIL: missing required coverage of large totals, deep books or crowded levels\n";
        return 1;
    }
    std::cout << "PASS: " << operations << " randomized operations across 7 profiles and " << seeds
              << " seeds per profile; boundary coverage and conservation checks passed\n";
}
