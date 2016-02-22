#include "base/std.h"

#include "fliconv.h"

#include <errno.h>        // for errno, E2BIG
#include <iconv.h>        // for iconv_open, iconv_t, iconv
#include <stddef.h>       // for size_t
#include <string.h>       // for strlen, strcat, strcmp, etc
#include "interactive.h"  // for interactive_t
#include "vm/vm.h"        // for sp, pop_stack, svalue_t, u, etc

#ifndef USE_ICONV
typedef void *iconv_t;
#endif

#ifdef USE_ICONV

struct translation *head;

static struct translation *find_translator(const char *encoding) {
  struct translation *cur = head;
  while (cur) {
    if (!strcmp(cur->name, encoding)) {
      break;
    }
    cur = cur->next;
  }
  return cur;
}

struct translation *get_translator(const char *encoding) {
  struct translation *ret = find_translator(encoding);
  if (ret) {
    return ret;
  }
  ret = reinterpret_cast<struct translation *>(
      DMALLOC(sizeof(struct translation), TAG_PERMANENT, "get_translator"));
  char *name =
      reinterpret_cast<char *>(DMALLOC(strlen(encoding) + 18 + 1, TAG_PERMANENT, "get_translator"));
  strcpy(name, encoding);
#ifdef __linux__
  strcat(name, "//TRANSLIT//IGNORE");
#endif
  ret->name = name;
  ret->incoming = iconv_open(USE_ICONV, encoding);
  ret->outgoing = iconv_open(name, USE_ICONV);

  ret->next = 0;
  if (ret->incoming == (iconv_t)-1 || ret->outgoing == (iconv_t)-1) {
    FREE(name);
    FREE(ret);
    return 0;
  }
  name[strlen(encoding)] = 0;
  if (!head) {
    head = ret;
  } else {
    struct translation *cur = head;
    while (cur->next) {
      cur = cur->next;
    }
    cur->next = ret;
  }
  return ret;
}

char *translate(iconv_t tr, const char *mes, int inlen, int *outlen) {
  if (!tr) {
    *outlen = inlen;
    return (char *)mes;
  }

  size_t len = inlen;
  size_t len2;
  unsigned char *tmp = (unsigned char *)mes;
  static char *res = 0;
  static size_t reslen = 0;
  char *tmp2;

  if (!res) {
    res = reinterpret_cast<char *>(DMALLOC(1, TAG_PERMANENT, "translate"));
    reslen = 1;
  }

  tmp2 = res;
  len2 = reslen;

  while (len) {
    iconv(tr, reinterpret_cast<char **>(&tmp), &len, &tmp2, &len2);
#ifdef PACKAGE_DWLIB
    if (len > 1 && tmp[0] == 0xff && tmp[1] == 0xf9) {
      len -= 2;
      tmp += 2;
#else
    if (0) {
#endif
    } else {
      if (E2BIG == errno) {
        errno = 0;
        tmp = (unsigned char *)mes;
        len = strlen(mes) + 1;
        FREE(res);
        reslen *= 2;
        res = reinterpret_cast<char *>(DMALLOC(reslen, TAG_PERMANENT, "translate"));
        tmp2 = res;
        len2 = reslen;
        continue;
      }
      tmp2[0] = 0;
      *outlen = reslen - len2;
      return res;
    }
  }
  *outlen = reslen - len2;
  return res;
}

#else
char *translate(iconv_t tr, const char *mes, int inlen, int *outlen) {
  *outlen = inlen;
  return (char *)mes;
}
#endif

char *translate_easy(iconv_t tr, const char *mes) {
  if (!tr)
    return (char *)mes;

  int dummy;
  char *res = translate(tr, mes, strlen(mes) + 1, &dummy);
  return res;
}

#ifdef F_SET_ENCODING
void f_set_encoding() {
  object_t *ob;
  if (st_num_arg == 2)
  {
    ob = sp->u.ob;
    pop_stack();
  }
  else
    ob = current_object;

  if (ob->interactive) {
    struct translation *newt = get_translator(const_cast<char *>(sp->u.string));
    if (newt) {
      ob->interactive->trans = newt;
      return;
    }
  }
  pop_stack();
  push_number(0);
}
#endif

#ifdef F_TO_DEFAULT_ENCODING
void f_to_default_encoding(){
  struct translation *newt = get_translator((char *)sp->u.string);
  pop_stack();
  if(!newt)
    error("unknown encoding");
  char *text = (char *)sp->u.string;
  char *translated = translate_easy(newt->incoming, text);
  pop_stack();
  
  if( !translated )
    push_number(0);
  else
    copy_and_push_string(translated);
}
#endif

#ifdef F_DEFAULT_ENCODING_TO
void f_default_encoding_to(){
  struct translation *newt = get_translator((char *)sp->u.string);
  pop_stack();
  if(!newt)
    error("unknown encoding");
  char *text = (char *)sp->u.string;
  char *translated = translate_easy(newt->outgoing, text);
  pop_stack();

  if( !translated )
    push_number(0);
  else
    copy_and_push_string(translated);
}
#endif

#ifdef F_STR_TO_ARR
void f_str_to_arr() {
  static struct translation *newt = 0;
  if (!newt) {
    newt = get_translator("UTF-32");
    translate_easy(newt->outgoing, " ");
  }
  int len;
  int *trans =
      reinterpret_cast<int *>(translate(newt->outgoing, sp->u.string, SVALUE_STRLEN(sp) + 1, &len));
  len /= 4;
  array_t *arr = allocate_array(len);
  while (len--) {
    arr->item[len].u.number = trans[len];
  }
  free_svalue(sp, "str_to_arr");
  put_array(arr);
}

#endif

#ifdef F_ARR_TO_STR
void f_arr_to_str() {
  static struct translation *newt = 0;
  if (!newt) {
    newt = get_translator("UTF-32");
  }
  int len = sp->u.arr->size;
  int *in =
      reinterpret_cast<int *>(DMALLOC(sizeof(int) * (len + 1), TAG_TEMPORARY, "f_arr_to_str"));
  char *trans;
  in[len] = 0;
  while (len--) {
    in[len] = sp->u.arr->item[len].u.number;
  }

  trans = translate(newt->incoming, reinterpret_cast<char *>(in), (sp->u.arr->size + 1) * 4, &len);
  FREE(in);
  pop_stack();
  copy_and_push_string(trans);
}

#endif

#ifdef F_STRWIDTH
void f_strwidth() {
  int len = SVALUE_STRLEN(sp);
  int width = 0;
  int i;
  for (i = 0; i < len; i++) {
    width += !(((sp->u.string[i]) & 0xc0) == 0x80);
  }
  pop_stack();
  push_number(width);
}

#endif
