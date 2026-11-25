#include "romset.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <span>
#include <zlib.h>

namespace romset {

namespace {

// ---- a minimal zip reader: the central directory, stored and deflated entries ----

struct entry { std::string name; uint32_t crc = 0, csize = 0, usize = 0, local = 0; uint16_t method = 0; };

uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t *p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }

struct zip
{
	std::vector<uint8_t> data;
	std::vector<entry> entries;

	bool open(const std::string &path)
	{
		// vf2.zip is about 20 MB: anything over 256 MB isn't the set, and isn't read
		std::error_code ec;
		const uintmax_t size = std::filesystem::file_size(path, ec);
		if (ec || size < 22 || size > (256u << 20)) return false;
		FILE *f = fopen(path.c_str(), "rb");
		if (!f) return false;
		const long n = long(size);
		data.resize(size_t(n));
		const bool got = fread(data.data(), 1, data.size(), f) == data.size();
		fclose(f);
		if (!got) return false;
		// the end of central directory record, within the last 64 KB
		long eocd = -1;
		for (long i = n - 22; i >= 0 && i >= n - 22 - 65535; i--)
			if (le32(&data[i]) == 0x06054b50) { eocd = i; break; }
		if (eocd < 0) return false;
		const uint16_t count = le16(&data[eocd + 10]);
		size_t at = le32(&data[eocd + 16]);
		for (int k = 0; k < count; k++) {
			if (at + 46 > data.size() || le32(&data[at]) != 0x02014b50) return false;
			entry e;
			e.method = le16(&data[at + 10]);
			e.crc = le32(&data[at + 16]);
			e.csize = le32(&data[at + 20]);
			e.usize = le32(&data[at + 24]);
			const uint16_t nlen = le16(&data[at + 28]), xlen = le16(&data[at + 30]), clen = le16(&data[at + 32]);
			e.local = le32(&data[at + 42]);
			if (at + 46 + nlen > data.size()) return false;
			e.name.assign(reinterpret_cast<const char *>(&data[at + 46]), nlen);
			const size_t slash = e.name.find_last_of('/');
			if (slash != std::string::npos) e.name = e.name.substr(slash + 1);
			entries.push_back(e);
			at += 46 + nlen + xlen + clen;
		}
		return true;
	}

	bool read(const entry &e, std::vector<uint8_t> &out) const
	{
		const size_t local = e.local;
		if (local + 30 > data.size() || le32(&data[local]) != 0x04034b50) return false;
		const size_t start = local + 30 + le16(&data[local + 26]) + le16(&data[local + 28]);
		if (start + size_t(e.csize) > data.size() || e.usize > (64u << 20)) return false;
		out.resize(e.usize);
		if (e.method == 0) {
			if (e.csize != e.usize) return false;
			memcpy(out.data(), &data[start], e.usize);
		} else if (e.method == 8) {
			z_stream z{};
			if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return false;
			z.next_in = const_cast<Bytef *>(&data[start]);
			z.avail_in = e.csize;
			z.next_out = out.data();
			z.avail_out = e.usize;
			const int r = inflate(&z, Z_FINISH);
			inflateEnd(&z);
			if (r != Z_STREAM_END || z.total_out != e.usize) return false;
		} else {
			return false;
		}
		return uint32_t(crc32(0, out.data(), uInt(out.size()))) == e.crc;
	}
};

// ---- the set, as MAME's ROM_START( vf2 ) with MODEL2_CPU_BOARD and MODEL2A_VID_BOARD ----

struct region_def { const char *name; size_t size; uint8_t fill; };
const region_def REGIONS[N_REGIONS] = {
	{ "maincpu", 0x200000, 0 }, { "main_data", 0x2400000, 0 }, { "copro_data", 0x800000, 0 },
	{ "polygons", 0x2000000, 0 }, { "textures", 0x1000000, 0xff }, { "audiocpu", 0x80000, 0 },
	{ "samples", 0x800000, 0 }, { "copro_tgp_tables", 0x40000, 0 }, { "other_data", 0x80000, 0 },
	{ "video_unk", 0x200000, 0 },
};
// ROM_LOAD32_WORD (16-bit halves of 32-bit words), ROM_LOAD16_WORD_SWAP, ROM_LOAD32_BYTE
enum kind { WORD32, WORDSWAP, BYTE32, RAW };
struct rom { const char *name; uint32_t crc; region_id region; uint32_t offset; kind k; };
const rom VF2_ROMS[] = {
	{ "epr-18385.12", 0x78ed2d41, MAINCPU, 0x000000, WORD32 },
	{ "epr-18386.13", 0x3418f428, MAINCPU, 0x000002, WORD32 },
	{ "epr-18387.14", 0x124a8453, MAINCPU, 0x040000, WORD32 },
	{ "epr-18388.15", 0x8d347980, MAINCPU, 0x040002, WORD32 },
	{ "mpr-17560.10", 0xd1389864, MAIN_DATA, 0x000000, WORD32 },
	{ "mpr-17561.11", 0xb98d0101, MAIN_DATA, 0x000002, WORD32 },
	{ "mpr-17558.8", 0x4b15f5a6, MAIN_DATA, 0x400000, WORD32 },
	{ "mpr-17559.9", 0xd3264de6, MAIN_DATA, 0x400002, WORD32 },
	{ "mpr-17566.6", 0xfb41ef98, MAIN_DATA, 0x800000, WORD32 },
	{ "mpr-17567.7", 0xc3396922, MAIN_DATA, 0x800002, WORD32 },
	{ "mpr-17564.4", 0xd8062489, MAIN_DATA, 0xc00000, WORD32 },
	{ "mpr-17565.5", 0x0517c6e9, MAIN_DATA, 0xc00002, WORD32 },
	{ "mpr-17554.16", 0x27896d82, POLYGONS, 0x000000, WORD32 },
	{ "mpr-17548.20", 0xc95facc2, POLYGONS, 0x000002, WORD32 },
	{ "mpr-17555.17", 0x4df2810b, POLYGONS, 0x400000, WORD32 },
	{ "mpr-17549.21", 0xe0bce0e6, POLYGONS, 0x400002, WORD32 },
	{ "mpr-17556.18", 0x41a47616, POLYGONS, 0x800000, WORD32 },
	{ "mpr-17550.22", 0xc36ff3f5, POLYGONS, 0x800002, WORD32 },
	{ "mpr-17553.25", 0x5da1c5d3, TEXTURES, 0x000000, WORD32 },
	{ "mpr-17552.24", 0xe91e7427, TEXTURES, 0x000002, WORD32 },
	{ "mpr-17547.27", 0xbe940431, TEXTURES, 0x800000, WORD32 },
	{ "mpr-17546.26", 0x042a194b, TEXTURES, 0x800002, WORD32 },
	{ "epr-17574.30", 0x4d4c3a55, AUDIOCPU, 0x000000, WORDSWAP },
	{ "mpr-17573.31", 0xe43557fe, SAMPLES, 0x000000, WORDSWAP },
	{ "mpr-17572.32", 0x4febecc8, SAMPLES, 0x200000, WORDSWAP },
	{ "mpr-17571.36", 0x51caa584, SAMPLES, 0x400000, WORDSWAP },
	{ "mpr-17570.37", 0xbccd324b, SAMPLES, 0x600000, WORDSWAP },
};
const rom CPU_BOARD_ROMS[] = {
	{ "opr-14742a.45", 0x90c6b117, COPRO_TABLES, 0x000000, WORD32 },
	{ "opr-14743a.46", 0xae7f446b, COPRO_TABLES, 0x000002, WORD32 },
	{ "opr-14744.58", 0x730ea9e0, OTHER_DATA, 0x000000, WORD32 },
	{ "opr-14745.59", 0x4c934d96, OTHER_DATA, 0x000002, WORD32 },
	{ "opr-14746.62", 0x2a266cbd, OTHER_DATA, 0x040000, WORD32 },
	{ "opr-14747.63", 0xa4ad5e19, OTHER_DATA, 0x040002, WORD32 },
};
const rom VF2_VIDEO_BOARD_ROMS[] = {
	{ "mpr-16310.15", 0xc078a780, VIDEO_UNK, 0x000000, BYTE32 },
	{ "mpr-16311.16", 0x452a492b, VIDEO_UNK, 0x000001, BYTE32 },
	{ "mpr-16312.14", 0xa25fef5b, VIDEO_UNK, 0x000002, BYTE32 },
};

// Original Model 2, MAME's daytona parent set (Revision A).
const rom DAYTONA_ROMS[] = {
	{ "epr-16722a.12", 0x48b94318, MAINCPU, 0x000000, WORD32 },
	{ "epr-16723a.13", 0x8af8b32d, MAINCPU, 0x000002, WORD32 },
	{ "mpr-16528.10", 0x9ce591f6, MAIN_DATA, 0x000000, WORD32 },
	{ "mpr-16529.11", 0xf7095eaf, MAIN_DATA, 0x000002, WORD32 },
	{ "mpr-16808.8", 0x44f1f5a0, MAIN_DATA, 0x400000, WORD32 },
	{ "mpr-16809.9", 0x37a2dd12, MAIN_DATA, 0x400002, WORD32 },
	{ "epr-16724a.6", 0x469f10fd, MAIN_DATA, 0x800000, WORD32 },
	{ "epr-16725a.7", 0xba0df8db, MAIN_DATA, 0x800002, WORD32 },
	{ "mpr-16537.ic28", 0x36b7c35a, COPRO_DATA, 0x000000, WORD32 },
	{ "mpr-16536.ic29", 0x6d6afed9, COPRO_DATA, 0x000002, WORD32 },
	{ "mpr-16523.ic16", 0x2f484d42, POLYGONS, 0x000000, WORD32 },
	{ "mpr-16518.ic20", 0xdf683bf7, POLYGONS, 0x000002, WORD32 },
	{ "mpr-16524.ic17", 0x34658bd7, POLYGONS, 0x400000, WORD32 },
	{ "mpr-16519.ic21", 0xfacd1c81, POLYGONS, 0x400002, WORD32 },
	{ "mpr-16525.ic18", 0xfb517521, POLYGONS, 0x800000, WORD32 },
	{ "mpr-16520.ic22", 0xd66bd9bd, POLYGONS, 0x800002, WORD32 },
	{ "mpr-16772.ic19", 0x770ed912, POLYGONS, 0xc00000, WORD32 },
	{ "mpr-16771.ic23", 0xa2205124, POLYGONS, 0xc00002, WORD32 },
	{ "mpr-16522.25", 0x55d39a57, TEXTURES, 0x000000, WORD32 },
	{ "mpr-16521.24", 0xaf1934fb, TEXTURES, 0x000002, WORD32 },
	{ "mpr-16770.27", 0xf9fa7bfb, TEXTURES, 0x800000, WORD32 },
	{ "mpr-16769.26", 0xe57429e9, TEXTURES, 0x800002, WORD32 },
	{ "epr-16720.7", 0x8e73cffd, AUDIOCPU, 0x000000, WORDSWAP },
	{ "epr-16721.8", 0x1bb3b7b7, AUDIOCPU, 0x020000, WORDSWAP },
	{ "mpr-16491.32", 0x89920903, SAMPLES, 0x000000, RAW },
	{ "mpr-16492.33", 0x459e701b, SAMPLES, 0x200000, RAW },
	{ "mpr-16493.4", 0x9990db15, SAMPLES, 0x400000, RAW },
	{ "mpr-16494.5", 0x600e1d6c, SAMPLES, 0x600000, RAW },
	{ "epr-14869c.25", 0x24b68e64, IOCPU, 0, RAW },
};

const region_def DAYTONA_REGIONS[N_REGIONS] = {
	{ "maincpu", 0x200000, 0 }, { "main_data", 0x2000000, 0 }, { "copro_data", 0x800000, 0 },
	{ "polygons", 0x1000000, 0 }, { "textures", 0x1000000, 0 }, { "audiocpu", 0x40000, 0 },
	{ "samples", 0x800000, 0 }, { "copro_tgp_tables", 0x40000, 0 }, { "other_data", 0x80000, 0 },
	{ "video_unk", 0, 0 },
};

void place(std::vector<uint8_t> &r, const rom &ro, const std::vector<uint8_t> &d)
{
	const size_t off = ro.offset;
	switch (ro.k) {
	case WORD32:
		for (size_t i = 0; i + 1 < d.size() && off + 2 * i + 1 < r.size(); i += 2) { r[off + 2 * i] = d[i]; r[off + 2 * i + 1] = d[i + 1]; }
		break;
	case WORDSWAP:
		for (size_t i = 0; i + 1 < d.size() && off + i + 1 < r.size(); i += 2) { r[off + i] = d[i + 1]; r[off + i + 1] = d[i]; }
		break;
	case BYTE32:
		for (size_t i = 0; i < d.size() && off + 4 * i < r.size(); i++) r[off + 4 * i] = d[i];
		break;
	case RAW:
		if (ro.region == IOCPU) r = d;
		else if (off <= r.size() && d.size() <= r.size() - off)
			memcpy(r.data() + off, d.data(), d.size());
		break;
	}
}

} // namespace

const char *region_name(int r) { return r == IOCPU ? "iocpu" : r >= 0 && r < N_REGIONS ? REGIONS[r].name : "?"; }

static result load_set(const std::string &path, const region_def (&regions)[N_REGIONS],
	std::initializer_list<std::span<const rom>> lists, const char *description)
{
	result res;
	zip z;
	if (!z.open(path)) { res.error = "Couldn't read " + path + " as a zip file."; return res; }
	for (int r = 0; r < N_REGIONS; r++) res.regions[r].assign(regions[r].size, regions[r].fill);
	int bad = 0;
	std::vector<uint8_t> d;
	for (auto list : lists) for (const rom &ro : list) {
		rom_status st{ ro.name, ro.crc, ro.region, ro.offset, rom_status::OK };
		// by CRC first (any name), then report a same-named file with other contents
		const entry *e = nullptr;
		for (const entry &x : z.entries) if (x.crc == ro.crc && x.name == ro.name) { e = &x; break; }
		if (!e) for (const entry &x : z.entries) if (x.crc == ro.crc) { e = &x; break; }
		if (!e) {
			st.state = rom_status::MISSING;
			for (const entry &x : z.entries) if (x.name == ro.name) st.state = rom_status::WRONG;
		} else if (!z.read(*e, d)) {
			st.state = rom_status::DAMAGED;
		} else {
			place(ro.region == IOCPU ? res.io_firmware : res.regions[ro.region], ro, d);
		}
		if (st.state != rom_status::OK) {
			if (!bad++) res.error = description;
			res.error += std::string(" ") + ro.name;
		}
		res.roms.push_back(st);
	}
	res.ok = bad == 0;
	return res;
}

result load(const std::string &path)
{
	return load_set(path, REGIONS, {VF2_ROMS, CPU_BOARD_ROMS, VF2_VIDEO_BOARD_ROMS},
		"This isn't a complete Virtua Fighter 2 (Version 2.1) set, MAME's \"vf2\". Not usable:");
}

result load_daytona(const std::string &path)
{
	result res = load_set(path, DAYTONA_REGIONS, {DAYTONA_ROMS, CPU_BOARD_ROMS},
		"Missing or invalid ROMs for Daytona USA (Revision A), MAME's \"daytona\":");
	if (res.ok) {
		auto &data = res.regions[MAIN_DATA];
		for (size_t offset = 0x900000; offset < 0x1000000; offset += 0x100000)
			memcpy(data.data() + offset, data.data() + 0x800000, 0x100000);
		// Record the replication so the machine can fold reads to the source and
		// discard the seven mirror pages after the region is hashed. The copies
		// stay in the buffer for now: the ROM hash is a whole-buffer hash and must
		// see them.
		res.mirror_region = MAIN_DATA;
		res.mirror_base = 0x800000; res.mirror_size = 0x100000; res.mirror_end = 0x1000000;
	}
	return res;
}

} // namespace romset
