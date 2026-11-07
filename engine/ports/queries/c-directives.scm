; SPDX-License-Identifier: GPL-3.0-or-later
; Copyright (C) 2026 davoda1
; Add the editor's preprocessor color category to the upstream C/C++ queries.
; The upstream grammar uses generic @keyword for these tokens.
[
  "#define"
  "#elif"
  "#else"
  "#endif"
  "#if"
  "#ifdef"
  "#ifndef"
  "#include"
  (preproc_directive)
] @keyword.directive
