// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 davoda1
#include "pch.h"
#include <gtest/gtest.h>
#include <QFile>
#include <QTemporaryDir>
#include "paths.h"
#include "Environment.h"
#include "unicoder.h"
#include "cio.h"
#include "DirTravel.h"
#include "DirItem.h"

TEST(WindowsUtf8, WideBoundaryRoundtrip) {
    const String text = u8"árvíztűrő 日本語 🙂";
    std::wstring wide;
    ucr::toUTF16(text, wide);
    EXPECT_EQ(text, ucr::toTString(wide));
    EXPECT_EQ(text, ucr::toUTF8(text));
}
TEST(WindowsUtf8, FileOpenAndDirectoryTraversal) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const String base = temporary.path().toUtf8().toStdString();
    const String filename = u8"árvíztűrő-日本語-🙂.txt";
    const String path = paths::ConcatPath(base, filename);
    FILE* file = nullptr;
    ASSERT_EQ(0, cio::tfopen_s(&file, path, "wb"));
    const String content = u8"ő and 中 and 🙂\n";
    ASSERT_EQ(content.size(), fwrite(content.data(),1,content.size(),file));
    ASSERT_EQ(0, fclose(file));
    EXPECT_EQ(paths::IS_EXISTING_FILE, paths::DoesPathExist(path));
    ASSERT_EQ(0, cio::tfopen_s(&file, path, "rb"));
    std::string actual(content.size(), '\0');
    ASSERT_EQ(actual.size(), fread(actual.data(),1,actual.size(),file));
    fclose(file);
    EXPECT_EQ(content, actual);
    DirItem item;
    ASSERT_TRUE(item.Update(path));
    EXPECT_EQ(content.size(), item.size);
    ASSERT_TRUE(item.UpdateFileName(path));
    EXPECT_EQ(filename, item.filename.get());
    DirItemArray directories, files;
    DirTravel::LoadFiles(base, &directories, &files);
    ASSERT_EQ(1u, files.size());
    EXPECT_EQ(filename, files[0].filename.get());
}
TEST(WindowsUtf8, NestedDirectoryAndEnvironmentExpansion) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const String nested = paths::ConcatPath(temporary.path().toUtf8().toStdString(), u8"mappa-ő/中/🙂");
    ASSERT_TRUE(paths::CreateIfNeeded(nested));
    EXPECT_TRUE(paths::IsDirectory(nested));
    EXPECT_TRUE(paths::IsPathAbsolute(paths::GetLongPath(nested)));
    const QByteArray variable = temporary.path().toUtf8();
    qputenv("MERGEDESK_TEST_DIRECTORY", variable);
    EXPECT_EQ(variable.toStdString(), env::ExpandEnvironmentVariables("%MERGEDESK_TEST_DIRECTORY%"));
    qunsetenv("MERGEDESK_TEST_DIRECTORY");
}
