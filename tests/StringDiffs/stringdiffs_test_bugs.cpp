// MergeDesk modifications: Copyright (C) 2026 davoda1. Modified 2026-11-06.
#include "pch.h"
#include <gtest/gtest.h>
#include <vector>
#include "stringdiffs.h"


using std::vector;

namespace
{
	// The fixture for testing String differencing functions.
	// These tests are from actually reported bugs.
	class StringDiffsBugsTest : public testing::Test
	{
	protected:
		// You can remove any or all of the following functions if its body
		// is	empty.

		StringDiffsBugsTest()
		{
			// You can do set-up work for each test	here.
			strdiff::Init();
		}

		virtual ~StringDiffsBugsTest()
		{
			// You can do clean-up work	that doesn't throw exceptions here.
			strdiff::Close();
		}

		// If	the	constructor	and	destructor are not enough for setting up
		// and cleaning up each test, you can define the following methods:

		virtual void SetUp()
		{
			// Code	here will be called	immediately	after the constructor (right
			// before each test).
		}

		virtual void TearDown()
		{
			// Code	here will be called	immediately	after each test	(right
			// before the destructor).
		}

		// Objects declared here can be used by all tests in the test case for Foo.
	};

	// strdiff::ComputeWordDiffs() parameters are:
	// String & str1 - the first string to compare
	// String & str2 - the second string to compare
	// bool case_sensitive - is the compare case-sensitive?
	// int whitespace - do we ignore whitespace and how
	// int breakType - Space (0) or punctuations (1) break
	// bool byte_level - are we word (false) or byte-level (true) diffing
	// std::vector<strdiff::wdiff> * pDiffs - resultting diff list


	// UTF-8 character classification must preserve word ranges, including
	// cased supplementary characters and non-ASCII decimal digits.
	TEST_F(StringDiffsBugsTest, Utf8AccentedWordsKeepWholeWordRanges)
	{
		const String words[] = { _T("café"), _T("ÉTÉ"), _T("árvíztűrő"), _T("Ωμέγα"), _T("x١"), _T("x𐐀𐐨") };
		for (const String &word : words)
		{
			for (int breakType : {0, 1})
			{
				SCOPED_TRACE(word);
				SCOPED_TRACE(breakType);
				const String left = _T("keep ") + word + _T("A tail");
				const String right = _T("keep ") + word + _T("B tail");
				const auto diffs = strdiff::ComputeWordDiffs(left, right, true, strdiff::EOL_STRICT, 0, false, breakType, false);
				ASSERT_EQ(1u, diffs.size());
				for (int side : {0, 1})
				{
					EXPECT_EQ(5, diffs[0].begin[side]);
					EXPECT_EQ(5 + static_cast<int>(word.size()), diffs[0].end[side]);
				}
			}
		}
	}

	TEST_F(StringDiffsBugsTest, Utf8PunctuationAndAccentsHaveCompleteCharacterRanges)
	{
		for (const String &separator : {String(_T("，")), String(_T("—")), String(_T("。")), String(_T("😀")), String(_T("漢"))})
		{
			SCOPED_TRACE(separator);
			const String prefix = _T("same") + separator;
			for (int breakType : {0, 1})
			{
				const auto diffs = strdiff::ComputeWordDiffs(prefix + _T("cafe tail"), prefix + _T("caff tail"),
					true, strdiff::EOL_STRICT, 0, false, breakType, false);
				ASSERT_EQ(1u, diffs.size());
				for (int side : {0, 1})
				{
					EXPECT_EQ(static_cast<int>(prefix.size()), diffs[0].begin[side]);
					EXPECT_EQ(static_cast<int>(prefix.size()) + 3, diffs[0].end[side]);
				}
			}
		}
		const String left = _T("pre café, post");
		const String right = _T("pre cafè, post");
		for (bool byteLevel : {false, true})
		{
			const auto diffs = strdiff::ComputeWordDiffs(left, right, true, strdiff::EOL_STRICT, 0, false, 1, byteLevel);
			ASSERT_EQ(1u, diffs.size());
			for (int side : {0, 1})
			{
				EXPECT_EQ(byteLevel ? 7 : 4, diffs[0].begin[side]);
				EXPECT_EQ(6 + static_cast<int>(String(_T("é")).size()), diffs[0].end[side]);
			}
		}
	}

	TEST_F(StringDiffsBugsTest, Utf8Latin1BreakCharactersRespectConfiguration)
	{
		for (const String &middle : {String(_T("·")), String(_T("©"))})
		{
			const String left = _T("a") + middle + _T("b");
			const String right = _T("a") + middle + _T("c");
			for (int breakType : {0, 1})
			{
				strdiff::SetBreakChars(_T(",.;:"));
				auto diffs = strdiff::ComputeWordDiffs(left, right, true, strdiff::EOL_STRICT, 0, false, breakType, false);
				ASSERT_EQ(1u, diffs.size());
				EXPECT_EQ(0, diffs[0].begin[0]);
				EXPECT_EQ(0, diffs[0].begin[1]);
				strdiff::SetBreakChars(_T("·"));
				diffs = strdiff::ComputeWordDiffs(left, right, true, strdiff::EOL_STRICT, 0, false, breakType, false);
				ASSERT_EQ(1u, diffs.size());
				const int start = breakType == 1 && middle == _T("·") ? 1 + static_cast<int>(middle.size()) : 0;
				for (int side : {0, 1})
				{
					EXPECT_EQ(start, diffs[0].begin[side]);
					EXPECT_EQ(static_cast<int>(left.size()) - 1, diffs[0].end[side]);
				}
			}
		}
	}

#ifndef _UNICODE
	TEST_F(StringDiffsBugsTest, Utf8TruncatedInputHasBoundedRanges)
	{
		for (const String &suffix : {String("\xc3"), String("\xe2\x80"), String("\xf0\x9f\x98"), String("\x80")})
		{
			const String left = "same " + suffix;
			const String right = "same !";
			const auto diffs = strdiff::ComputeWordDiffs(left, right, true, strdiff::EOL_STRICT, 0, false, 1, false);
			ASSERT_EQ(1u, diffs.size());
			EXPECT_EQ(5, diffs[0].begin[0]);
			EXPECT_EQ(static_cast<int>(left.size()) - 1, diffs[0].end[0]);
			EXPECT_EQ(5, diffs[0].begin[1]);
			EXPECT_EQ(5, diffs[0].end[1]);
		}
	}
#endif

	// Sf.net bug #1685466 - char level diff
	// (ENUMRESLANGPROC) added to middle
	TEST_F(StringDiffsBugsTest, Bug_1685466_1)
	{
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  01234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789
			_T("if (EnumResourceLanguages(hinstLang, RT_VERSION, MAKEINTRESOURCE(VS_VERSION_INFO), (ENUMRESLANGPROC)FindNextResLang, (LPARAM)&wLangID) == 0)"),
			_T("if (EnumResourceLanguages(hinstLang, RT_VERSION, MAKEINTRESOURCE(VS_VERSION_INFO), FindNextResLang, (LPARAM)&wLangID) == 0)"),
			false, strdiff::EOL_STRICT, 0, false, 1, true);
		EXPECT_EQ(1, diffs.size());
		if (diffs.size() > 0)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(83, pDiff->begin[0]);
			EXPECT_EQ(83, pDiff->begin[1]);
			EXPECT_EQ(99, pDiff->end[0]);
			EXPECT_EQ(82, pDiff->end[1]);
		}
	}

	// Sf.net bug #1685466 - word level diff
	// (ENUMRESLANGPROC) added to middle
	TEST_F(StringDiffsBugsTest, Bug_1685466_2)
	{
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  01234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789
			_T("if (EnumResourceLanguages(hinstLang, RT_VERSION, MAKEINTRESOURCE(VS_VERSION_INFO), (ENUMRESLANGPROC)FindNextResLang, (LPARAM)&wLangID) == 0)"),
			_T("if (EnumResourceLanguages(hinstLang, RT_VERSION, MAKEINTRESOURCE(VS_VERSION_INFO), FindNextResLang, (LPARAM)&wLangID) == 0)"),
			false, strdiff::EOL_STRICT, 0, false, 1, false);
		EXPECT_EQ(1, diffs.size());
		if (diffs.size() > 0)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(83, pDiff->begin[0]);
			EXPECT_EQ(83, pDiff->begin[1]);
			EXPECT_EQ(114, pDiff->end[0]);
			EXPECT_EQ(97, pDiff->end[1]);
		}
	}

	// Sf.net bug #1939279 - char level diff
	// various changes
	TEST_F(StringDiffsBugsTest, Bug_1939279_1)
	{
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  01234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789
			_T("[overlay_oid_origin, overlay_oid_target], [nil, nil]"),
			_T("[overlay_oid_origin, overlay_oid_target, origin_file_name, target_file_name], [nil, nil, \"origin.txt\"), \"target.txt\"]"),
			false, strdiff::EOL_STRICT, 0, false, 1, true);
		EXPECT_EQ(3, diffs.size());
		if (diffs.size() > 0)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(39, pDiff->begin[0]);
			EXPECT_EQ(39, pDiff->begin[1]);
			EXPECT_EQ(39, pDiff->end[0]);
			EXPECT_EQ(38, pDiff->end[1]);
		}
	}

	// Sf.net bug #1939279 - word level diff
	// various changes
	TEST_F(StringDiffsBugsTest, Bug_1939279_2)
	{
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  01234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789
			_T("[overlay_oid_origin, overlay_oid_target], [nil, nil]"),
			_T("[overlay_oid_origin, overlay_oid_target, origin_file_name, target_file_name], [nil, nil, \"origin.txt\"), \"target.txt\"]"),
			false, strdiff::EOL_STRICT, 0, false, 1, false);
		EXPECT_EQ(3, diffs.size());
		if (diffs.size() > 0)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(21, pDiff->begin[0]);
			EXPECT_EQ(21, pDiff->begin[1]);
			EXPECT_EQ(39, pDiff->end[0]);
			EXPECT_EQ(38, pDiff->end[1]);
		}
	}

	// Sf.net bug #2022935 - char level diff
	// Chars added to end
	TEST_F(StringDiffsBugsTest, Bug_2022935)
	{
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  012345678901234567890
			_T("N42=Import"), 
			_T("N42=Importuj"),
			false, strdiff::EOL_STRICT, 0, false, 1, true);
		EXPECT_EQ(1, diffs.size());
		if (diffs.size() > 0)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(10, pDiff->begin[0]);
			EXPECT_EQ(10, pDiff->begin[1]);
			EXPECT_EQ(9, pDiff->end[0]);
			EXPECT_EQ(11, pDiff->end[1]);
		}
	}

	// Sf.net bug #2638045 - word diff
	// Char added to end
	TEST_F(StringDiffsBugsTest, Bug_2638045)
	{
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  01234567890123456789012345678901234567890123456789
			_T("LIB_PHP4_DIR=$(RPM_NAME)-$(RPM_VER)/usr/lib/php"),
			_T("LIB_PHP4_DIR=$(RPM_NAME)-$(RPM_VER)/usr/lib/php4"),
			false, strdiff::EOL_STRICT, 0, false, 0, false);
		EXPECT_EQ(1, diffs.size());
		if (diffs.size() > 0)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(0, pDiff->begin[0]);
			EXPECT_EQ(0, pDiff->begin[1]);
			EXPECT_EQ(46, pDiff->end[0]);
			EXPECT_EQ(47, pDiff->end[1]);
		}
	}

	// Sf.net bug #2638045 - word diff with puncts
	// Char added to end
	TEST_F(StringDiffsBugsTest, Bug_2638045_2)
	{
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  01234567890123456789012345678901234567890123456789
			_T("LIB_PHP4_DIR=$(RPM_NAME)-$(RPM_VER)/usr/lib/php"),
			_T("LIB_PHP4_DIR=$(RPM_NAME)-$(RPM_VER)/usr/lib/php4"),
			false, strdiff::EOL_STRICT, 0, false, 1, false);
		EXPECT_EQ(1, diffs.size());
		if (diffs.size() > 0)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(0, pDiff->begin[0]);
			EXPECT_EQ(0, pDiff->begin[1]);
			EXPECT_EQ(46, pDiff->end[0]);
			EXPECT_EQ(47, pDiff->end[1]);
		}
	}

	// Sf.net bug #2638045 - char diff
	// Char added to end
	TEST_F(StringDiffsBugsTest, Bug_2638045_3)
	{
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  01234567890123456789012345678901234567890123456789
			_T("LIB_PHP4_DIR=$(RPM_NAME)-$(RPM_VER)/usr/lib/php"),
			_T("LIB_PHP4_DIR=$(RPM_NAME)-$(RPM_VER)/usr/lib/php4"),
			false, strdiff::EOL_STRICT, 0, false, 0, true);
		EXPECT_EQ(1, diffs.size());
		if (diffs.size() > 0)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(47, pDiff->begin[0]);
			EXPECT_EQ(47, pDiff->begin[1]);
			EXPECT_EQ(46, pDiff->end[0]);
			EXPECT_EQ(47, pDiff->end[1]);
		}
	}

	// Sf.net bug #2638045 - custom punct word diff
	// Char added to end
	TEST_F(StringDiffsBugsTest, Bug_2638045_4)
	{
		strdiff::SetBreakChars(_T("/"));
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  01234567890123456789012345678901234567890123456789
			_T("LIB_PHP4_DIR=$(RPM_NAME)-$(RPM_VER)/usr/lib/php"),
			_T("LIB_PHP4_DIR=$(RPM_NAME)-$(RPM_VER)/usr/lib/php4"),
			false, strdiff::EOL_STRICT, 0, false, 1, false);
		EXPECT_EQ(1, diffs.size());
		if (diffs.size() > 0)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(44, pDiff->begin[0]);
			EXPECT_EQ(44, pDiff->begin[1]);
			EXPECT_EQ(46, pDiff->end[0]);
			EXPECT_EQ(47, pDiff->end[1]);
		}
	}

	// Sf.net bug #2779834
	// (int) added to right-side
	// word-diff
	TEST_F(StringDiffsBugsTest, Bug_2779834_1)
	{
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  01234567890123456789012345678901234567890123456789
			_T("if (nDiff < m_diffs.size())"),
			_T("if(nDiff < (int) m_diffs.size())"),
			false, strdiff::EOL_STRICT, 0, false, 0, false);
		EXPECT_EQ(3, diffs.size());
		if (diffs.size() > 0)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(0, pDiff->begin[0]);
			EXPECT_EQ(0, pDiff->begin[1]);
			EXPECT_EQ(1, pDiff->end[0]);
			EXPECT_EQ(7, pDiff->end[1]);
		}
	}

	// Sf.net bug #2779834
	// (int) added to right-side
	// char-diff
	TEST_F(StringDiffsBugsTest, Bug_2779834_2)
	{
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  01234567890123456789012345678901234567890123456789
			_T("if (nDiff < m_diffs.size())"),
			_T("if(nDiff < (int) m_diffs.size())"),
			false, strdiff::EOL_STRICT, 0, false, 0, true);
		EXPECT_EQ(3, diffs.size());
		if (diffs.size() > 0)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(2, pDiff->begin[0]);
			EXPECT_EQ(2, pDiff->begin[1]);
			EXPECT_EQ(1, pDiff->end[0]);
			EXPECT_EQ(7, pDiff->end[1]);
		}
	}

	// Sf.net bug #2783185
	// 'private' added to right-side
	// char-diff
	TEST_F(StringDiffsBugsTest, Bug_2783185_1)
	{
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  01234567890123456789012345678901234567890123456789
			_T("const string ManualFolder = \"Manual\";"),
			_T("private const string ManualFolder = \"Manual\";"),
			false, strdiff::EOL_STRICT, 0, false, 0, true);
		EXPECT_EQ(1, diffs.size());
		if (diffs.size() > 0)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(0, pDiff->begin[0]);
			EXPECT_EQ(0, pDiff->begin[1]);
			EXPECT_EQ(-1, pDiff->end[0]);
			EXPECT_EQ(7, pDiff->end[1]);
		}
	}

	// Sf.net bug #2783185
	// 'private' added to right-side
	// word-diff
	TEST_F(StringDiffsBugsTest, Bug_2783185_2)
	{
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  01234567890123456789012345678901234567890123456789
			_T("const string ManualFolder = \"Manual\";"),
			_T("private const string ManualFolder = \"Manual\";"),
			false, strdiff::EOL_STRICT, 0, false, 0, false);
		EXPECT_EQ(1, diffs.size());
		if (diffs.size() > 0)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(0, pDiff->begin[0]);
			EXPECT_EQ(0, pDiff->begin[1]);
			EXPECT_EQ(-1, pDiff->end[0]);
			EXPECT_EQ(7, pDiff->end[1]);
		}
	}

	// Sf.net bug #2791879
	// 'x' added in right side at end of second and fourth words
	// char-diff
	TEST_F(StringDiffsBugsTest, Bug_2791879_1)
	{
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  01234567890123456789012345678901234567890123456789
			_T("abc def ghi jkl mno"),
			_T("abc defx ghi jklx mno"),
			false, strdiff::EOL_STRICT, 0, false, 0, true);
		EXPECT_EQ(2, diffs.size());
		if (diffs.size() > 1)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(7, pDiff->begin[0]);
			EXPECT_EQ(7, pDiff->begin[1]);
			EXPECT_EQ(6, pDiff->end[0]);
			EXPECT_EQ(7, pDiff->end[1]);
			pDiff = &diffs[1];
			EXPECT_EQ(15, pDiff->begin[0]);
			EXPECT_EQ(16, pDiff->begin[1]);
			EXPECT_EQ(14, pDiff->end[0]);
			EXPECT_EQ(16, pDiff->end[1]);
		}
	}

	// Sf.net bug #2791879
	// 'x' added in right side at end of second and fourth words
	// word-diff
	TEST_F(StringDiffsBugsTest, Bug_2791879_2)
	{
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  01234567890123456789012345678901234567890123456789
			_T("abc def ghi jkl mno"),
			_T("abc defx ghi jklx mno"),
			false, strdiff::EOL_STRICT, 0, false, 0, false);
		EXPECT_EQ(2, diffs.size());
		if (diffs.size() > 1)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(4, pDiff->begin[0]);
			EXPECT_EQ(4, pDiff->begin[1]);
			EXPECT_EQ(6, pDiff->end[0]);
			EXPECT_EQ(7, pDiff->end[1]);
			pDiff = &diffs[1];
			EXPECT_EQ(12, pDiff->begin[0]);
			EXPECT_EQ(13, pDiff->begin[1]);
			EXPECT_EQ(14, pDiff->end[0]);
			EXPECT_EQ(16, pDiff->end[1]);
		}
	}

	// Sf.net bug #2793479
	// ' ' (space) added before '(TCHAR' in left
	// 'const' added before 'BYTE' in right
	// char-diff
	TEST_F(StringDiffsBugsTest, Bug_2793479_1)
	{
		strdiff::SetBreakChars(_T("() "));
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  0123456789012345678901234567890123456789012345678901234567890123456789
			_T("static int iTranslateBytesToBC (TCHAR* pd, BYTE* src, int srclen);"),
			_T("static int iTranslateBytesToBC(TCHAR* pd, const BYTE* src, int srclen);"),
			false, strdiff::EOL_STRICT, 0, false, 1, true);
		EXPECT_EQ(2, diffs.size());
		if (diffs.size() > 1)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(30, pDiff->begin[0]);
			EXPECT_EQ(30, pDiff->begin[1]);
			EXPECT_EQ(30, pDiff->end[0]);
			EXPECT_EQ(29, pDiff->end[1]);
			pDiff = &diffs[1];
			EXPECT_EQ(43, pDiff->begin[0]);
			EXPECT_EQ(42, pDiff->begin[1]);
			EXPECT_EQ(42, pDiff->end[0]);
			EXPECT_EQ(47, pDiff->end[1]);
		}
	}

	// Sf.net bug #2793479
	// ' ' (space) added before '(TCHAR' in left
	// 'const' added before 'BYTE' in right
	// word-diff
	TEST_F(StringDiffsBugsTest, Bug_2793479_2)
	{
		strdiff::SetBreakChars(_T("() "));
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  0123456789012345678901234567890123456789012345678901234567890123456789
			_T("static int iTranslateBytesToBC (TCHAR* pd, BYTE* src, int srclen);"),
			_T("static int iTranslateBytesToBC(TCHAR* pd, const BYTE* src, int srclen);"),
			false, strdiff::EOL_STRICT, 0, false, 1, false);
		EXPECT_EQ(2, diffs.size());
		if (diffs.size() > 0)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(30, pDiff->begin[0]);
			EXPECT_EQ(30, pDiff->begin[1]);
			EXPECT_EQ(30, pDiff->end[0]);
			EXPECT_EQ(29, pDiff->end[1]);
			pDiff = &diffs[1];
			EXPECT_EQ(43, pDiff->begin[0]);
			EXPECT_EQ(42, pDiff->begin[1]);
			EXPECT_EQ(42, pDiff->end[0]);
			EXPECT_EQ(47, pDiff->end[1]);
		}
	}

	// Sf.net bug #2797067
	// '123' and '456  added in right side and 'x' separated from 'vwx'
	// char-diff
	TEST_F(StringDiffsBugsTest, Bug_2797067_1)
	{
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  01234567890123456789012345678901234567890123456789
			_T("abc def ghi jkl mno pqr stu vwx yz"),
			_T("abc def 123 ghi jkl mno pqr stu 456 vw x yz"),
			false, strdiff::EOL_STRICT, 0, false, 0, true);
		EXPECT_EQ(3, diffs.size());
		if (diffs.size() > 2)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(8, pDiff->begin[0]);
			EXPECT_EQ(8, pDiff->begin[1]);
			EXPECT_EQ(7, pDiff->end[0]);
			EXPECT_EQ(11, pDiff->end[1]);
			pDiff = &diffs[1];
			EXPECT_EQ(28, pDiff->begin[0]);
			EXPECT_EQ(32, pDiff->begin[1]);
			EXPECT_EQ(30, pDiff->end[0]);
			EXPECT_EQ(34, pDiff->end[1]);
			pDiff = &diffs[2];
			EXPECT_EQ(32, pDiff->begin[0]);
			EXPECT_EQ(36, pDiff->begin[1]);
			EXPECT_EQ(31, pDiff->end[0]);
			EXPECT_EQ(40, pDiff->end[1]);
		}
	}

	// Sf.net bug #2797067
	// '123' and '456  added in right side and 'x' separated from 'vwx'
	// word-diff
	TEST_F(StringDiffsBugsTest, Bug_2797067_2)
	{
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  01234567890123456789012345678901234567890123456789
			_T("abc def ghi jkl mno pqr stu vwx yz"),
			_T("abc def 123 ghi jkl mno pqr stu 456 vw x yz"),
			false, strdiff::EOL_STRICT, 0, false, 0, false);
		EXPECT_EQ(3, diffs.size());
		if (diffs.size() > 2)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(8, pDiff->begin[0]);
			EXPECT_EQ(8, pDiff->begin[1]);
			EXPECT_EQ(7, pDiff->end[0]);
			EXPECT_EQ(11, pDiff->end[1]);
			pDiff = &diffs[1];
			EXPECT_EQ(28, pDiff->begin[0]);
			EXPECT_EQ(32, pDiff->begin[1]);
			EXPECT_EQ(30, pDiff->end[0]);
			EXPECT_EQ(34, pDiff->end[1]);
			pDiff = &diffs[2];
			EXPECT_EQ(32, pDiff->begin[0]);
			EXPECT_EQ(36, pDiff->begin[1]);
			EXPECT_EQ(31, pDiff->end[0]);
			EXPECT_EQ(40, pDiff->end[1]);
		}
	}

	// Sf.net bug #2797067
	// '123', '456' and last 'r' added to right side
	// char-diff
	TEST_F(StringDiffsBugsTest, Bug_2797067_3)
	{
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  01234567890123456789012345678901234567890123456789
			_T("abc def ghi jkl mno pqr stu vwx yzr"),
			_T("abc def 123ghi jkl mno pqr stu 456vwx yzrr"),
			false, strdiff::EOL_STRICT, 0, false, 0, true);
		EXPECT_EQ(3, diffs.size());
		if (diffs.size() > 2)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(8, pDiff->begin[0]);
			EXPECT_EQ(8, pDiff->begin[1]);
			EXPECT_EQ(7, pDiff->end[0]);
			EXPECT_EQ(10, pDiff->end[1]);
			pDiff = &diffs[1];
			EXPECT_EQ(28, pDiff->begin[0]);
			EXPECT_EQ(31, pDiff->begin[1]);
			EXPECT_EQ(27, pDiff->end[0]);
			EXPECT_EQ(33, pDiff->end[1]);
			pDiff = &diffs[2];
			EXPECT_EQ(35, pDiff->begin[0]);
			EXPECT_EQ(41, pDiff->begin[1]);
			EXPECT_EQ(34, pDiff->end[0]);
			EXPECT_EQ(41, pDiff->end[1]);
		}
	}

	// Sf.net bug #2797067
	// '123', '456' and last 'r' added to right side
	// word-diff
	TEST_F(StringDiffsBugsTest, Bug_2797067_4)
	{
		std::vector<strdiff::wdiff> diffs = strdiff::ComputeWordDiffs(
			//  01234567890123456789012345678901234567890123456789
			_T("abc def ghi jkl mno pqr stu vwx yzr"),
			_T("abc def 123ghi jkl mno pqr stu 456vwx yzrr"),
			false, strdiff::EOL_STRICT, 0, false, 0, false);
		EXPECT_EQ(3, diffs.size());
		if (diffs.size() > 2)
		{
			strdiff::wdiff *pDiff = &diffs[0];
			EXPECT_EQ(8, pDiff->begin[0]);
			EXPECT_EQ(8, pDiff->begin[1]);
			EXPECT_EQ(10, pDiff->end[0]);
			EXPECT_EQ(13, pDiff->end[1]);
			pDiff = &diffs[1];
			EXPECT_EQ(28, pDiff->begin[0]);
			EXPECT_EQ(31, pDiff->begin[1]);
			EXPECT_EQ(30, pDiff->end[0]);
			EXPECT_EQ(36, pDiff->end[1]);
			pDiff = &diffs[2];
			EXPECT_EQ(32, pDiff->begin[0]);
			EXPECT_EQ(38, pDiff->begin[1]);
			EXPECT_EQ(34, pDiff->end[0]);
			EXPECT_EQ(41, pDiff->end[1]);
		}
	}
}  // namespace
