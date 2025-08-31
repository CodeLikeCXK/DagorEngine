﻿// Copyright (C) Gaijin Games KFT.  All rights reserved.

#include "hypenation.h"
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <debug/dag_debug.h>
#include <debug/dag_fatal.h>
#include <osApiWrappers/dag_unicode.h>

extern unsigned int read_utf8(const dag_char8_t *&ptr);
extern void write_utf8(unsigned int val, char *&ptr);

const dag_char8_t *cant_break_before =
  u8"\r\n\t !%),.:;>?]}¢¨°·ˇˉ―‖’”„‟†‡›℃∶、。〃〆〈《「『〕〗〞︵︹︽︿﹃﹘﹚﹜！＂％＇），．：；？］｀｜｝～";
const dag_char8_t *cant_break_after = u8"\r\n\t $(*,£¥·‘“〈《「『【〔〖〝﹗﹙﹛＄（．［｛￡￥";

#define IS_LOWER(x)   ((x) < 0x100)
#define IS_CHINESE(x) (((x) >= 0x4E00) && ((x) <= 9FFF))

static bool can_break(unsigned int first, unsigned int second)
{
  if (first == '\t' || first == L'\u200B')
    return false;
  if (second == '\t' || second == L'\u200B')
    return false;

  if (IS_LOWER(first) && IS_LOWER(second))
    return false;

  const dag_char8_t *ptr = cant_break_after;
  for (int i = 0; i < utf8_strlen(cant_break_after); i++)
  {
    if (first == read_utf8(ptr))
      return false;
  }

  ptr = cant_break_before;
  for (int i = 0; i < utf8_strlen(cant_break_before); i++)
  {
    if (second == read_utf8(ptr))
      return false;
  }


  return true;
}

SimpleString process_chinese_string(const char *str, wchar_t sep_char)
{
  if (!str || !*str)
    return {};

  SimpleString out;

  int len = (int)strlen(str);
  out.allocBuffer(len * 6);

  //--- PROCESS HERE ---------
  int wlen = utf8_strlen(str);
  if (wlen < 2)
  {
    strcpy(out.str(), str);
    return out;
  }
  auto src = (const dag_char8_t *)str;
  char *dst = out.str();
  unsigned first = read_utf8(src);
  unsigned second = 0;
  for (int i = 0; i < wlen - 1; i++)
  {
    second = read_utf8(src);
    if ((first == '\t' || first == L'\u200B') && (second == '\t' || second == L'\u200B'))
      continue;
    write_utf8(first, dst);
    if (can_break(first, second))
      write_utf8(sep_char, dst);
    first = second;
  }
  write_utf8(second, dst);
  *dst = 0;
  //--- PROCESS HERE ---------

  return out;
}
