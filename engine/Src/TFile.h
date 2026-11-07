// MergeDesk modifications: Copyright (C) 2026 davoda1. Modified 2026-11-06.
#pragma once

#define POCO_NO_UNWINDOWS 1
#include <Poco/File.h>
#include <Poco/Exception.h>
#include <Poco/UnicodeConverter.h>
#include "UnicodeString.h"
#include "unicoder.h"

class TFile : public Poco::File
{
public:
	explicit TFile(const String& path) : File(ucr::toUTF8(path)) {}
#ifdef _WIN32
	// Poco exposes the UTF-8 path. Convert explicitly at Windows API boundaries.
	std::wstring wpath() const
	{
		std::wstring result;
		Poco::UnicodeConverter::toUTF16(path(), result);
		return result;
	}
#else
	const std::string& wpath() const { return path(); }
#endif
	void copyTo(const String& path) const { File::copyTo(ucr::toUTF8(path)); }
	void moveTo(const String& path) { File::moveTo(ucr::toUTF8(path)); }
	void renameTo(const String& path) { File::renameTo(ucr::toUTF8(path)); }
};
