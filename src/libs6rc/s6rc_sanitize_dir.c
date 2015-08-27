/* ISC license. */

#include <skalibs/bytestr.h>
#include <skalibs/stralloc.h>
#include <skalibs/djbunix.h>
#include <s6-rc/s6rc-utils.h>

int s6rc_sanitize_dir (stralloc *sa, char const *fn, unsigned int *dirlen)
{
  int wasnull = !sa->s ;
  unsigned int base = sa->len ;
  unsigned int fnlen = str_len(fn) ;
  unsigned int ddirlen ;
  if (!sadirname(sa, fn, fnlen)) return 0 ;
  if (sa->len != base + 1 || sa->s[base] != '/')
    if (!stralloc_catb(sa, "/", 1)) goto err ;
  ddirlen = sa->len ;
  if (!sabasename(sa, fn, fnlen)) goto err ;
  *dirlen = ddirlen ;
  return 1 ;

 err:
  if (wasnull) stralloc_free(sa) ;
  else sa->len = base ;
  return 0 ;
}
