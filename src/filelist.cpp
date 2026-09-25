#include <dirent.h>
#include <sys/stat.h>

#include <cstring>
#include <cstdio>
#include <cctype>
#include <algorithm>

#include "filelist.h"
#include "statefile.h"

/*
 * POSIX port of the PSP ROM listing helper: sceIoDopen/Dread become
 * opendir/readdir, the directory-bit check goes through d_type with
 * a stat() fallback for file systems that report DT_UNKNOWN.
 */

namespace FileList
{
    /* Extensions treated as Vector-06C ROMs; everything else
     * (.gif/.png/.bmp/.txt/.ini, ...) stays out of the list. */
    static const char * const ROM_EXTENSIONS[] = {
        ".rom",
        ".bin",
        ".r0m",
    };

    bool hasRomExtension(const std::string &name)
    {
        /* Accept .rom, .ROM, .bin, .BIN, ... */
        size_t dot = name.rfind('.');
        if (dot == std::string::npos)
            return false;

        std::string ext = name.substr(dot);
        for (size_t i = 0; i < ext.size(); ++i)
            ext[i] = (char)tolower(ext[i]);

        /* std::string compares against const char * directly. */
        for (const char * rom_ext : ROM_EXTENSIONS)
            if (ext == rom_ext)
                return true;
        return false;
    }

    /* Alphabetical, case-insensitive, stable. */
    static bool ci_less(const std::string &a, const std::string &b)
    {
        return std::lexicographical_compare(
            a.begin(), a.end(), b.begin(), b.end(),
            [](char ca, char cb) {
                return tolower((unsigned char)ca)
                     < tolower((unsigned char)cb);
            });
    }

    /* True when the dirent names a directory; d_type first, stat()
     * when the file system leaves it unknown. */
    static bool entry_is_dir(const char * dir_path, const struct dirent * entry)
    {
#ifdef DT_DIR
        if (entry->d_type == DT_DIR)
            return true;
        if (entry->d_type != DT_UNKNOWN)
            return false;
#endif
        const std::string path = std::string(dir_path) + "/" + entry->d_name;
        struct stat st;
        return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }

    bool listRoms(const std::string &dir, std::vector<std::string> &files)
    {
        files.clear();

        DIR * d = opendir(dir.c_str());
        if (d == nullptr)
            return false;

        struct dirent * entry;
        while ((entry = readdir(d)) != nullptr)
        {
            if (entry->d_name[0] == '.')
                continue;
            if (entry_is_dir(dir.c_str(), entry))
                continue;
            std::string name(entry->d_name);
            if (hasRomExtension(name))
                files.push_back(name);
        }

        closedir(d);

        std::stable_sort(files.begin(), files.end(), ci_less);
        return true;
    }

    /* Case-insensitive whole-name compare. */
    static bool ci_equal(const std::string &a, const std::string &b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
                return false;
        return true;
    }

    bool findPreview(const std::string &dir, const std::string &rom_name,
                     std::string &out_path)
    {
        /* Base name: everything before the last dot (the ROM
         * extension is never part of the preview name). */
        const size_t dot = rom_name.rfind('.');
        if (dot == std::string::npos || dot == 0)
            return false;
        const std::string base = rom_name.substr(0, dot);

        DIR * d = opendir(dir.c_str());
        if (d == nullptr)
            return false;

        struct dirent * entry;
        while ((entry = readdir(d)) != nullptr)
        {
            if (entry->d_name[0] == '.')
                continue;
            if (entry_is_dir(dir.c_str(), entry))
                continue;

            std::string name(entry->d_name);
            const size_t ndot = name.rfind('.');
            if (ndot == std::string::npos || ndot == 0)
                continue;
            /* base + ".png", any case on both parts */
            if (ci_equal(name.substr(0, ndot), base)
                    && ci_equal(name.substr(ndot), ".png")) {
                closedir(d);
                out_path = dir + "/" + name;
                return true;
            }
        }

        closedir(d);
        return false;
    }

    const std::string & default_rom_dir()
    {
        /* <sdcard>/Roms/VECTOR06C: two levels up from the emulator
         * directory (<sdcard>/Emus/VECTOR06C). Path normalization is
         * left to the OS; FAT/exFAT accept the ".." components. */
        static std::string dir =
            StateFile::emu_dir() + "/../../Roms/VECTOR06C";
        return dir;
    }
}
