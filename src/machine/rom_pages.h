// Owned Linux ROM mappings with byte-exact, copy-on-write duplicate pages.
#pragma once

#include <cstdint>
#include <span>

#if defined(__linux__)
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/magic.h>
#include <string>
#include <sys/mman.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <zlib.h>
#endif

class rom_page_mapping {
public:
    rom_page_mapping() = default;
    rom_page_mapping(const rom_page_mapping &) = delete;
    rom_page_mapping &operator=(const rom_page_mapping &) = delete;

    ~rom_page_mapping()
    {
#if defined(__linux__)
        if (!m_bytes.empty()) munmap(m_bytes.data(), m_bytes.size());
#endif
    }

    // Build a separate mapping before releasing the source allocation. If RAM
    // backing, writes or mappings fail, the original bytes remain usable.
    size_t compact(std::span<const uint8_t> source)
    {
#if defined(__linux__)
        const long system_page = sysconf(_SC_PAGESIZE);
        if (system_page <= 0 || !m_bytes.empty()) return 0;
        const size_t page = size_t(system_page);
        if (source.size() < page * 2 || source.size() % page) return 0;
        const size_t count = source.size() / page;
        const size_t empty = count;
        const std::vector<uint8_t> zero(page);
        std::vector<size_t> offsets(count, empty);
        std::vector<size_t> unique;
        std::unordered_multimap<uint32_t, size_t> groups;
        size_t duplicates = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto *bytes = source.data() + i * page;
            if (std::memcmp(bytes, zero.data(), page) == 0) continue;
            const auto hash = uint32_t(crc32(0, bytes, uInt(page)));
            const auto [first, last] = groups.equal_range(hash);
            auto match = first;
            while (match != last && std::memcmp(bytes, source.data() + unique[match->second] * page, page)) ++match;
            if (match == last) {
                offsets[i] = unique.size();
                groups.emplace(hash, offsets[i]);
                unique.push_back(i);
            } else {
                offsets[i] = match->second;
                ++duplicates;
            }
        }
        if (!duplicates) return 0;

        const int fd = backing_file();
        if (fd < 0) return 0;
        struct close_file {
            int fd;
            ~close_file() { close(fd); }
        } file{fd};
        for (size_t i = 0; i < unique.size();) {
            size_t end = i + 1;
            while (end < unique.size() && unique[end] == unique[end - 1] + 1) ++end;
            if (!write_all(fd, source.data() + unique[i] * page, (end - i) * page, off_t(i * page))) return 0;
            i = end;
        }

        constexpr int protection = PROT_READ | PROT_WRITE;
        void *base = mmap(nullptr, source.size(), protection, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) return 0;
        auto *bytes = static_cast<uint8_t *>(base);
        for (size_t i = 0; i < count;) {
            if (offsets[i] == empty) {
                ++i;
                continue;
            }
            size_t end = i + 1;
            while (end < count && offsets[end] != empty && offsets[end] == offsets[end - 1] + 1) ++end;
            // A write to VF2's main-data ROM patches only its own virtual page.
            if (mmap(bytes + i * page, (end - i) * page, protection,
                     MAP_PRIVATE | MAP_FIXED, fd, off_t(offsets[i] * page)) == MAP_FAILED) {
                munmap(base, source.size());
                return 0;
            }
            i = end;
        }
        m_bytes = {bytes, source.size()};
        return duplicates * page;
#else
        (void)source;
        return 0;
#endif
    }

    std::span<uint8_t> bytes() const { return m_bytes; }

private:
    std::span<uint8_t> m_bytes;
#if defined(__linux__)
    static int backing_file()
    {
        const char *dir = std::getenv("VF2_ROM_DEDUP_DIR");
#if defined(SYS_memfd_create)
        // MFD_CLOEXEC. The Mini's 3.4 kernel falls through to its RAM filesystem.
        if (!dir) {
            const int fd = int(syscall(SYS_memfd_create, "vf2-rom", 1u));
            if (fd >= 0) return fd;
        }
#endif
        std::string path = std::string(dir ? dir : "/run/vf2") + "/.romshare.XXXXXX";
        const int fd = mkstemp(path.data());
        if (fd < 0) return -1;
        struct statfs fs{};
        if (unlink(path.c_str()) != 0 || fstatfs(fd, &fs) != 0
            || (fs.f_type != TMPFS_MAGIC && fs.f_type != RAMFS_MAGIC)) {
            close(fd);
            return -1;
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        return fd;
    }

    static bool write_all(int fd, const uint8_t *bytes, size_t size, off_t offset)
    {
        while (size) {
            const ssize_t n = pwrite(fd, bytes, size, offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            bytes += n;
            size -= size_t(n);
            offset += n;
        }
        return true;
    }
#endif
};
