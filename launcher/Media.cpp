#include "Media.hpp"
#include "Common.hpp"

// stb, kept to this file (static), with GIF, which the client's own copy leaves out.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cstring>

namespace Ranga {

namespace {

constexpr int kStepMs = 100, kMaxMs = 5000;

// The middle of the picture in the shape asked for, shrunk to it.
std::vector<uint8_t> Fit(const uint8_t* rgba, int width, int height, int stride, int outWidth, int outHeight) {
    const double target = static_cast<double>(outWidth) / outHeight;
    int cropWidth = width, cropHeight = height, x = 0, y = 0;
    if (static_cast<double>(width) / height > target) {
        cropWidth = std::max(1, static_cast<int>(height * target));
        x = (width - cropWidth) / 2;
    } else {
        cropHeight = std::max(1, static_cast<int>(width / target));
        y = (height - cropHeight) / 2;
    }
    std::vector<uint8_t> out(static_cast<size_t>(outWidth) * outHeight * 4);
    stbir_resize_uint8_linear(rgba + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4, cropWidth, cropHeight, stride, out.data(), outWidth,
                              outHeight, outWidth * 4, STBIR_RGBA);
    return out;
}

// Frames of any timing to one every 100 ms, at most 5 s: each step takes the frame showing at that moment.
void Resample(const std::vector<std::vector<uint8_t>>& frames, const std::vector<int>& delays, Media& out) {
    int total = 0;
    for (const int delay : delays) total += std::max(delay, 10);
    if (frames.size() <= 1 || total <= kStepMs) {
        if (!frames.empty()) out.frames.push_back({frames.front(), kStepMs});
        return;
    }
    total = std::min(total, kMaxMs);
    size_t index = 0;
    int ends = std::max(delays[0], 10);
    for (int at = 0; at < total; at += kStepMs) {
        while (at >= ends && index + 1 < frames.size()) ends += std::max(delays[++index], 10);
        out.frames.push_back({frames[index], kStepMs});
    }
}

bool ReadPicture(const std::string& bytes, int width, int height, Media& out, std::string& error) {
    int w = 0, h = 0, channels = 0, layers = 0;
    int* delays = nullptr;
    const bool gif = bytes.size() > 6 && bytes.compare(0, 3, "GIF") == 0;
    unsigned char* pixels = gif ? stbi_load_gif_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &delays, &w, &h, &layers, &channels, 4)
                                : stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    if (pixels == nullptr) {
        error = "Não deu para ler essa imagem (aceita PNG, JPG, BMP, GIF ou vídeo MP4).";
        return false;
    }
    if (!gif) layers = 1;
    std::vector<std::vector<uint8_t>> frames;
    std::vector<int> times;
    for (int i = 0; i < layers; ++i) {
        frames.push_back(Fit(pixels + static_cast<size_t>(i) * w * h * 4, w, h, w * 4, width, height));
        times.push_back(delays != nullptr ? delays[i] : kStepMs);
        // A long GIF: past five seconds is not kept anyway.
        int sum = 0;
        for (const int t : times) sum += std::max(t, 10);
        if (sum >= kMaxMs) break;
    }
    stbi_image_free(pixels);
    if (delays != nullptr) STBI_FREE(delays);
    Resample(frames, times, out);
    return true;
}

template <typename T> void Release(T*& p) {
    if (p != nullptr) p->Release();
    p = nullptr;
}

// A video, through Media Foundation: frames converted to RGB32 by Windows, one every 100 ms, the first 5 s.
bool ReadVideo(const std::wstring& path, int width, int height, Media& out, std::string& error) {
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    bool ok = false;
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        IMFAttributes* attributes = nullptr;
        IMFSourceReader* reader = nullptr;
        IMFMediaType* wanted = nullptr;
        IMFMediaType* current = nullptr;
        if (SUCCEEDED(MFCreateAttributes(&attributes, 1)) && SUCCEEDED(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) &&
            SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(), attributes, &reader)) &&
            SUCCEEDED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE)) &&
            SUCCEEDED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE)) && SUCCEEDED(MFCreateMediaType(&wanted)) &&
            SUCCEEDED(wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) && SUCCEEDED(wanted->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) &&
            SUCCEEDED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, wanted)) &&
            SUCCEEDED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current))) {
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &w, &h);
            INT32 stride = static_cast<INT32>(MFGetAttributeUINT32(current, MF_MT_DEFAULT_STRIDE, w * 4));
            LONGLONG next = 0;
            std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
            while (w > 0 && h > 0 && out.frames.size() < static_cast<size_t>(kMaxMs / kStepMs)) {
                DWORD stream = 0, flags = 0;
                LONGLONG stamp = 0;
                IMFSample* sample = nullptr;
                if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &stamp, &sample))) break;
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                    Release(sample);
                    break;
                }
                if (sample == nullptr) continue;
                if (stamp >= next) {
                    IMFMediaBuffer* buffer = nullptr;
                    BYTE* data = nullptr;
                    DWORD length = 0;
                    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                        // BGRX rows, bottom-up when the stride is negative.
                        const LONG rowBytes = static_cast<LONG>(w) * 4;
                        if (length >= static_cast<DWORD>(rowBytes) * h) {
                            for (UINT32 row = 0; row < h; ++row) {
                                const BYTE* source = stride < 0 ? data + static_cast<size_t>(h - 1 - row) * rowBytes : data + static_cast<size_t>(row) * std::abs(stride);
                                uint8_t* target = rgba.data() + static_cast<size_t>(row) * rowBytes;
                                for (UINT32 x = 0; x < w; ++x) {
                                    target[x * 4 + 0] = source[x * 4 + 2];
                                    target[x * 4 + 1] = source[x * 4 + 1];
                                    target[x * 4 + 2] = source[x * 4 + 0];
                                    target[x * 4 + 3] = 255;
                                }
                            }
                            out.frames.push_back({Fit(rgba.data(), static_cast<int>(w), static_cast<int>(h), rowBytes, width, height), kStepMs});
                        }
                        buffer->Unlock();
                    }
                    Release(buffer);
                    next += static_cast<LONGLONG>(kStepMs) * 10000;
                }
                Release(sample);
            }
            ok = !out.frames.empty();
        }
        Release(current);
        Release(wanted);
        Release(reader);
        Release(attributes);
        MFShutdown();
    }
    if (com) CoUninitialize();
    if (!ok) error = "Não deu para ler esse vídeo. Tente um MP4 (H.264) curto.";
    return ok;
}

void Put16(std::string& out, uint16_t v) { out.append(reinterpret_cast<const char*>(&v), 2); }
void Put32(std::string& out, uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); }

std::string Encode(const Media& media, size_t step, int quality) {
    std::string out = "RANI";
    Put16(out, 1);
    Put16(out, static_cast<uint16_t>(media.width));
    Put16(out, static_cast<uint16_t>(media.height));
    const size_t count = (media.frames.size() + step - 1) / step;
    Put16(out, static_cast<uint16_t>(count));
    for (size_t i = 0; i < media.frames.size(); i += step) {
        std::string jpeg;
        stbi_write_jpg_to_func([](void* context, void* data, int size) { static_cast<std::string*>(context)->append(static_cast<const char*>(data), static_cast<size_t>(size)); },
                               &jpeg, media.width, media.height, 4, media.frames[i].rgba.data(), quality);
        int delay = 0;
        for (size_t k = i; k < std::min(i + step, media.frames.size()); ++k) delay += media.frames[k].delayMs;
        Put16(out, static_cast<uint16_t>(delay));
        Put32(out, static_cast<uint32_t>(jpeg.size()));
        out += jpeg;
    }
    return out;
}

}

bool ImportMedia(const std::wstring& path, int width, int height, Media& out, std::string& error) {
    out = Media{};
    out.width = width;
    out.height = height;
    const std::wstring ext = Extension(path);
    if (ext == L".mp4" || ext == L".mov" || ext == L".wmv" || ext == L".avi" || ext == L".m4v" || ext == L".mkv" || ext == L".webm") return ReadVideo(path, width, height, out, error);
    const std::string bytes = ReadBytes(path);
    if (bytes.empty()) {
        error = "Não deu para abrir o arquivo.";
        return false;
    }
    return ReadPicture(bytes, width, height, out, error);
}

bool ImportPicture(const std::wstring& path, int maxSide, Media& out, std::string& error) {
    out = Media{};
    const std::string bytes = ReadBytes(path);
    int w = 0, h = 0, channels = 0;
    if (bytes.empty() || !stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &w, &h, &channels) || w <= 0 || h <= 0) {
        error = "Não deu para ler essa imagem (aceita PNG, JPG, BMP ou GIF).";
        return false;
    }
    const double shrink = std::min(1.0, static_cast<double>(maxSide) / std::max(w, h));
    out.width = std::max(1, static_cast<int>(w * shrink));
    out.height = std::max(1, static_cast<int>(h * shrink));
    return ReadPicture(bytes, out.width, out.height, out, error);
}

std::string EncodeMedia(const Media& media, size_t maxBytes) {
    if (media.frames.empty()) return "";
    // Fewer frames (every other one, then one in three...) until it fits; a still picture fits by lowering quality.
    for (size_t step = 1; step <= media.frames.size(); ++step) {
        const std::string out = Encode(media, step, 82);
        if (out.size() <= maxBytes) return out;
    }
    return Encode(media, media.frames.size(), 60);
}

bool DecodeMedia(const std::string& bytes, Media& out) {
    out = Media{};
    if (bytes.size() < 12 || bytes.compare(0, 4, "RANI") != 0) return false;
    uint16_t version = 0, width = 0, height = 0, count = 0;
    std::memcpy(&version, bytes.data() + 4, 2);
    std::memcpy(&width, bytes.data() + 6, 2);
    std::memcpy(&height, bytes.data() + 8, 2);
    std::memcpy(&count, bytes.data() + 10, 2);
    if (version != 1 || width == 0 || height == 0 || width > 2048 || height > 2048) return false;
    out.width = width;
    out.height = height;
    size_t at = 12;
    for (uint16_t i = 0; i < count; ++i) {
        if (at + 6 > bytes.size()) return false;
        uint16_t delay = 0;
        uint32_t size = 0;
        std::memcpy(&delay, bytes.data() + at, 2);
        std::memcpy(&size, bytes.data() + at + 2, 4);
        at += 6;
        if (at + size > bytes.size()) return false;
        int w = 0, h = 0, channels = 0;
        unsigned char* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data() + at), static_cast<int>(size), &w, &h, &channels, 4);
        at += size;
        if (pixels == nullptr || w != width || h != height) {
            if (pixels != nullptr) stbi_image_free(pixels);
            return false;
        }
        out.frames.push_back({std::vector<uint8_t>(pixels, pixels + static_cast<size_t>(w) * h * 4), std::max<int>(delay, 20)});
        stbi_image_free(pixels);
    }
    return !out.frames.empty();
}

}
