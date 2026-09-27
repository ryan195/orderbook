#include "exchange.hpp"

#include <charconv>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::size_t default_depth = 5;

// Parse directly into the target type so every command gets the same range checks.
template <typename Integer>
Integer parse_number(const std::string& text) {
    Integer value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || text.front() == '-') {
        throw std::invalid_argument("Expected a non-negative integer within range: " + text);
    }
    return value;
}

std::vector<std::string> parse_words(const std::string& line) {
    std::istringstream input(line.substr(0, line.find('#')));
    std::vector<std::string> words;
    for (std::string word; input >> word;) {
        words.push_back(word);
    }
    return words;
}

void show(const Exchange& exchange, const std::string& symbol, std::size_t levels = default_depth) {
    const auto snapshot = exchange.snapshot(symbol, levels);
    std::cout << "Stock: " << symbol << '\n'
              << "Best bid: " << (snapshot.best_bid ? std::to_string(*snapshot.best_bid) : "none")
              << " | Best ask: " << (snapshot.best_ask ? std::to_string(*snapshot.best_ask) : "none") << '\n';
    const auto print_levels = [](const char* label, const std::vector<PriceLevel>& prices) {
        std::cout << label << " (price / quantity)\n";
        for (const auto& level : prices) {
            std::cout << "  " << level.price << " / " << level.quantity << '\n';
        }
    };
    print_levels("BIDS", snapshot.bids);
    print_levels("ASKS", snapshot.asks);
}

void help() {
    std::cout << "Commands (prices in integer cents):\n"
              << "  stock SYMBOL   (create/select a demo stock)\n  stocks\n"
              << "  buy PRICE QUANTITY [ioc]\n  sell PRICE QUANTITY [ioc]\n  cancel ID\n"
              << "  book [LEVELS]   (default: 5)\n  help\n  quit\n"
              << "Lines may contain # comments. Replay: ./build/orderbook-cli FILE\n";
}

void submit_order(Exchange& exchange, const std::string& symbol, const std::vector<std::string>& words) {
    if (words.size() == 4 && words[3] != "ioc") {
        throw std::invalid_argument("Optional order policy must be ioc");
    }
    const auto policy = words.size() == 4 ? TimeInForce::ImmediateOrCancel : TimeInForce::GoodTillCancelled;
    const auto side = words[0] == "buy" ? Side::Buy : Side::Sell;
    const auto price = parse_number<std::int64_t>(words[1]);
    const auto quantity = parse_number<std::int64_t>(words[2]);
    const auto result = exchange.add_order(symbol, side, price, quantity, policy);
    std::cout << symbol << " accepted order #" << result.order_id << '\n';
    std::int64_t filled = 0;
    for (const auto& trade : result.trades) {
        std::cout << symbol << " TRADE buy #" << trade.buy_order_id << " sell #" << trade.sell_order_id
                  << " @ " << trade.price << " qty " << trade.quantity << '\n';
        filled += trade.quantity;
    }
    if (policy == TimeInForce::ImmediateOrCancel) {
        std::cout << "IOC discarded quantity: " << quantity - filled << '\n';
    }
    show(exchange, symbol);
}

// Return false only for quit. Parsing errors leave the caller's loop in control.
bool execute_command(Exchange& exchange, std::string& symbol, const std::vector<std::string>& words) {
    const auto& command = words.front();
    if ((command == "buy" || command == "sell") && (words.size() == 3 || words.size() == 4)) {
        submit_order(exchange, symbol, words);
    } else if (command == "cancel" && words.size() == 2) {
        const auto id = parse_number<std::uint64_t>(words[1]);
        const bool cancelled = exchange.cancel_order(symbol, id);
        std::cout << (cancelled ? "Cancelled #" : "No resting order #") << id << '\n';
        show(exchange, symbol);
    } else if (command == "book" && words.size() <= 2) {
        const auto levels = words.size() == 2 ? parse_number<std::size_t>(words[1]) : default_depth;
        show(exchange, symbol, levels);
    } else if (command == "stock" && words.size() == 2) {
        exchange.add_stock(words[1]);
        symbol = words[1];
        show(exchange, symbol);
    } else if (command == "stocks" && words.size() == 1) {
        for (const auto& name : exchange.symbols()) {
            std::cout << name << (name == symbol ? " (selected)" : "") << '\n';
        }
    } else if (command == "help" && words.size() == 1) {
        help();
    } else if (command == "quit" && words.size() == 1) {
        return false;
    } else {
        throw std::invalid_argument("Unknown command or wrong arguments; type help");
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "Usage: " << argv[0] << " [REPLAY_FILE]\n";
        return 1;
    }
    const bool replay = argc == 2;
    std::ifstream file;
    if (replay) {
        file.open(argv[1]);
        if (!file) {
            std::cerr << "Cannot open replay file: " << argv[1] << '\n';
            return 1;
        }
    }
    std::istream& input = replay ? file : std::cin;
    Exchange exchange;
    std::string symbol = "DEMO";
    exchange.add_stock(symbol);
    if (!replay) {
        help();
    }

    std::string line;
    std::size_t line_number = 0;
    while (true) {
        if (!replay) {
            std::cout << symbol << "> " << std::flush;
        }
        if (!std::getline(input, line)) {
            break;
        }
        ++line_number;
        const auto words = parse_words(line);
        if (words.empty()) {
            continue;
        }
        if (replay) {
            std::cout << "> " << line.substr(0, line.find('#')) << '\n';
        }
        try {
            if (!execute_command(exchange, symbol, words)) {
                break;
            }
        } catch (const std::invalid_argument& error) {
            std::cerr << "Line " << line_number << ": " << error.what() << '\n';
            if (replay) {
                return 1;
            }
        } catch (const std::overflow_error& error) {
            std::cerr << "Line " << line_number << ": " << error.what() << '\n';
            if (replay) {
                return 1;
            }
        }
    }
    if (input.bad()) {
        std::cerr << "Input read failed\n";
        return 1;
    }
}
