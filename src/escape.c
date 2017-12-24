/*
Copyright 2017 Denis Shtyrov. All rights reserved.
Use of this source code is governed by MIT
license that can be found in the LICENSE file.
*/

#include "escape.h"

#include <ctype.h>
#include <stdlib.h>

static int control_char(const char* str) {
    switch (*str) {
        case 'a':  return '\a';
        case 'b':  return '\b';
        case 'f':  return '\f';
        case 'n':  return '\n';
        case 'r':  return '\r';
        case 't':  return '\t';
        case 'v':  return '\v';
    }
    return *str;
}

int unescape(const char* str) {
  if (*str != '\\') {
    /* use unescaped characters as is */
    return *str;
  }
  if (isdigit(str[1])) {
    /* octal number */
    return strtol(str + 1, NULL, 8);
  }
  if (str[1] == 'x') {
    return strtol(str + 2, NULL, 16);
  }
  return control_char(str + 1);
}