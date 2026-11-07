// Copyright (C) 2026 davoda1
// SPDX-License-Identifier: GPL-3.0-or-later
#include "AutoMerge.h"
#include <cstdlib>
#include <climits>
#include "xdiff/xdiff.h"
namespace md {
AutoMergeResult mergeText(const QByteArray &base,const QByteArray &left,const QByteArray &right)
{
    AutoMergeResult result;
    if(base.size()>LONG_MAX || left.size()>LONG_MAX || right.size()>LONG_MAX){result.error=QStringLiteral("The input exceeds the merge engine's supported size.");return result;}
    mmfile_t ancestor{const_cast<char*>(base.constData()),static_cast<long>(base.size())};
    mmfile_t mine{const_cast<char*>(left.constData()),static_cast<long>(left.size())};
    mmfile_t theirs{const_cast<char*>(right.constData()),static_cast<long>(right.size())};
    xmparam_t parameters{};parameters.level=XDL_MERGE_ZEALOUS;parameters.marker_size=7;
    parameters.style=XDL_MERGE_DIFF3;parameters.ancestor="BASE";parameters.file1="LEFT";parameters.file2="RIGHT";
    mmbuffer_t output{};
    result.conflicts=xdl_merge(&ancestor,&mine,&theirs,&parameters,&output);
    if(result.conflicts<0)result.error=QStringLiteral("The merge engine could not complete this operation.");
    else result.text=QByteArray(output.ptr,output.size);
    if(output.ptr)xdl_free(output.ptr);
    return result;
}
}
