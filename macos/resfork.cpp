#include "resfork.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace MacOS {

namespace {

	uint16_t ru16(const uint8_t *p)
	{
		return (uint16_t)((p[0] << 8) | p[1]);
	}

	uint32_t ru32(const uint8_t *p)
	{
		return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
			((uint32_t)p[2] << 8) | (uint32_t)p[3];
	}

	void wu16(std::vector<uint8_t> &o, uint16_t v)
	{
		o.push_back((uint8_t)(v >> 8));
		o.push_back((uint8_t)v);
	}

	void wu32(std::vector<uint8_t> &o, uint32_t v)
	{
		o.push_back((uint8_t)(v >> 24));
		o.push_back((uint8_t)(v >> 16));
		o.push_back((uint8_t)(v >> 8));
		o.push_back((uint8_t)v);
	}

	void pu16(std::vector<uint8_t> &o, size_t at, uint16_t v)
	{
		o[at] = (uint8_t)(v >> 8);
		o[at + 1] = (uint8_t)v;
	}

	void pu32(std::vector<uint8_t> &o, size_t at, uint32_t v)
	{
		o[at] = (uint8_t)(v >> 24);
		o[at + 1] = (uint8_t)(v >> 16);
		o[at + 2] = (uint8_t)(v >> 8);
		o[at + 3] = (uint8_t)v;
	}

	bool inRange(uint64_t off, uint64_t len, uint64_t size)
	{
		return off <= size && len <= size - off;
	}

} // namespace

std::string rsrcPath(const std::string &dataPath)
{
	return dataPath + ".rsrc";
}

std::string finfoPath(const std::string &dataPath)
{
	return dataPath + ".finfo";
}

bool readFinderInfo(const std::string &dataPath, uint8_t info[32])
{
	std::memset(info, 0, 32);
	std::ifstream in(finfoPath(dataPath).c_str(), std::ios::binary);
	if (!in) return false;
	in.read(reinterpret_cast<char *>(info), 32);
	return true;
}

bool writeFinderInfo(const std::string &dataPath, const uint8_t info[32])
{
	std::ofstream out(finfoPath(dataPath).c_str(), std::ios::binary | std::ios::trunc);
	if (!out) return false;
	out.write(reinterpret_cast<const char *>(info), 32);
	return (bool)out;
}

bool ResFork::read(const std::string &path, std::string &error)
{
	items.clear();
	fileAttrs = 0;

	std::ifstream in(path.c_str(), std::ios::binary);
	if (!in)
	{
		error = "cannot open " + path;
		return false;
	}
	std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)),
		std::istreambuf_iterator<char>());
	if (b.size() < 16)
	{
		error = "resource fork is shorter than its header";
		return false;
	}

	uint32_t dataOff = ru32(&b[0]);
	uint32_t mapOff = ru32(&b[4]);
	uint32_t dataLen = ru32(&b[8]);
	uint32_t mapLen = ru32(&b[12]);
	if (!inRange(mapOff, mapLen, b.size()) || !inRange(dataOff, dataLen, b.size()))
	{
		error = "resource fork header points outside the file";
		return false;
	}
	if (mapLen < 28)
	{
		error = "resource map is too small";
		return false;
	}

	const uint8_t *map = &b[mapOff];
	fileAttrs = ru16(map + 22);
	uint16_t typeOff = ru16(map + 24);
	uint16_t nameOff = ru16(map + 26);
	if ((uint32_t)typeOff + 2 > mapLen)
	{
		error = "type list is outside the resource map";
		return false;
	}

	const uint8_t *tl = map + typeOff;
	uint16_t nTypesM1 = ru16(tl);
	if (nTypesM1 == 0xffff) return true;

	uint32_t nTypes = (uint32_t)nTypesM1 + 1;
	if ((uint32_t)typeOff + 2 + nTypes * 8 > mapLen)
	{
		error = "type list overruns the resource map";
		return false;
	}

	for (uint32_t t = 0; t < nTypes; ++t)
	{
		const uint8_t *te = tl + 2 + t * 8;
		uint32_t type = ru32(te);
		uint32_t count = (uint32_t)ru16(te + 4) + 1;
		uint16_t refOff = ru16(te + 6);
		uint64_t refAt = (uint64_t)typeOff + refOff;
		if (!inRange(refAt, (uint64_t)count * 12, mapLen))
		{
			error = "reference list overruns the resource map";
			return false;
		}
		const uint8_t *rb = map + refAt;
		for (uint32_t k = 0; k < count; ++k)
		{
			const uint8_t *re = rb + k * 12;
			ResItem it;
			it.type = type;
			it.id = (int16_t)ru16(re);
			it.attrs = re[4];
			uint16_t nref = ru16(re + 2);
			uint32_t doff = ((uint32_t)re[5] << 16) | ((uint32_t)re[6] << 8) | re[7];
			if (nref == 0xffff)
			{
				it.name.clear();
			}
			else
			{
				uint64_t np = (uint64_t)nameOff + nref;
				if (np >= mapLen)
				{
					error = "resource name is outside the map";
					return false;
				}
				uint8_t nlen = map[np];
				if (!inRange(np + 1, nlen, mapLen))
				{
					error = "resource name overruns the map";
					return false;
				}
				it.name.assign(reinterpret_cast<const char *>(map + np + 1), nlen);
			}
			if (!inRange(doff, 4, dataLen))
			{
				error = "resource data offset is outside the data area";
				return false;
			}
			uint32_t dlen = ru32(&b[dataOff + doff]);
			if (!inRange((uint64_t)doff + 4, dlen, dataLen))
			{
				error = "resource data overruns the data area";
				return false;
			}
			const uint8_t *dp = &b[dataOff + doff + 4];
			it.data.assign(dp, dp + dlen);
			items.push_back(std::move(it));
		}
	}
	return true;
}

std::vector<uint8_t> ResFork::emptyFork()
{
	ResFork empty;
	std::string error;
	std::vector<uint8_t> bytes;
	// write() goes to a path. Build the same bytes the writer emits for no items.
	// An empty fork is a 256-byte header gap plus a map with no types.
	uint8_t attrs[2] = {0, 0};
	(void)attrs;
	(void)error;

	std::vector<uint8_t> data;
	std::vector<uint8_t> tlist;
	wu16(tlist, 0xffff);

	std::vector<uint8_t> map(28, 0);
	pu16(map, 24, 28); // type list
	pu16(map, 26, (uint16_t)(28 + tlist.size())); // name list, empty
	map.insert(map.end(), tlist.begin(), tlist.end());

	const uint32_t dataOff = 256;
	const uint32_t dataLen = 0;
	const uint32_t mapOff = dataOff;
	const uint32_t mapLen = (uint32_t)map.size();
	std::vector<uint8_t> file(mapOff + mapLen, 0);
	pu32(file, 0, dataOff);
	pu32(file, 4, mapOff);
	pu32(file, 8, dataLen);
	pu32(file, 12, mapLen);
	std::memcpy(&file[mapOff], &map[0], map.size());
	std::memcpy(&file[mapOff], &file[0], 16);
	return file;
}

bool ResFork::write(const std::string &path, std::string &error) const
{
	// Group by type, types and ids ascending, without rearranging *this.
	struct Ref
	{
		uint32_t index;
	};
	std::vector<uint32_t> order(items.size());
	for (uint32_t i = 0; i < items.size(); ++i) order[i] = i;
	// insertion sort keeps the implementation small and the counts tiny
	for (size_t i = 1; i < order.size(); ++i)
	{
		uint32_t key = order[i];
		size_t j = i;
		while (j > 0)
		{
			const ResItem &a = items[order[j - 1]];
			const ResItem &b = items[key];
			bool greater = a.type > b.type || (a.type == b.type && a.id > b.id);
			if (!greater) break;
			order[j] = order[j - 1];
			--j;
		}
		order[j] = key;
	}

	std::vector<uint8_t> data;
	struct Group
	{
		uint32_t type;
		uint16_t countM1;
		std::vector<uint32_t> members; // indexes into items, data offsets filled below
		std::vector<uint32_t> dataOff;
	};
	std::vector<Group> groups;
	for (uint32_t ix : order)
	{
		const ResItem &it = items[ix];
		if (groups.empty() || groups.back().type != it.type)
		{
			Group g;
			g.type = it.type;
			g.countM1 = 0;
			groups.push_back(g);
		}
		else if (groups.back().countM1 != 0xffff)
		{
			groups.back().countM1++;
		}
		uint32_t off = (uint32_t)data.size();
		wu32(data, (uint32_t)it.data.size());
		data.insert(data.end(), it.data.begin(), it.data.end());
		if (data.size() & 1) data.push_back(0);
		groups.back().members.push_back(ix);
		groups.back().dataOff.push_back(off);
	}

	std::vector<uint8_t> names;
	std::vector<uint8_t> tlist;
	uint16_t nTypes = (uint16_t)groups.size();
	wu16(tlist, nTypes == 0 ? 0xffff : (uint16_t)(nTypes - 1));
	size_t entries = tlist.size();
	tlist.resize(entries + (size_t)nTypes * 8, 0);
	std::vector<uint8_t> refs;
	for (uint16_t gi = 0; gi < nTypes; ++gi)
	{
		const Group &g = groups[gi];
		size_t at = entries + (size_t)gi * 8;
		pu32(tlist, at, g.type);
		pu16(tlist, at + 4, g.countM1);
		pu16(tlist, at + 6, (uint16_t)(tlist.size() + refs.size()));
		for (size_t k = 0; k < g.members.size(); ++k)
		{
			const ResItem &it = items[g.members[k]];
			uint16_t noff = 0xffff;
			if (!it.name.empty())
			{
				noff = (uint16_t)names.size();
				uint8_t nlen = (uint8_t)std::min(it.name.size(), (size_t)255);
				names.push_back(nlen);
				names.insert(names.end(), it.name.begin(), it.name.begin() + nlen);
			}
			wu16(refs, (uint16_t)it.id);
			wu16(refs, noff);
			refs.push_back(it.attrs);
			uint32_t off = g.dataOff[k];
			refs.push_back((uint8_t)(off >> 16));
			refs.push_back((uint8_t)(off >> 8));
			refs.push_back((uint8_t)off);
			wu32(refs, 0); // reserved handle
		}
	}
	tlist.insert(tlist.end(), refs.begin(), refs.end());

	std::vector<uint8_t> map(28, 0);
	pu16(map, 22, fileAttrs);
	pu16(map, 24, 28);
	pu16(map, 26, (uint16_t)(28 + tlist.size()));
	map.insert(map.end(), tlist.begin(), tlist.end());
	map.insert(map.end(), names.begin(), names.end());

	const uint32_t dataOff = 256;
	const uint32_t dataLen = (uint32_t)data.size();
	const uint32_t mapOff = dataOff + dataLen;
	const uint32_t mapLen = (uint32_t)map.size();
	if (mapLen > 0xffff)
	{
		error = "resource map exceeds 64K";
		return false;
	}
	std::vector<uint8_t> file(mapOff + mapLen, 0);
	pu32(file, 0, dataOff);
	pu32(file, 4, mapOff);
	pu32(file, 8, dataLen);
	pu32(file, 12, mapLen);
	if (!data.empty()) std::memcpy(&file[dataOff], &data[0], data.size());
	std::memcpy(&file[mapOff], &map[0], map.size());
	std::memcpy(&file[mapOff], &file[0], 16);

	std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
	if (!out)
	{
		error = "cannot write " + path;
		return false;
	}
	out.write(reinterpret_cast<const char *>(&file[0]), file.size());
	if (!out)
	{
		error = "short write of " + path;
		return false;
	}
	return true;
}

} // namespace MacOS
