/*
 * Copyright (c) 2013, Kelvin W Sherlock
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <string>
#include <cstring>
#include <cstdio>
#include <list>
#include <map>
#include <vector>
#include <algorithm>

#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <limits.h>

#include "rm.h"
#include "toolbox.h"
#include "mm.h"
#include "os_internal.h"

#include <macos/resfork.h>

#include <cpu/defs.h>
#include <cpu/CpuModule.h>
#include <cpu/fmem.h>

#include <macos/sysequ.h>
#include <macos/errors.h>
#include <macos/tool_return.h>

#include "stackframe.h"
#include "fs_spec.h"
using ToolBox::Log;

using namespace OS::Internal;
using namespace ToolBox;

using MacOS::tool_return;
using MacOS::macos_error_from_errno;
using MacOS::macos_error;

namespace
{

	bool ResLoad = true;

	inline uint16_t SetResError(uint16_t error)
	{
		memoryWriteWord(error, MacOS::ResErr);
		return error;
	}

	// Guest handle -> resource. The resource bytes live in the open file.
	struct ResKey
	{
		uint16_t file;
		uint32_t type;
		int16_t id;
	};

	struct OpenItem
	{
		MacOS::ResItem item;
		uint32_t handle;
	};

	struct OpenFile
	{
		std::string path;
		std::string forkPath;
		uint16_t ref;
		uint16_t attrs;
		std::vector<OpenItem> items;
		bool dirty;
	};

	struct Store
	{
		std::map<uint16_t, OpenFile> files;
		std::vector<uint16_t> chain; // back() is the current file
		std::map<uint32_t, ResKey> handles;
		uint16_t nextRef;

		Store() : nextRef(2) {}

		OpenFile *byRef(uint16_t ref)
		{
			auto it = files.find(ref);
			if (it == files.end()) return nullptr;
			return &it->second;
		}

		OpenFile *current()
		{
			if (chain.empty()) return nullptr;
			return byRef(chain.back());
		}
	};

	Store &store()
	{
		static Store s;
		return s;
	}

	std::string canonPath(const std::string &path)
	{
		char buf[PATH_MAX];
		if (::realpath(path.c_str(), buf)) return buf;
		return path;
	}

	void useFile(uint16_t ref)
	{
		std::vector<uint16_t> &c = store().chain;
		c.erase(std::remove(c.begin(), c.end(), ref), c.end());
		c.push_back(ref);
	}

	OpenItem *findItem(OpenFile &f, uint32_t type, int16_t id)
	{
		for (size_t i = 0; i < f.items.size(); ++i)
			if (f.items[i].item.type == type && f.items[i].item.id == id)
				return &f.items[i];
		return nullptr;
	}

	uint16_t flushFile(OpenFile &f)
	{
		MacOS::ResFork fork;
		fork.fileAttrs = f.attrs;
		fork.items.reserve(f.items.size());
		for (size_t i = 0; i < f.items.size(); ++i)
			fork.items.push_back(f.items[i].item);
		std::string error;
		if (!fork.write(f.forkPath, error)) return MacOS::ioErr;
		f.dirty = false;
		return 0;
	}

	uint16_t publish(OpenFile &f, OpenItem &it, uint32_t &theHandle)
	{
		// Each GetResource allocates a new guest handle. PascalIIGS calls
		// GetResource with SetResLoad(false) to read the on-disk size, releases
		// that handle, then gets the resource again with SetResLoad(true).
		// Reusing the unloaded handle would hand back zero bytes.
		uint32_t ptr = 0;
		uint32_t size = ResLoad ? (uint32_t)it.item.data.size() : 0;
		uint16_t error = MM::Native::NewHandle(size, false, theHandle, ptr);
		if (!theHandle) return SetResError(error);
		MM::Native::HSetRBit(theHandle);
		if (size)
			std::memcpy(memoryPointer(ptr), it.item.data.data(), size);
		it.handle = theHandle;
		store().handles[theHandle] = ResKey{f.ref, it.item.type, it.item.id};
		return SetResError(0);
	}

	// Copies the guest handle back into the map and writes the sidecar.
	uint16_t storeHandle(uint32_t theHandle, bool write)
	{
		auto hit = store().handles.find(theHandle);
		if (hit == store().handles.end()) return SetResError(MacOS::resNotFound);
		OpenFile *f = store().byRef(hit->second.file);
		if (!f) return SetResError(MacOS::resNotFound);
		OpenItem *it = findItem(*f, hit->second.type, hit->second.id);
		if (!it) return SetResError(MacOS::resNotFound);

		auto info = MM::GetHandleInfo(theHandle);
		if (info.error()) return SetResError(info.error());
		it->item.data.resize(info->size);
		if (info->size)
			std::memcpy(it->item.data.data(), memoryPointer(info->address), info->size);
		it->item.attrs = (uint8_t)(it->item.attrs | 0x02);
		f->dirty = true;
		if (!write) return SetResError(0);
		return SetResError(flushFile(*f));
	}


	bool LoadResType(uint32_t type)
	{
		// this filtering was originally to
		// block cursors ('acur').  Can probably
		// change it to a blacklist rather than
		// a whitelist at some point.

		return true;

		switch (type)
		{
			case 0x76657273: // 'vers';
			case 0x48455841: // 'HEXA'
			case 0x53545220: // 'STR '
			case 0x53545223: // 'STR#' (reziigs)
			case 0x59414343: // 'YACC' (pascaliigs)
			case 0x72547970: // 'rTyp' (rezIIgs)
			case 0x756e6974: // 'unit' (Pascal)
			case 0x434f4445: // 'CODE' (Link)
			case 0x5041434b: // 'PACK' (PascalIIgs)
			case 0x4b4f4445: // 'KODE' (Link 32-bit Startup)
			case 0x45525253: // 'ERRS' (PPCLink)
			case 0x63667267: // 'cfrg' (PPCLink)
			case 0x44415441: // 'DATA' (MetroWerks tools)
			case 0x54455854: // 'TEXT' (MetroWerks tools)
			case 0x6f626a64: // 'objd' (MetroWerks tools)
				return true;
			default:
				return false;
		}

	}



}
namespace RM
{



	uint16_t getById(uint32_t type, int16_t id, bool topOnly, uint32_t &theHandle)
	{
		theHandle = 0;
		if (!LoadResType(type)) return SetResError(MacOS::resNotFound);
		Store &s = store();
		if (s.chain.empty()) return SetResError(MacOS::resNotFound);
		for (int i = (int)s.chain.size() - 1; i >= 0; --i)
		{
			OpenFile *f = s.byRef(s.chain[i]);
			if (!f) continue;
			OpenItem *it = findItem(*f, type, id);
			if (it) return publish(*f, *it, theHandle);
			if (topOnly) break;
		}
		return SetResError(MacOS::resNotFound);
	}

	uint16_t getByName(uint32_t type, const std::string &name, bool topOnly, uint32_t &theHandle)
	{
		theHandle = 0;
		if (!LoadResType(type)) return SetResError(MacOS::resNotFound);
		Store &s = store();
		if (s.chain.empty()) return SetResError(MacOS::resNotFound);
		for (int i = (int)s.chain.size() - 1; i >= 0; --i)
		{
			OpenFile *f = s.byRef(s.chain[i]);
			if (!f) continue;
			for (size_t k = 0; k < f->items.size(); ++k)
			{
				if (f->items[k].item.type == type && f->items[k].item.name == name)
					return publish(*f, f->items[k], theHandle);
			}
			if (topOnly) break;
		}
		return SetResError(MacOS::resNotFound);
	}

	uint16_t getInd(uint32_t type, uint16_t index, uint32_t &theHandle)
	{
		theHandle = 0;
		OpenFile *f = store().current();
		if (!f || index == 0) return SetResError(MacOS::resNotFound);
		uint16_t n = 0;
		for (size_t k = 0; k < f->items.size(); ++k)
		{
			if (f->items[k].item.type != type) continue;
			if (++n == index) return publish(*f, f->items[k], theHandle);
		}
		return SetResError(MacOS::resNotFound);
	}

	namespace Native
	{

		// used by GetString (utility.h)
		// used by Loader.
		uint16_t GetResource(uint32_t type, uint16_t id, uint32_t &theHandle)
		{
			return getById(type, (int16_t)id, false, theHandle);
		}

		uint16_t SetResLoad(bool load)
		{
			ResLoad = load;
			memoryWriteByte(load ? 0xff : 0x00, MacOS::ResLoad); // word or byte?
			return SetResError(0);
		}

		uint16_t OpenResourceFile(const std::string &path);

	}

	uint16_t CloseResFile(uint16_t trap)
	{
		// PROCEDURE CloseResFile (refNum: Integer);
		uint16_t refNum;

		StackFrame<2>(refNum);

		Log("%04x CloseResFile(%04x)\n", trap, refNum);

		// If the value of the refNum parameter is 0, it represents the System file and is ignored.

		if (refNum != 0)
		{
			Store &s = store();
			OpenFile *f = s.byRef(refNum);
			if (!f) return SetResError(MacOS::resFNotFound);
			uint16_t err = 0;
			if (f->dirty) err = flushFile(*f);
			for (auto it = s.handles.begin(); it != s.handles.end(); )
			{
				if (it->second.file == refNum) it = s.handles.erase(it);
				else ++it;
			}
			s.chain.erase(std::remove(s.chain.begin(), s.chain.end(), refNum), s.chain.end());
			s.files.erase(refNum);
			return SetResError(err);
		}
		return SetResError(0);
		//return SetResError(resFNotFound);
	}


	uint16_t Get1NamedResource(uint16_t trap)
	{
		// Get1NamedResource (theType: ResType; name: Str255) : Handle;

		/*
		 * -----------
		 * +8 outHandle
		 * ------------
		 * +4 theType
		 * ------------
		 * +0 name
		 * ------------
		 *
		 */

		// nb - return address is not on the stack.

		uint32_t sp;
		uint32_t theType;
		uint32_t name;

		sp = StackFrame<8>(theType, name);

		std::string sname = ToolBox::ReadPString(name);

		Log("%04x Get1NamedResource(%08x ('%s'), %s)\n",
			trap, theType, TypeToString(theType).c_str(), sname.c_str());

		uint32_t resourceHandle;
		uint32_t d0;
		d0 = getByName(theType, sname, true, resourceHandle);

		ToolReturn<4>(sp, resourceHandle);
		return d0;
	}

	uint16_t GetNamedResource(uint16_t trap)
	{
		// FUNCTION GetNamedResource (theType: ResType; name: Str255): Handle;

		uint32_t sp;
		uint32_t theType;
		uint32_t name;

		sp = StackFrame<8>(theType, name);

		std::string sname = ToolBox::ReadPString(name);

		Log("%04x GetNamedResource(%08x ('%s'), %s)\n",
			trap, theType, TypeToString(theType).c_str(), sname.c_str());

		uint32_t resourceHandle;
		uint32_t d0;
		d0 = getByName(theType, sname, false, resourceHandle);

		ToolReturn<4>(sp, resourceHandle);
		return d0;
	}

	uint16_t GetResource(uint16_t trap)
	{
		// GetResource (theType: ResType; theID: Integer): Handle;

		/*
		 * -----------
		 * +6 outHandle
		 * ------------
		 * +2 theType
		 * ------------
		 * +0 theID
		 * ------------
		 *
		 */

		// nb - return address is not on the stack.

		uint32_t sp;
		uint32_t theType;
		uint16_t theID;

		sp = StackFrame<6>(theType, theID);

		Log("%04x GetResource(%08x ('%s'), %04x)\n",
				trap, theType, TypeToString(theType).c_str(), theID);


		uint32_t resourceHandle;
		uint32_t d0;
		d0 = getById(theType, (int16_t)theID, false, resourceHandle);

		ToolReturn<4>(sp, resourceHandle);
		return d0;
	}


	uint16_t Get1Resource(uint16_t trap)
	{
		// Get1Resource (theType: ResType; theID: Integer): Handle;

		/*
		 * -----------
		 * +6 outHandle
		 * ------------
		 * +2 theType
		 * ------------
		 * +0 theID
		 * ------------
		 *
		 */

		// nb - return address is not on the stack.

		uint32_t sp;
		uint32_t theType;
		uint16_t theID;

		sp = StackFrame<6>(theType, theID);

		Log("%04x Get1Resource(%08x ('%s'), %04x)\n", trap, theType, TypeToString(theType).c_str(), theID);


		uint32_t resourceHandle;
		uint32_t d0;
		d0 = getById(theType, (int16_t)theID, true, resourceHandle);


		ToolReturn<4>(sp, resourceHandle);
		return d0;
	}


	uint16_t ReleaseResource(uint16_t trap)
	{
		// ReleaseResource (theResource: Handle);

		/*
		 * ------------
		 * +0 theResource
		 * ------------
		 *
		 */

		uint32_t sp;
		uint32_t theResource;

		sp = StackFrame<4>(theResource);

		Log("%04x ReleaseResource(%08x)\n", trap, theResource);

		return SetResError(0);
	}

	uint16_t ResError(uint16_t trap)
	{
		uint32_t sp;

		Log("%04x ResError()\n", trap);

		sp = cpuGetAReg(7);
		ToolReturn<2>(sp, memoryReadWord(MacOS::ResErr));
		return 0;
	}

	// SetResLoad (load: BOOLEAN);
	uint16_t SetResLoad(uint16_t trap)
	{
		uint16_t load;

		StackFrame<2>(load);

		Log("%04x SetResLoad(%04x)\n", trap, load);

		return Native::SetResLoad(load);
	}





	uint16_t CurResFile(uint16_t trap)
	{

		Log("%04x CurResFile()\n", trap);

		uint16_t refNum = store().chain.empty() ? 0 : store().chain.back();

		ToolReturn<2>(-1, refNum);
		return SetResError(0);
	}

	uint16_t UseResFile(uint16_t trap)
	{
		uint16_t resFile;

		StackFrame<2>(resFile);

		Log("%04x UseResFile(%04x)\n", trap, resFile);

		if (resFile == 0 || !store().byRef(resFile))
			return SetResError(MacOS::resFNotFound);
		useFile(resFile);
		return SetResError(0);
	}


	tool_return<void> CreateResFile(const std::string &path, uint32_t creator = 0, uint32_t fileType = 0)
	{

		if (path.empty()) return MacOS::paramErr;

		int fd;

		// Creating the data fork does not create the resource fork, and the
		// reverse. An existing non-empty sidecar is a duplicate fork.

		fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
		if (fd < 0)
		{
			if (errno != EEXIST) return macos_error_from_errno();
		}
		else
		{
			if (creator || fileType)
				OS::Internal::SetFinderInfo(path, fileType, creator);

			close(fd);
		}

		std::string forkPath = MacOS::rsrcPath(path);
		struct stat st;
		if (::stat(forkPath.c_str(), &st) == 0)
			return MacOS::dupFNErr;

		std::vector<uint8_t> empty = MacOS::ResFork::emptyFork();
		fd = ::open(forkPath.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
		if (fd < 0) return macos_error_from_errno();
		ssize_t n = ::write(fd, empty.data(), empty.size());
		close(fd);
		if (n != (ssize_t)empty.size()) return MacOS::ioErr;
		return MacOS::noErr;
	}

	uint16_t CreateResFile(uint16_t trap)
	{
		// PROCEDURE CreateResFile (fileName: Str255);

		/*
		 * CreateResFile creates a resource file containing no resource
		 * data. If there's no file at all with the given name, it also
		 * creates an empty data fork for the file. If there's already a
		 * resource file with the given name (that is, a resource fork
		 * that isn't empty), CreateResFile will do nothing and the
		 * ResError function will return an appropriate Operating System
		 * result code.
		 */

		uint32_t fileName;

		StackFrame<4>(fileName);

		std::string sname = ToolBox::ReadPString(fileName, true);
		Log("%04x CreateResFile(%s)\n", trap, sname.c_str());

		if (!sname.length()) return SetResError(MacOS::paramErr);


		auto rv = CreateResFile(sname);

		return SetResError(rv.error());
	}

	uint16_t HCreateResFile(uint16_t trap)
	{
		// PROCEDURE HCreateResFile (vRefNum: Integer; dirID: LongInt;
		//                           fileName: Str255);

		uint16_t vRefNum;
		uint32_t dirID;
		uint32_t fileName;

		StackFrame<10>(vRefNum, dirID, fileName);

		std::string sname = ToolBox::ReadPString(fileName, true);

		Log("%04x HCreateResFile(%04x, %08x, %s)\n",
			trap, vRefNum, dirID, sname.c_str());


		sname = OS::FSSpecManager::ExpandPath(sname, dirID);
		if (sname.empty())
		{
			return SetResError(MacOS::dirNFErr);
		}

		auto rv = CreateResFile(sname);

		return SetResError(rv.error() == MacOS::dupFNErr ? 0 : rv.error());
	}


	uint16_t FSpCreateResFile(void)
	{

		// PROCEDURE FSpCreateResFile (spec: FSSpec; creator, fileType: OSType; scriptTag: ScriptCode);

		// creates the file, if necessary.

		uint32_t sp;
		uint32_t spec;
		uint32_t creator;
		uint32_t fileType;
		uint16_t scriptTag;

		sp = StackFrame<14>(spec, creator, fileType, scriptTag);


		int parentID = memoryReadLong(spec + 2);
		std::string sname = ToolBox::ReadPString(spec + 6, false);

		Log("     FSpCreateResFile(%s, %08x ('%s'), %08x ('%s'), %02x)\n",
			sname.c_str(),
			creator, ToolBox::TypeToString(creator).c_str(),
			fileType, ToolBox::TypeToString(fileType).c_str(),
			scriptTag);


		sname = OS::FSSpecManager::ExpandPath(sname, parentID);
		if (sname.empty())
		{
			return SetResError(MacOS::dirNFErr);
		}

		auto rv = CreateResFile(sname, creator, fileType);

		return SetResError(rv.error() == MacOS::dupFNErr ? 0 : rv.error());
	}




	tool_return<int16_t> OpenResCommon(const std::string &path, uint16_t permission = 0)
	{
		(void)permission;
		if (path.empty()) return MacOS::paramErr;
		if (::access(path.c_str(), F_OK) != 0) return macos_error_from_errno();

		std::string canon = canonPath(path);
		Store &s = store();
		for (auto it = s.files.begin(); it != s.files.end(); ++it)
		{
			if (it->second.path == canon)
			{
				useFile(it->first);
				return (int16_t)it->first;
			}
		}

		std::string forkPath = MacOS::rsrcPath(canon);
		// FSOpenResourceFile reports eofErr when the data file exists and the
		// resource fork does not. PascalIIGS treats that as "create it" and
		// any other error as fatal.
		if (::access(forkPath.c_str(), F_OK) != 0) return MacOS::eofErr;

		MacOS::ResFork fork;
		std::string error;
		if (!fork.read(forkPath, error)) return MacOS::mapReadErr;

		OpenFile f;
		f.path = canon;
		f.forkPath = forkPath;
		f.ref = s.nextRef++;
		if (f.ref == 0) f.ref = s.nextRef++;
		f.attrs = fork.fileAttrs;
		f.dirty = false;
		f.items.reserve(fork.items.size());
		for (size_t i = 0; i < fork.items.size(); ++i)
		{
			OpenItem item;
			item.item = fork.items[i];
			item.handle = 0;
			f.items.push_back(item);
		}
		uint16_t ref = f.ref;
		s.files.insert(std::make_pair(ref, f));
		s.chain.push_back(ref);
		return (int16_t)ref;
	}

	uint16_t Native::OpenResourceFile(const std::string &path)
	{
		return OpenResCommon(path).error();
	}

	uint16_t OpenResFile(uint16_t trap)
	{
		// OpenResFile (fileName: Str255) : INTEGER;

		uint32_t sp;
		uint32_t fileName;

		sp = StackFrame<4>(fileName);

		std::string sname = ToolBox::ReadPString(fileName, true);

		Log("%04x OpenResFile(%s)\n", trap, sname.c_str());

		auto rv = OpenResCommon(sname);

		ToolReturn<2>(sp, rv.value_or(-1));

		return SetResError(rv.error());
	}

	uint16_t HOpenResFile(uint16_t trap)
	{
		// FUNCTION HOpenResFile (vRefNum: Integer; dirID: LongInt;
		// fileName: Str255; permission: SignedByte): Integer;

		uint32_t sp;

		uint16_t vRefNum;
		uint32_t dirID;
		uint32_t fileName;
		uint8_t permission;

		sp = StackFrame<12>(vRefNum, dirID, fileName, permission);

		std::string sname = ToolBox::ReadPString(fileName, true);

		Log("%04x HOpenResFile(%04x, %08x, %s, %04x)\n",
			trap, vRefNum, dirID, sname.c_str(), permission);

		if (vRefNum) {
			fprintf(stderr, "HOpenResFile: vRefNum not supported yet.\n");
			exit(1);
		}

		sname = OS::FSSpecManager::ExpandPath(sname, dirID);
		if (sname.empty())
		{
			ToolReturn<2>(sp, (uint16_t)-1);
			return SetResError(MacOS::dirNFErr);
		}

		auto rv = OpenResCommon(sname, permission);

		ToolReturn<2>(sp, rv.value_or(-1));

		return SetResError(rv.error());
	}

	uint16_t FSpOpenResFile(void)
	{
		// FUNCTION FSpOpenResFile (spec: FSSpec; permission: SignedByte): Integer;

		uint32_t sp;
		uint32_t spec;
		uint8_t permission;

		sp = StackFrame<6>(spec, permission);


		int parentID = memoryReadLong(spec + 2);

		std::string sname = ToolBox::ReadPString(spec + 6, false);

		Log("     FSpOpenResFile(%s, %04x)\n",  sname.c_str(), permission);


		sname = OS::FSSpecManager::ExpandPath(sname, parentID);
		if (sname.empty())
		{
			ToolReturn<2>(sp, (uint16_t)-1);
			return SetResError(MacOS::dirNFErr);
		}

		auto rv = OpenResCommon(sname, permission);

		ToolReturn<2>(sp, rv.value_or(-1));

		return SetResError(rv.error());
	}



	uint16_t OpenRFPerm(uint16_t trap)
	{
		// FUNCTION OpenRFPerm (fileName: Str255; vRefNum: Integer;
        //           permission: SignedByte): Integer;
		uint32_t sp;
		uint32_t fileName;
		uint16_t vRefNum;
		uint16_t permission;

		sp = StackFrame<8>(fileName, vRefNum, permission);

		std::string sname = ToolBox::ReadPString(fileName, true);
		Log("%04x OpenRFPerm(%s, %04x, %04x)\n",
			trap, sname.c_str(), vRefNum, permission);

		auto rv = OpenResCommon(sname, permission);
		ToolReturn<2>(sp, rv.value_or(-1));
		return SetResError(rv.error());
	}

	uint16_t Count1Resources(uint16_t trap)
	{
		// FUNCTION Count1Resources (theType: ResType): Integer;

		uint32_t sp;
		uint32_t theType;
		uint16_t count;

		sp = StackFrame<4>(theType);

		Log("%04x Count1Resources(%08x ('%s'))\n",
			trap, theType, TypeToString(theType).c_str());

		count = 0;
		OpenFile *f = store().current();
		if (f)
		{
			for (size_t i = 0; i < f->items.size(); ++i)
				if (f->items[i].item.type == theType) count++;
		}

		ToolReturn<2>(sp, count);
		return SetResError(0);
	}


	uint16_t UpdateResFile(uint16_t trap)
	{
		// PROCEDURE UpdateResFile (refNum: Integer);

		uint16_t refNum;

		StackFrame<2>(refNum);

		Log("%04x UpdateResFile(%04x)\n", trap, refNum);

		OpenFile *f = refNum == 0 ? store().current() : store().byRef(refNum);
		if (!f) return SetResError(MacOS::resFNotFound);
		if (!f->dirty) return SetResError(0);
		return SetResError(flushFile(*f));
	}


	uint16_t ChangedResource(uint16_t trap)
	{
		// PROCEDURE ChangedResource (theResource: Handle);

		uint32_t theResource;

		StackFrame<4>(theResource);

		Log("%04x ChangedResource(%08x)\n", trap, theResource);


		// set the resChanged attribute so when UpdateResFile() is called
		// (or the app exits)
		// also needs to update the carbon handle/data with the mpw handle
		// since the data has changed....


		// Write through. The next unit reads the cache this unit just stored,
		// and nothing in the emulator flushes resource files at process exit.
		return storeHandle(theResource, true);
	}





	uint16_t GetResFileAttrs(uint16_t trap)
	{
		// FUNCTION GetResFileAttrs (refNum: Integer): Integer;
		uint32_t sp;
		uint16_t attrs;
		uint16_t refNum;

		sp = StackFrame<2>(refNum);
		Log("%04x GetResFileAttrs(%04x)\n", trap, refNum);

		OpenFile *f = store().byRef(refNum);
		if (!f)
		{
			ToolReturn<2>(sp, 0);
			return SetResError(MacOS::resFNotFound);
		}
		attrs = f->attrs;
		ToolReturn<2>(sp, attrs);

		return SetResError(0);
	}


	uint16_t SetResFileAttrs(uint16_t trap)
	{
		// PROCEDURE SetResFileAttrs  (refNum: Integer; attrs: Integer);
		uint32_t sp;
		uint16_t attrs;
		uint16_t refNum;

		sp = StackFrame<4>(refNum, attrs);
		Log("%04x GetResFileAttrs(%04x, %04x)\n", trap, refNum, attrs);

		OpenFile *f = store().byRef(refNum);
		if (!f) return SetResError(MacOS::resFNotFound);
		f->attrs = attrs;
		f->dirty = true;
		return SetResError(flushFile(*f));
	}

	// TODO -- update rhandle_map when loading resources
	// TODO -- update rhandle_map when disposing resources.

	uint16_t AddResource(uint16_t trap)
	{
		// PROCEDURE AddResource (theData: Handle; theType: ResType;
		// theID: Integer; name: Str255);

		uint32_t theData;
		uint32_t theType;
		uint16_t theID;
		uint32_t namePtr;


		StackFrame<14>(theData, theType, theID, namePtr);

		std::string sname = ToolBox::ReadPString(namePtr, false);

		Log("%04x AddResource(%08x, %08x ('%s'), %04x, %s)\n",
			trap, theData, theType, TypeToString(theType).c_str(), theID, sname.c_str()
		);

		OpenFile *f = store().current();
		if (!f || !theData) return SetResError(MacOS::addResFailed);
		if (findItem(*f, theType, (int16_t)theID)) return SetResError(MacOS::addResFailed);
		if (store().handles.find(theData) != store().handles.end())
			return SetResError(MacOS::addResFailed);

		auto info = MM::GetHandleInfo(theData);
		if (info.error()) return SetResError(MacOS::addResFailed);

		OpenItem item;
		item.item.type = theType;
		item.item.id = (int16_t)theID;
		item.item.attrs = 0x02;
		item.item.name = sname;
		item.item.data.resize(info->size);
		if (info->size)
			std::memcpy(item.item.data.data(), memoryPointer(info->address), info->size);
		item.handle = theData;
		f->items.push_back(item);
		store().handles[theData] = ResKey{f->ref, theType, (int16_t)theID};
		f->dirty = true;
		return SetResError(flushFile(*f));
	}

	uint16_t SetResAttrs(uint16_t trap)
	{
		// PROCEDURE SetResAttrs (theResource: Handle; attrs: Integer);

		uint32_t theResource;
		uint16_t attrs;

		StackFrame<6>(theResource, attrs);

		Log("%04x SetResAttrs(%08x, %04x)\n", trap, theResource, attrs);

		// find the native handle..

		auto hit = store().handles.find(theResource);
		if (hit == store().handles.end()) return SetResError(MacOS::resNotFound);
		OpenFile *f = store().byRef(hit->second.file);
		OpenItem *it = f ? findItem(*f, hit->second.type, hit->second.id) : nullptr;
		if (!it) return SetResError(MacOS::resNotFound);
		it->item.attrs = (uint8_t)attrs;
		f->dirty = true;
		return SetResError(flushFile(*f));
	}

	uint16_t GetResAttrs(uint16_t trap)
	{
		// FUNCTION GetResAttrs (theResource: Handle): Integer;

		uint32_t sp;
		uint32_t theResource;
		uint16_t attrs;

		sp = StackFrame<4>(theResource);

		Log("%04x GetResAttrs(%08x)\n", trap, theResource);

		auto hit = store().handles.find(theResource);
		if (hit == store().handles.end())
		{
			ToolReturn<2>(sp, 0);
			return SetResError(MacOS::resNotFound);
		}
		OpenFile *f = store().byRef(hit->second.file);
		OpenItem *it = f ? findItem(*f, hit->second.type, hit->second.id) : nullptr;
		if (!it)
		{
			ToolReturn<2>(sp, 0);
			return SetResError(MacOS::resNotFound);
		}
		attrs = it->item.attrs;

		ToolReturn<2>(sp, attrs);

		return SetResError(0);
	}


	uint16_t WriteResource(uint16_t trap)
	{
		// PROCEDURE WriteResource (theResource: Handle);

		uint32_t theResource;
		StackFrame<4>(theResource);

		Log("%04x WriteResource(%08x)\n", trap, theResource);


		return storeHandle(theResource, true);
	}



	uint16_t DetachResource(uint16_t trap)
	{
		// PROCEDURE DetachResource (theResource: Handle);

		uint32_t theResource;
		StackFrame<4>(theResource);

		Log("%04x DetachResource(%08x)\n", trap, theResource);


		auto hit = store().handles.find(theResource);
		if (hit == store().handles.end()) return SetResError(MacOS::resNotFound);
		OpenFile *f = store().byRef(hit->second.file);
		OpenItem *it = f ? findItem(*f, hit->second.type, hit->second.id) : nullptr;
		if (it) it->handle = 0;
		store().handles.erase(hit);
		return SetResError(0);
	}


	uint16_t Get1IndResource(uint16_t trap)
	{
		// FUNCTION Get1IndResource (theType: ResType; index: Integer): Handle;

		uint32_t sp;
		uint32_t theType;
		uint16_t index;

		sp = StackFrame<6>(theType, index);
		Log("%04x Get1IndResource(%08x ('%s'), %04x)\n",
			trap, theType, TypeToString(theType).c_str(), index);

		uint32_t resourceHandle = 0;
		uint16_t d0;
		d0 = getInd(theType, index, resourceHandle);

		ToolReturn<4>(sp, resourceHandle);
		return d0;
	}


	uint16_t RemoveResource(uint16_t trap)
	{
		// PROCEDURE RemoveResource (theResource: Handle);

		uint32_t theResource;

		StackFrame<4>(theResource);

		Log("%04x RemoveResource(%08x)\n", trap, theResource);


		auto hit = store().handles.find(theResource);
		if (hit == store().handles.end()) return SetResError(MacOS::resNotFound);
		ResKey key = hit->second;
		OpenFile *f = store().byRef(key.file);
		if (!f) return SetResError(MacOS::resNotFound);
		std::vector<OpenItem> &items = f->items;
		items.erase(std::remove_if(items.begin(), items.end(),
			[&](const OpenItem &it) {
				return it.item.type == key.type && it.item.id == key.id;
			}), items.end());
		store().handles.erase(hit);
		f->dirty = true;
		return SetResError(flushFile(*f));
	}

	uint16_t GetResourceSizeOnDisk(uint16_t trap)
	{
		// FUNCTION GetResourceSizeOnDisk (theResource: Handle): LongInt;

		uint32_t sp;
		uint32_t theResource;

		sp = StackFrame<4>(theResource);

		Log("%04x GetResourceSizeOnDisk(%08x)\n", trap, theResource);


		auto hit = store().handles.find(theResource);
		if (hit == store().handles.end())
		{
			ToolReturn<4>(sp, (uint32_t)0);
			return SetResError(MacOS::resNotFound);
		}
		OpenFile *f = store().byRef(hit->second.file);
		OpenItem *it = f ? findItem(*f, hit->second.type, hit->second.id) : nullptr;
		if (!it)
		{
			ToolReturn<4>(sp, (uint32_t)0);
			return SetResError(MacOS::resNotFound);
		}

		ToolReturn<4>(sp, (uint32_t)it->item.data.size());
		return SetResError(0);
	}

	uint16_t GetResInfo(uint16_t trap)
	{
		// PROCEDURE GetResInfo (theResource: Handle;
		// VAR theID: INTEGER; VAR theType: ResType; VAR name: Str255);

		uint32_t theResource;
		uint32_t theID;
		uint32_t theType;
		uint32_t name;
		StackFrame<16>(theResource, theID, theType, name);

		Log("%04x GetResInfo(%08x)\n", trap, theResource);

		auto hit = store().handles.find(theResource);
		if (hit == store().handles.end())
		{
			return SetResError(MacOS::resNotFound);
		}
		OpenFile *f = store().byRef(hit->second.file);
		OpenItem *it = f ? findItem(*f, hit->second.type, hit->second.id) : nullptr;
		if (!it) return SetResError(MacOS::resNotFound);

		if (theID) memoryWriteWord((uint16_t)it->item.id, theID);
		if (theType) memoryWriteLong(it->item.type, theType);
		if (name) ToolBox::WritePString(name, it->item.name);

		return SetResError(0);
	}

	uint16_t LoadResource(uint16_t trap)
	{
		// PROCEDURE LoadResource (theResource: Handle);

		// this loads the resource from disk, if not already
		// loaded. (if purgeable or SetResLoad(false))

		// this needs cooperation with MM to check if
		// handle was purged.

		uint32_t theResource;

		StackFrame<4>(theResource);

		Log("%04x LoadResource(%08x)\n", trap, theResource);

		auto hit = store().handles.find(theResource);
		if (hit == store().handles.end())
		{
			return SetResError(MacOS::resNotFound);
		}
		OpenFile *f = store().byRef(hit->second.file);
		OpenItem *it = f ? findItem(*f, hit->second.type, hit->second.id) : nullptr;
		if (!it) return SetResError(MacOS::resNotFound);

		// if it has a size, it's loaded...
		auto info = MM::GetHandleInfo(theResource);
		if (info.error()) return SetResError(MacOS::resNotFound);
		if (info->size) return SetResError(0);

		uint32_t size = (uint32_t)it->item.data.size();
		uint16_t err = MM::Native::ReallocHandle(theResource, size);
		if (err) return SetResError(err);

		if (size)
		{
			info = MM::GetHandleInfo(theResource);
			if (info.error()) return SetResError(info.error());
			std::memcpy(memoryPointer(info->address), it->item.data.data(), size);
		}

		return SetResError(0);
	}


	uint16_t HomeResFile(uint16_t trap)
	{
		// PPCAsm

		// FUNCTION HomeResFile (theResource: Handle): Integer;

		uint32_t sp;
		uint32_t theResource;
		uint16_t resFile;

		sp = StackFrame<4>(theResource);
		Log("%04x HomeResFile(%08x)\n", trap, theResource);


		auto hit = store().handles.find(theResource);
		if (hit == store().handles.end())
		{
			ToolReturn<2>(sp, -1);
			return SetResError(MacOS::resNotFound);
		}

		resFile = hit->second.file;

		ToolReturn<2>(sp, resFile);

		return SetResError(0);
	}

	uint16_t Count1Types(uint16_t trap)
	{
		// FUNCTION Count1Types: Integer;

		uint16_t count;

		Log("%04x Count1Types\n", trap);

		count = 0;
		OpenFile *f = store().current();
		if (f)
		{
			std::vector<uint32_t> seen;
			for (size_t i = 0; i < f->items.size(); ++i)
			{
				uint32_t type = f->items[i].item.type;
				bool found = false;
				for (size_t k = 0; k < seen.size(); ++k)
					if (seen[k] == type) found = true;
				if (!found)
				{
					seen.push_back(type);
					count++;
				}
			}
		}

		ToolReturn<2>(-1, count);

		return SetResError(0);
	}


	uint16_t Get1IndType(uint16_t trap)
	{
		// PROCEDURE Get1IndType (VAR theType: ResType; index: Integer);

		uint32_t theType;
		uint16_t index;

		StackFrame<6>(theType, index);

		Log("%04x Get1IndType(%08x, %04x)\n", trap, theType, index);

		uint32_t nativeType = 0;
		OpenFile *f = store().current();
		if (f && index != 0)
		{
			std::vector<uint32_t> seen;
			for (size_t i = 0; i < f->items.size(); ++i)
			{
				uint32_t type = f->items[i].item.type;
				bool found = false;
				for (size_t k = 0; k < seen.size(); ++k)
					if (seen[k] == type) found = true;
				if (!found) seen.push_back(type);
			}
			if (index <= seen.size()) nativeType = seen[index - 1];
		}

		memoryWriteLong(nativeType, theType);

		return SetResError(nativeType ? 0 : MacOS::resNotFound);
	}
}
