#include "statefile.h"
#include "util.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>

#include <cstdio>
#include <cstring>

/*
 * SAVE/LOAD STATE storage, see statefile.h. The state file layout:
 *
 *   [0..3]   magic "V06S"
 *   [4..7]   STATE_VERSION, little-endian u32
 *   [8..15]  save timestamp (unix seconds), little-endian u64
 *   [16..]   the unchanged Board::serialize() payload
 *
 * Overwriting goes through a temp file first: a failed write never
 * destroys the previous state.
 */

namespace StateFile
{
    static const char MAGIC[4] = { 'V', '0', '6', 'S' };

    /* The directory of the running executable, resolved once.
     * launch.sh cd's there anyway, but the exe link is immune to a
     * different working directory. Also the anchor of the CrossMix
     * SD layout (<sdcard>/Emus/VECTOR06C), so FileList derives the
     * standard ROM folder from it. */
    const std::string & emu_dir()
    {
        static std::string dir = [] {
            char exe[PATH_MAX];
            const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
            std::string d = ".";
            if (n > 0) {
                exe[n] = '\0';
                const std::string path(exe);
                const size_t slash = path.find_last_of('/');
                if (slash != std::string::npos)
                    d = path.substr(0, slash);
            }
            return d;
        }();
        return dir;
    }

    /* <emu_dir>/SAVES */
    const std::string & saves_dir()
    {
        static std::string dir = emu_dir() + "/SAVES";
        return dir;
    }

    static void put32(std::vector<uint8_t> & v, uint32_t x)
    {
        v.push_back((uint8_t)(x & 0xff));
        v.push_back((uint8_t)((x >> 8) & 0xff));
        v.push_back((uint8_t)((x >> 16) & 0xff));
        v.push_back((uint8_t)((x >> 24) & 0xff));
    }

    static void put64(std::vector<uint8_t> & v, uint64_t x)
    {
        put32(v, (uint32_t)(x & 0xffffffffu));
        put32(v, (uint32_t)(x >> 32));
    }

    static uint32_t get32(const uint8_t * p)
    {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
             | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    static uint64_t get64(const uint8_t * p)
    {
        return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
    }

    std::string rom_dir(const std::string & rom_base)
    {
        return saves_dir() + "/" + rom_base;
    }

    std::string bin_path(const std::string & dir, int slot)
    {
        char name[32];
        snprintf(name, sizeof(name), "/state%d.bin", slot);
        return dir + name;
    }

    std::string shot_path(const std::string & dir, int slot)
    {
        char name[32];
        snprintf(name, sizeof(name), "/state%d.png", slot);
        return dir + name;
    }

    /* mkdir fails when the directory already exists, so the stat
     * check decides between "already there" and a real error. */
    static bool mkdir_ok(const char * path)
    {
        if (mkdir(path, 0777) == 0)
            return true;

        struct stat st;
        memset(&st, 0, sizeof(st));
        return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    }

    bool ensure_dir(const std::string & dir)
    {
        if (!mkdir_ok(saves_dir().c_str()))
            return false;
        return mkdir_ok(dir.c_str());
    }

    bool exists(const std::string & path)
    {
        struct stat st;
        memset(&st, 0, sizeof(st));
        return stat(path.c_str(), &st) == 0;
    }

    bool read_header(const std::string & bin_path, uint64_t & out_ts)
    {
        FILE * f = std::fopen(bin_path.c_str(), "rb");
        if (f == nullptr)
            return false;

        uint8_t hdr[HEADER_SIZE];
        const size_t got = std::fread(hdr, 1, sizeof(hdr), f);
        std::fclose(f);

        if (got != sizeof(hdr)
                || memcmp(hdr, MAGIC, sizeof(MAGIC)) != 0
                || get32(hdr + 4) != (uint32_t)STATE_VERSION) {
            return false;
        }
        out_ts = get64(hdr + 8);
        return true;
    }

    bool save(const std::string & dir, int slot,
              const std::vector<uint8_t> & payload, uint64_t timestamp)
    {
        std::vector<uint8_t> buf;
        buf.reserve(HEADER_SIZE + payload.size());
        buf.insert(buf.end(), MAGIC, MAGIC + sizeof(MAGIC));
        put32(buf, (uint32_t)STATE_VERSION);
        put64(buf, timestamp);
        buf.insert(buf.end(), payload.begin(), payload.end());

        const std::string dst = bin_path(dir, slot);
        const std::string tmp = dst + ".tmp";

        const int written = util::save_binfile(tmp, buf);
        if (written != (int)buf.size()) {
            std::remove(tmp.c_str());
            printf("StateFile: write failed: %s (%d of %lu)\n",
                   tmp.c_str(), written, (unsigned long)buf.size());
            return false;
        }

        /* Replace the old state only with a complete new one. */
        if (util::careful_rename(tmp, dst) != 0) {
            std::remove(tmp.c_str());
            printf("StateFile: rename failed: %s -> %s\n",
                   tmp.c_str(), dst.c_str());
            return false;
        }

        printf("StateFile: saved %s (%lu bytes)\n",
               dst.c_str(), (unsigned long)buf.size());
        return true;
    }

    bool load(const std::string & dir, int slot,
              std::vector<uint8_t> & out_payload, uint64_t & out_ts)
    {
        const std::string path = bin_path(dir, slot);
        std::vector<uint8_t> data = util::load_binfile(path);
        if (data.size() < HEADER_SIZE
                || memcmp(data.data(), MAGIC, sizeof(MAGIC)) != 0) {
            printf("StateFile: bad magic / truncated: %s\n", path.c_str());
            return false;
        }
        if (get32(data.data() + 4) != (uint32_t)STATE_VERSION) {
            printf("StateFile: unknown version %lu: %s\n",
                   (unsigned long)get32(data.data() + 4), path.c_str());
            return false;
        }

        out_ts = get64(data.data() + 8);
        out_payload.assign(data.begin() + HEADER_SIZE, data.end());
        return true;
    }
}
