#include "ChdReader.hpp"

#include "rc_hash.h"

#include <libchdr/chd.h>

#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace Achievements {

namespace {

// CD frames in a CHD: 2352 bytes of sector and 96 of subcode; each track's frames padded to a multiple of 4.
constexpr uint32_t kFrameBytes = 2448, kTrackPadding = 4;

struct ChdTrack {
    chd_file* chd = nullptr;
    uint32_t firstFrame = 0;  // the track's first data frame (after a stored pregap), counted in the file
    uint32_t lba = 0;         // that frame's sector number on the disc, the way rcheevos and the disc count them
    uint32_t frames = 0;      // data frames of the track
    uint32_t unitBytes = kFrameBytes;
    uint32_t headerBytes = 0; // what comes before the user data in each frame
    uint32_t dataBytes = 2048;
    uint32_t hunkFrames = 1;
    std::vector<unsigned char> hunk;
    int64_t hunkIndex = -1;
};

// Either one of ours (a .chd) or one of rcheevos' own reader.
struct Handle {
    ChdTrack* chd = nullptr;
    void* other = nullptr;
};

rc_hash_cdreader g_default{};

bool EndsWithChd(const char* path) {
    const size_t size = path != nullptr ? std::strlen(path) : 0;
    return size > 4 && _stricmp(path + size - 4, ".chd") == 0;
}

std::wstring Wide(const char* text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(size > 0 ? size : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), size);
    out.resize(wcslen(out.c_str()));
    return out;
}

struct TrackInfo {
    int number = 0;
    std::string type;
    uint32_t frames = 0, pregap = 0, postgap = 0, fileStart = 0, lba = 0;
    bool pregapStored = false;
    bool audio() const { return type == "AUDIO"; }
};

// Every track of a CD or GD-ROM image, with where each starts in the file.
std::vector<TrackInfo> Tracks(chd_file* chd, bool& gdrom) {
    std::vector<TrackInfo> tracks;
    gdrom = false;
    uint32_t fileFrame = 0, discSector = 0;
    for (uint32_t index = 0;; ++index) {
        char meta[256] = {};
        uint32_t length = 0, tag = 0;
        uint8_t flags = 0;
        TrackInfo track;
        char type[32] = {}, subtype[32] = {}, pgtype[32] = {}, pgsub[32] = {};
        int number = 0, frames = 0, pregap = 0, postgap = 0, pad = 0;
        if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, index, meta, sizeof(meta) - 1, &length, &tag, &flags) == CHDERR_NONE) {
            std::sscanf(meta, CDROM_TRACK_METADATA2_FORMAT, &number, type, subtype, &frames, &pregap, pgtype, pgsub, &postgap);
        } else if (chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, index, meta, sizeof(meta) - 1, &length, &tag, &flags) == CHDERR_NONE) {
            std::sscanf(meta, CDROM_TRACK_METADATA_FORMAT, &number, type, subtype, &frames);
        } else if (chd_get_metadata(chd, GDROM_TRACK_METADATA_TAG, index, meta, sizeof(meta) - 1, &length, &tag, &flags) == CHDERR_NONE) {
            std::sscanf(meta, GDROM_TRACK_METADATA_FORMAT, &number, type, subtype, &frames, &pad, &pregap, pgtype, pgsub, &postgap);
            gdrom = true;
        } else {
            break;
        }
        track.number = number;
        track.type = type;
        track.frames = static_cast<uint32_t>(frames > 0 ? frames : 0);
        track.pregap = static_cast<uint32_t>(pregap > 0 ? pregap : 0);
        // A pregap of type "V..." is stored in the file, ahead of the track's data.
        track.pregapStored = pgtype[0] == 'V';
        track.postgap = static_cast<uint32_t>(postgap > 0 ? postgap : 0);
        track.fileStart = fileFrame;
        // Where its data starts on the disc: the first track at 0, each next one after the one before and its own
        // pregap, and a GD-ROM's high-density area (its third track) at 45000.
        const uint32_t stored = track.pregapStored ? track.pregap : 0;
        if (tracks.empty()) track.lba = 0;
        else if (gdrom && number == 3) track.lba = 45000;
        else track.lba = discSector + track.pregap;
        discSector = track.lba + (track.frames > stored ? track.frames - stored : 0) + track.postgap;
        fileFrame += (track.frames + kTrackPadding - 1) / kTrackPadding * kTrackPadding;
        tracks.push_back(track);
    }
    return tracks;
}

void Layout(ChdTrack& track, const std::string& type) {
    // Where the 2048 bytes of user data sit in each 2352-byte frame, as rcheevos' own reader takes them.
    if (type == "MODE1_RAW" || type == "MODE1/2352") track.headerBytes = 16, track.dataBytes = 2048;
    else if (type == "MODE2_RAW" || type == "MODE2/2352" || type == "CDI/2352") track.headerBytes = 24, track.dataBytes = 2048;
    else if (type == "MODE2_FORM1" || type == "MODE1" || type == "MODE1/2048") track.headerBytes = 0, track.dataBytes = 2048;
    else if (type == "MODE2" || type == "MODE2/2336") track.headerBytes = 8, track.dataBytes = 2048;
    else track.headerBytes = 0, track.dataBytes = 2352; // audio
}

void* RC_CCONV OpenChd(const char* path, uint32_t wanted) {
    FILE* file = _wfopen(Wide(path).c_str(), L"rb");
    if (file == nullptr) return nullptr;
    chd_file* chd = nullptr;
    if (chd_open_file(file, CHD_OPEN_READ, nullptr, &chd) != CHDERR_NONE) {
        std::fclose(file);
        return nullptr;
    }
    const chd_header* header = chd_get_header(chd);
    auto* track = new ChdTrack;
    track->chd = chd;
    track->unitBytes = header->unitbytes != 0 ? header->unitbytes : kFrameBytes;
    track->hunkFrames = header->hunkbytes / track->unitBytes;
    track->hunk.resize(header->hunkbytes);
    // A DVD (PS2, PSP) is plain 2048-byte sectors and one "track".
    char meta[64] = {};
    uint32_t length = 0, tag = 0;
    uint8_t flags = 0;
    if (chd_get_metadata(chd, DVD_METADATA_TAG, 0, meta, sizeof(meta), &length, &tag, &flags) == CHDERR_NONE) {
        track->firstFrame = 0;
        track->frames = static_cast<uint32_t>(header->logicalbytes / track->unitBytes);
        track->headerBytes = 0;
        track->dataBytes = 2048;
        return track;
    }
    bool gdrom = false;
    const std::vector<TrackInfo> tracks = Tracks(chd, gdrom);
    const TrackInfo* chosen = nullptr;
    if (wanted == RC_HASH_CDTRACK_FIRST_DATA) {
        for (const auto& t : tracks) if (!t.audio()) { chosen = &t; break; }
    } else if (wanted == RC_HASH_CDTRACK_LAST) {
        if (!tracks.empty()) chosen = &tracks.back();
    } else if (wanted == RC_HASH_CDTRACK_LARGEST) {
        for (const auto& t : tracks) if (chosen == nullptr || t.frames > chosen->frames) chosen = &t;
    } else if (wanted == RC_HASH_CDTRACK_FIRST_OF_SECOND_SESSION) {
        // A GD-ROM's high-density area starts at its third track; on a CD the second session is not kept apart.
        for (const auto& t : tracks) if (gdrom ? t.number == 3 : t.number == 2) { chosen = &t; break; }
    } else {
        for (const auto& t : tracks) if (static_cast<uint32_t>(t.number) == wanted) { chosen = &t; break; }
    }
    if (chosen == nullptr || track->hunkFrames == 0) {
        chd_close(chd);
        delete track;
        return nullptr;
    }
    const uint32_t stored = chosen->pregapStored ? chosen->pregap : 0;
    track->firstFrame = chosen->fileStart + stored;
    track->lba = chosen->lba;
    track->frames = chosen->frames > stored ? chosen->frames - stored : 0;
    Layout(*track, chosen->type);
    return track;
}

const unsigned char* Frame(ChdTrack& track, uint32_t frame) {
    const uint32_t hunk = frame / track.hunkFrames;
    if (static_cast<int64_t>(hunk) != track.hunkIndex) {
        if (chd_read(track.chd, hunk, track.hunk.data()) != CHDERR_NONE) return nullptr;
        track.hunkIndex = hunk;
    }
    return track.hunk.data() + static_cast<size_t>(frame % track.hunkFrames) * track.unitBytes;
}

// Sectors are the disc's own numbers (the track starts at first_track_sector), as rcheevos and the disc's directories
// count them.
size_t ReadChd(ChdTrack& track, uint32_t sector, void* buffer, size_t requested) {
    auto* out = static_cast<unsigned char*>(buffer);
    size_t total = 0;
    if (sector < track.lba) return 0;
    sector -= track.lba;
    while (requested > 0 && sector < track.frames) {
        const unsigned char* frame = Frame(track, track.firstFrame + sector);
        if (frame == nullptr) break;
        const size_t take = requested < track.dataBytes ? requested : track.dataBytes;
        std::memcpy(out + total, frame + track.headerBytes, take);
        total += take;
        requested -= take;
        ++sector;
    }
    return total;
}

void* RC_CCONV OpenTrackIterator(const char* path, uint32_t track, const rc_hash_iterator* iterator) {
    void* opened = nullptr;
    const bool chd = EndsWithChd(path);
    if (chd) opened = OpenChd(path, track);
    else if (g_default.open_track_iterator != nullptr) opened = g_default.open_track_iterator(path, track, iterator);
    if (opened == nullptr) return nullptr;
    auto* handle = new Handle;
    if (chd) handle->chd = static_cast<ChdTrack*>(opened);
    else handle->other = opened;
    return handle;
}

void* RC_CCONV OpenTrack(const char* path, uint32_t track) { return EndsWithChd(path) ? OpenTrackIterator(path, track, nullptr) : nullptr; }

size_t RC_CCONV ReadSector(void* opened, uint32_t sector, void* buffer, size_t requested) {
    auto* handle = static_cast<Handle*>(opened);
    if (handle == nullptr) return 0;
    if (handle->chd != nullptr) return ReadChd(*handle->chd, sector, buffer, requested);
    return g_default.read_sector(handle->other, sector, buffer, requested);
}

void RC_CCONV CloseTrack(void* opened) {
    auto* handle = static_cast<Handle*>(opened);
    if (handle == nullptr) return;
    if (handle->chd != nullptr) {
        chd_close(handle->chd->chd);
        delete handle->chd;
    } else if (handle->other != nullptr) {
        g_default.close_track(handle->other);
    }
    delete handle;
}

uint32_t RC_CCONV FirstTrackSector(void* opened) {
    auto* handle = static_cast<Handle*>(opened);
    if (handle == nullptr) return 0;
    if (handle->chd != nullptr) return handle->chd->lba;
    return g_default.first_track_sector(handle->other);
}

}

void ChdCdReader(rc_hash_cdreader& reader) {
    rc_hash_get_default_cdreader(&g_default);
    reader.open_track = OpenTrack;
    reader.open_track_iterator = OpenTrackIterator;
    reader.read_sector = ReadSector;
    reader.close_track = CloseTrack;
    reader.first_track_sector = FirstTrackSector;
}

}
