#ifndef RANGA_RETRO_CHDREADER_HPP
#define RANGA_RETRO_CHDREADER_HPP

// A disc reader for rcheevos' hashing (rc_hash_cdreader) that also opens .chd images, through libchdr
// (3rdparty/libchdr, BSD): CD (PS1, Sega CD, Saturn, PC Engine CD), GD-ROM (Dreamcast) and DVD (PS2, PSP). Any other
// disc image goes to rcheevos' own reader.

struct rc_hash_cdreader;

namespace Achievements {

void ChdCdReader(rc_hash_cdreader& reader);

}

#endif
