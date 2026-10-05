// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <zlib.h>

#include "itch/itch50.hpp"

namespace itch {

// ---------------------------------------------------------------------------
// Nasdaq distributes ITCH as "BinaryFILE": a bare concatenation of
//
//     [uint16 big-endian length][length bytes of message]
//
// with no session layer, no sequence numbers and no end marker. Framing is
// therefore length-driven, which is what lets an unknown future message type be
// skipped without desynchronising the stream.
// ---------------------------------------------------------------------------

struct ParseStats {
    std::uint64_t messages   = 0;
    std::uint64_t bytes      = 0;
    std::uint64_t unknown    = 0;  // types this build does not model
    std::uint64_t truncated  = 0;  // trailing partial frame
    std::uint64_t by_type[128] = {};
};

// Walk a contiguous buffer of BinaryFILE frames, invoking `fn(Header)` per
// message. Returns the number of bytes consumed, so a streaming caller can
// carry the remainder into the next block.
template <class Fn>
std::size_t for_each_message(std::span<const std::byte> buf, ParseStats& stats, Fn&& fn) {
    std::size_t off = 0;
    while (off + 2 <= buf.size()) {
        const std::uint16_t len = be16(buf.data() + off);
        if (len == 0) { ++stats.truncated; break; }          // corrupt framing
        if (off + 2 + len > buf.size()) break;                // partial frame

        const std::byte* msg = buf.data() + off + 2;
        const char type = static_cast<char>(msg[0]);

        // Cross-check the framing length against the spec length. A mismatch
        // means either a spec revision or a corrupt file; we trust the frame
        // (it is what advances the cursor) but count the discrepancy.
        if (const std::size_t want = declared_length(type); want == 0) {
            ++stats.unknown;
        }

        fn(Header{msg}, len);

        ++stats.messages;
        if (static_cast<unsigned char>(type) < 128) ++stats.by_type[static_cast<unsigned char>(type)];
        stats.bytes += len + 2u;
        off += 2u + len;
    }
    return off;
}

// ---------------------------------------------------------------------------
// A read-only memory map of a decompressed ITCH file. Preferred for replay:
// the kernel pages the file in on demand, there is no copy into user space, and
// a second run reads from the page cache.
// ---------------------------------------------------------------------------
class MappedFile {
public:
    explicit MappedFile(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) throw std::runtime_error("cannot open " + path);

        struct stat st{};
        if (::fstat(fd_, &st) != 0) { ::close(fd_); throw std::runtime_error("cannot stat " + path); }
        size_ = static_cast<std::size_t>(st.st_size);

        if (size_ > 0) {
            void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
            if (p == MAP_FAILED) { ::close(fd_); throw std::runtime_error("cannot mmap " + path); }
            data_ = static_cast<const std::byte*>(p);
            // We stream strictly forward, so tell the kernel to read ahead and
            // not to retain pages we have already passed.
            ::madvise(const_cast<void*>(p), size_, MADV_SEQUENTIAL);
        }
    }

    ~MappedFile() {
        if (data_) ::munmap(const_cast<std::byte*>(data_), size_);
        if (fd_ >= 0) ::close(fd_);
    }

    MappedFile(const MappedFile&)            = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {data_, size_}; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    int              fd_   = -1;
    const std::byte* data_ = nullptr;
    std::size_t      size_ = 0;
};

// ---------------------------------------------------------------------------
// Streaming gzip reader. Nasdaq ships ~3.5 GB gzipped that expands past 12 GB,
// so the filter tool decompresses on the fly and never materialises the whole
// day on disk. Frames that straddle a block boundary are carried forward in
// `carry_`.
// ---------------------------------------------------------------------------
class GzMessageStream {
public:
    explicit GzMessageStream(const std::string& path, std::size_t block = 1u << 22)
        : block_(block) {
        gz_ = ::gzopen(path.c_str(), "rb");
        if (!gz_) throw std::runtime_error("cannot gzopen " + path);
        ::gzbuffer(gz_, 1u << 20);
        buf_.resize(block_);
    }

    ~GzMessageStream() { if (gz_) ::gzclose(gz_); }

    GzMessageStream(const GzMessageStream&)            = delete;
    GzMessageStream& operator=(const GzMessageStream&) = delete;

    // Pumps the whole stream, invoking fn(Header, frame_len) per message.
    // Returns false if the stream ended on a partial frame.
    template <class Fn>
    bool run(ParseStats& stats, Fn&& fn) {
        std::vector<std::byte> work;
        for (;;) {
            const int n = ::gzread(gz_, buf_.data(), static_cast<unsigned>(buf_.size()));
            if (n < 0) throw std::runtime_error("gzread failed");
            if (n == 0) break;

            std::span<const std::byte> view;
            if (carry_.empty()) {
                view = std::span<const std::byte>(buf_.data(), static_cast<std::size_t>(n));
            } else {
                work.clear();
                work.reserve(carry_.size() + static_cast<std::size_t>(n));
                work.insert(work.end(), carry_.begin(), carry_.end());
                work.insert(work.end(), buf_.begin(), buf_.begin() + n);
                view = std::span<const std::byte>(work.data(), work.size());
            }

            const std::size_t used = for_each_message(view, stats, fn);
            carry_.assign(view.begin() + static_cast<std::ptrdiff_t>(used), view.end());
        }
        return carry_.empty();
    }

private:
    gzFile                 gz_ = nullptr;
    std::size_t            block_;
    std::vector<std::byte> buf_;
    std::vector<std::byte> carry_;
};

}  // namespace itch
