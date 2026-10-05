// SPDX-License-Identifier: MIT
//
// itch_filter -- slice a full Nasdaq ITCH 5.0 day down to a replayable corpus.
//
// A day file is ~3.5 GB gzipped and 8.25 GB on the wire, which is more than a
// benchmark wants to page through on every run. This tool streams the gzip,
// keeps only the messages belonging to the requested symbols, and writes them
// back out in the same BinaryFILE framing so the result is still a valid ITCH
// stream that the replay driver can mmap.
//
// The catch that makes this non-trivial: only Add Order messages carry a stock
// symbol. Execute, Cancel, Delete and Replace name an order reference number
// and nothing else. So the filter has to track which references belong to the
// symbols we want, and follow Replace as it renumbers an order.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "itch/reader.hpp"

namespace {

struct Options {
    std::string              input;
    std::string              outdir = ".";
    std::vector<std::string> symbols;
    std::uint64_t            max_messages = 0;  // 0 = whole file per symbol
    std::uint64_t            stop_after_ns = 0; // 0 = whole session
};

void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --in <itch.gz|itch> --out-dir <dir> --symbol SYM [--symbol SYM ...]\n"
        "          [--max-messages N] [--until-seconds S]\n\n"
        "  --in             Nasdaq ITCH 5.0 file, gzipped or raw\n"
        "  --out-dir        directory for the output; one <SYMBOL>.itch per symbol\n"
        "  --symbol         symbol to keep; repeat for several\n"
        "  --max-messages   stop a symbol after N output messages\n"
        "  --until-seconds  stop at this many seconds after midnight (e.g. 37800 = 10:30)\n\n"
        "Each symbol gets its own file because an order book holds one instrument:\n"
        "replaying two symbols into one book would interleave unrelated price levels.\n"
        "A day file is scanned once no matter how many symbols are requested.\n",
        argv0);
}

class Writer {
public:
    explicit Writer(const std::string& path) {
        f_ = std::fopen(path.c_str(), "wb");
        if (!f_) { std::perror("fopen"); std::exit(1); }
        buf_.reserve(kFlush + 1024);
    }
    ~Writer() { flush(); if (f_) std::fclose(f_); }

    void write(const std::byte* msg, std::uint16_t len) {
        const std::uint16_t be = itch::bswap(len);
        const auto* p = reinterpret_cast<const std::byte*>(&be);
        buf_.insert(buf_.end(), p, p + 2);
        buf_.insert(buf_.end(), msg, msg + len);
        ++count_;
        if (buf_.size() >= kFlush) flush();
    }

    void flush() {
        if (f_ && !buf_.empty()) {
            std::fwrite(buf_.data(), 1, buf_.size(), f_);
            buf_.clear();
        }
    }

    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }

private:
    static constexpr std::size_t kFlush = 1u << 20;
    std::FILE*             f_ = nullptr;
    std::vector<std::byte> buf_;
    std::uint64_t          count_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); std::exit(2); }
            return argv[++i];
        };
        if      (a == "--in")            opt.input  = next("--in");
        else if (a == "--out-dir")       opt.outdir = next("--out-dir");
        else if (a == "--symbol")        opt.symbols.push_back(next("--symbol"));
        else if (a == "--max-messages")  opt.max_messages = std::strtoull(next("--max-messages").c_str(), nullptr, 10);
        else if (a == "--until-seconds") opt.stop_after_ns = std::strtoull(next("--until-seconds").c_str(), nullptr, 10) * 1'000'000'000ull;
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); usage(argv[0]); return 2; }
    }
    if (opt.input.empty() || opt.symbols.empty()) { usage(argv[0]); return 2; }

    std::unordered_set<std::string> wanted(opt.symbols.begin(), opt.symbols.end());

    // stock_locate is a dense per-symbol id assigned in the Stock Directory. It
    // is on every message that carries a symbol, so resolving symbol -> locate
    // once turns the per-message symbol test into an integer compare.
    std::unordered_map<std::uint16_t, Writer*> by_locate;
    std::unordered_map<std::string, std::unique_ptr<Writer>> writers;

    // Order reference number -> the writer that owns it. Execute, Cancel,
    // Delete and Replace carry no symbol, so this is the only way to route
    // them once an Add has introduced the reference.
    std::unordered_map<std::uint64_t, Writer*> refs;
    refs.reserve(1u << 20);

    // Session-level messages are copied into every symbol's file so each one
    // stands alone as a valid stream.
    std::vector<Writer*> all;

    itch::ParseStats stats;
    std::uint64_t last_ts = 0;

    auto writer_for = [&](std::uint16_t locate) -> Writer* {
        const auto it = by_locate.find(locate);
        return it == by_locate.end() ? nullptr : it->second;
    };

    auto handle = [&](itch::Header h, std::uint16_t len) {
        const char t = h.type();
        if (h.timestamp()) last_ts = h.timestamp();
        if (opt.stop_after_ns && h.timestamp() > opt.stop_after_ns) return;

        switch (t) {
            // Session framing: tiny, and each output should be self-contained.
            case 'S':
                for (Writer* w : all) w->write(h.p, len);
                return;

            case 'R': {
                itch::StockDirectory d{h.p};
                const std::string sym(d.stock());
                if (!wanted.count(sym) || writers.count(sym)) return;
                auto w = std::make_unique<Writer>(opt.outdir + "/" + sym + ".itch");
                by_locate[h.stock_locate()] = w.get();
                all.push_back(w.get());
                w->write(h.p, len);
                writers.emplace(sym, std::move(w));
                return;
            }

            case 'H': case 'Y': case 'L': case 'K': case 'J': case 'h': case 'N':
            case 'Q': case 'I': case 'P':
                if (Writer* w = writer_for(h.stock_locate())) w->write(h.p, len);
                return;

            // Adds carry the symbol, so this is where a reference is routed.
            case 'A': case 'F': {
                Writer* w = writer_for(h.stock_locate());
                if (!w || (opt.max_messages && w->count() >= opt.max_messages)) return;
                refs.emplace(itch::AddOrder{h.p}.order_ref(), w);
                w->write(h.p, len);
                return;
            }

            case 'E': case 'C': {
                const auto it = refs.find(itch::OrderExecuted{h.p}.order_ref());
                if (it != refs.end()) it->second->write(h.p, len);
                return;
            }

            case 'X': {
                const auto it = refs.find(itch::OrderCancel{h.p}.order_ref());
                if (it != refs.end()) it->second->write(h.p, len);
                return;
            }

            case 'D': {
                // Emit before forgetting the reference: the delete is in scope.
                const auto it = refs.find(itch::OrderDelete{h.p}.order_ref());
                if (it == refs.end()) return;
                it->second->write(h.p, len);
                refs.erase(it);
                return;
            }

            case 'U': {
                itch::OrderReplace r{h.p};
                const auto it = refs.find(r.original_ref());
                if (it == refs.end()) return;
                Writer* w = it->second;
                w->write(h.p, len);
                refs.erase(it);
                refs.emplace(r.new_ref(), w);  // the order keeps trading under a new reference
                return;
            }

            default:
                return;  // everything else is market-wide noise for our purpose
        }
    };

    const bool gz = opt.input.size() > 3 && opt.input.compare(opt.input.size() - 3, 3, ".gz") == 0;
    if (gz) {
        itch::GzMessageStream in(opt.input);
        in.run(stats, handle);
    } else {
        itch::MappedFile in(opt.input);
        itch::for_each_message(in.bytes(), stats, handle);
    }

    std::fprintf(stderr,
        "read %llu messages (%.2f GB), last timestamp %.3f s after midnight\n",
        (unsigned long long)stats.messages, (double)stats.bytes / 1e9,
        (double)last_ts / 1e9);
    for (auto& [sym, w] : writers) {
        w->flush();
        std::fprintf(stderr, "  %-8s %10llu messages -> %s/%s.itch\n",
                     sym.c_str(), (unsigned long long)w->count(), opt.outdir.c_str(), sym.c_str());
    }
    for (const auto& s : wanted)
        if (!writers.count(s))
            std::fprintf(stderr, "  warning: %s never appeared in the Stock Directory\n", s.c_str());
    return 0;
}
