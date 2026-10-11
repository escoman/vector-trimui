#ifndef V06X_SERIALIZE_H
#define V06X_SERIALIZE_H

#include <cstdint>
#include <vector>
#include <algorithm>

class SerializeChunk {
public:
    typedef std::vector<uint8_t> stype_t;

    enum id : uint32_t {
        MEMORY = 0xfecacaca,
        IO = 0xfeca1010,
        CPU = 0xfeca0000,
        BOARD = 0xfecababe,
        TIMER = 0xfeca5353,
    };

    /* Fixed-width little-endian scalars, so a save file written on one
     * platform (PSP size_t=4, x86/aarch64 size_t=8, host endianness) reads
     * back identically on another. Used by the timing-related chunks. */
    static void append_i32_le(stype_t & v, int32_t x)
    {
        const uint32_t u = static_cast<uint32_t>(x);
        v.push_back(static_cast<uint8_t>(u & 0xff));
        v.push_back(static_cast<uint8_t>((u >> 8) & 0xff));
        v.push_back(static_cast<uint8_t>((u >> 16) & 0xff));
        v.push_back(static_cast<uint8_t>((u >> 24) & 0xff));
    }

    static void append_u16_le(stype_t & v, uint16_t x)
    {
        v.push_back(static_cast<uint8_t>(x & 0xff));
        v.push_back(static_cast<uint8_t>((x >> 8) & 0xff));
    }

    static void append_u32_le(stype_t & v, uint32_t u)
    {
        v.push_back(static_cast<uint8_t>(u & 0xff));
        v.push_back(static_cast<uint8_t>((u >> 8) & 0xff));
        v.push_back(static_cast<uint8_t>((u >> 16) & 0xff));
        v.push_back(static_cast<uint8_t>((u >> 24) & 0xff));
    }

    static int32_t read_i32_le(stype_t::iterator & it)
    {
        const uint32_t u = static_cast<uint32_t>(*it)
            | (static_cast<uint32_t>(it[1]) << 8)
            | (static_cast<uint32_t>(it[2]) << 16)
            | (static_cast<uint32_t>(it[3]) << 24);
        it += 4;
        return static_cast<int32_t>(u);
    }

    static uint16_t read_u16_le(stype_t::iterator & it)
    {
        const uint16_t u = static_cast<uint16_t>(*it)
            | (static_cast<uint16_t>(it[1]) << 8);
        it += 2;
        return u;
    }

    static uint32_t read_u32_le(stype_t::iterator & it)
    {
        const uint32_t u = static_cast<uint32_t>(*it)
            | (static_cast<uint32_t>(it[1]) << 8)
            | (static_cast<uint32_t>(it[2]) << 16)
            | (static_cast<uint32_t>(it[3]) << 24);
        it += 4;
        return u;
    }

    static void insert_chunk(stype_t &to, id chunk_id, stype_t &chunk)
    {
        uint8_t * pchunk = reinterpret_cast<uint8_t *>(&chunk_id);
        to.insert(to.end(), pchunk, pchunk + 4);

        uint32_t size = chunk.size();
        uint8_t * psize = reinterpret_cast<uint8_t *>(&size);
        to.insert(to.end(), psize, psize + 4);

        to.insert(to.end(), chunk.begin(), chunk.end());
    }

    static stype_t::iterator take_chunk(stype_t::iterator it, id & signature, uint32_t & size)
    {
        uint32_t sig;
        std::copy(it, it + 4, reinterpret_cast<uint8_t *>(&sig));
        signature = static_cast<id>(sig);
        it += 4;

        uint32_t size_;
        std::copy(it, it + 4, reinterpret_cast<uint8_t *>(&size_));
        size = size_;
        return it + 4;
    }
};

#endif
