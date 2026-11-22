#ifndef LAUNCHER_MEDIA_HPP
#define LAUNCHER_MEDIA_HPP

// The profile's photo and cover: a picture, a GIF or a short video (MP4, MOV, WMV, AVI, through Windows' own Media
// Foundation), cut to the middle of the shape asked for, shrunk, and kept as a few seconds of JPEG frames in Ranga's
// own small format ("RANI"), which is what goes to the friends.

#include <cstdint>
#include <string>
#include <vector>

namespace Ranga {

struct MediaFrame {
    std::vector<uint8_t> rgba;
    int delayMs = 100;
};

struct Media {
    int width = 0, height = 0;
    std::vector<MediaFrame> frames; // one frame: a still picture
};

// The photo is a 128 x 128 square; the cover 720 x 144. At most 5 seconds, 10 frames a second.
inline constexpr int kPhotoSide = 128, kCoverWidth = 720, kCoverHeight = 144;

// Reads path (PNG, JPG, BMP, GIF, MP4, MOV, WMV, AVI, M4V) into width x height frames; false and why when it cannot.
bool ImportMedia(const std::wstring& path, int width, int height, Media& out, std::string& error);
// A picture or GIF whole, in its own shape, shrunk so its longer side is at most maxSide (the chat's images).
bool ImportPicture(const std::wstring& path, int maxSide, Media& out, std::string& error);
// The frames as "RANI": JPEG frames, fewer of them when the whole would pass maxBytes.
std::string EncodeMedia(const Media& media, size_t maxBytes = 900 * 1024);
bool DecodeMedia(const std::string& bytes, Media& out);

// zlib (stb's), for the saves in the cloud: Deflate packs; Inflate unpacks to exactly size bytes or fails.
std::string Deflate(const std::string& bytes);
bool Inflate(const std::string& packed, size_t size, std::string& out);

}

#endif
