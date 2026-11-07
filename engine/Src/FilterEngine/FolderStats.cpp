// MergeDesk modifications: Copyright (C) 2026 davoda1. Modified 2026-11-06.
/**
 * @file  FolderStats.cpp
 * @brief Implementation file for FolderStats functions
 */
#include "pch.h"
#include "FolderStats.h"
#include "TFile.h"
#include "unicoder.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif
#include <string>
#include <cstdint>

namespace FolderStats
{
	FolderStatsResult ScanFolder(const String& path, bool recursive)
	{
		FolderStatsResult result;
#ifndef _WIN32
		// POSIX implementation: readdir + stat
		DIR *dp = opendir(path.c_str());
		if (dp == nullptr)
			return result;
		struct dirent *de;
		while ((de = readdir(dp)) != nullptr)
		{
			const String name = de->d_name;
			if (name == _T(".") || name == _T(".."))
				continue;
			const String child = path + _T("/") + name;
			struct stat st;
			if (stat(child.c_str(), &st) != 0)
				continue;
			if (S_ISDIR(st.st_mode))
			{
				result.itemCount++;
				if (recursive)
				{
					FolderStatsResult sub = ScanFolder(child, recursive);
					result.fileCount += sub.fileCount;
					result.itemCount += sub.itemCount;
					result.totalSize += sub.totalSize;
				}
			}
			else
			{
				result.fileCount++;
				result.itemCount++;
				result.totalSize += static_cast<int64_t>(st.st_size);
			}
		}
		closedir(dp);
		return result;
#else
		WIN32_FIND_DATAW findData;
		HANDLE hFind = FindFirstFileW(TFile(path + _T("\\*")).wpath().c_str(), &findData);
		if (hFind == INVALID_HANDLE_VALUE)
			return result;
		do
		{
			const String name = ucr::toTString(std::wstring(findData.cFileName));
			if (name == _T(".") || name == _T(".."))
				continue;
			const bool isDir = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			if (isDir)
			{
				result.itemCount++;
				if (recursive)
				{
					FolderStatsResult sub = ScanFolder(path + _T("\\") + name, recursive);
					result.fileCount += sub.fileCount;
					result.itemCount += sub.itemCount;
					result.totalSize += sub.totalSize;
				}
			}
			else
			{
				result.fileCount++;
				result.itemCount++;
				result.totalSize += (static_cast<int64_t>(findData.nFileSizeHigh) << 32)
					+ static_cast<int64_t>(findData.nFileSizeLow);
			}
		} while (FindNextFileW(hFind, &findData));
		FindClose(hFind);
		return result;
#endif
	}
}
