#ifndef __macos_resfork_h__
#define __macos_resfork_h__

#include <cstdint>
#include <string>
#include <vector>

namespace MacOS {

// One resource in a Macintosh resource fork. Bytes stay in 68K order.
struct ResItem {
	uint32_t type;
	int16_t id;
	uint8_t attrs;
	std::string name;
	std::vector<uint8_t> data;
};

// Reader and writer for a raw resource-fork file (a <name>.rsrc sidecar).
// The writer round-trips through the reader. It does not reproduce Carbon's bytes.
class ResFork {
public:
	uint16_t fileAttrs;
	std::vector<ResItem> items;

	ResFork() : fileAttrs(0) {}

	bool read(const std::string &path, std::string &error);
	bool write(const std::string &path, std::string &error) const;
	static std::vector<uint8_t> emptyFork();
};

std::string rsrcPath(const std::string &dataPath);
std::string finfoPath(const std::string &dataPath);

// Fills info with 32 bytes. Returns false when the sidecar is absent
// (info is then thirty-two zero bytes).
bool readFinderInfo(const std::string &dataPath, uint8_t info[32]);
bool writeFinderInfo(const std::string &dataPath, const uint8_t info[32]);

}

#endif
