/* ISC license. */

#include <errno.h>
#include <skalibs/uint32.h>
#include <skalibs/bytestr.h>
#include <skalibs/allreadwrite.h>
#include <skalibs/buffer.h>
#include <skalibs/env.h>
#include <skalibs/djbunix.h>
#include <skalibs/unix-transactional.h>
#include <s6-rc/s6rc-db.h>

#ifdef DEBUG
#include <skalibs/lolstdio.h>
#define DBG(...) do { bprintf(buffer_2, "debug: ") ; bprintf(buffer_2, __VA_ARGS__) ; bprintf(buffer_2, "\n") ; buffer_flush(buffer_2) ; } while(0)
#else
#define DBG(...)
#endif

static int s6rc_db_check_valid_string (char const *string, unsigned int stringlen, unsigned int pos)
{
  if (pos >= stringlen) return 0 ;
  if (str_nlen(string + pos, stringlen - pos) == stringlen - pos) return 0 ;
  return 1 ;
}

static inline int s6rc_db_check_valid_strings (char const *string, unsigned int stringlen, unsigned int pos, unsigned int n)
{
  while (n--)
  {
    if (!s6rc_db_check_valid_string(string, stringlen, pos)) return 0 ;
    pos += str_len(string + pos) + 1 ;
  }
  return 1 ;
}

static inline int s6rc_db_read_deps (buffer *b, unsigned int max, uint32 *deps, unsigned int ndeps)
{
  uint32 x ;
  ndeps <<= 1 ;
  while (ndeps--)
  {
    if (!s6rc_db_read_uint32(b, &x)) return -1 ;
    if (x >= max) return 0 ;
    *deps++ = x ;
  }
  return 1 ;
}

static inline int s6rc_db_read_services (buffer *b, s6rc_db_t *db)
{
  unsigned int n = db->nshort + db->nlong ;
  s6rc_service_t *sv = db->services ;
  unsigned int nargvs = db->nargvs ;
  char **argvs = db->argvs ;
  char type ;
#ifdef DEBUG
  register unsigned int i = 0 ;
#endif
  while (n--)
  {
    DBG("iteration %u - %u remaining", i++, n) ;
    if (!s6rc_db_read_uint32(b, &sv->name)) return -1 ;
    DBG("  name is %u: %s", sv->name, db->string + sv->name) ;
    if (sv->name >= db->stringlen) return 0 ;
    if (!s6rc_db_check_valid_string(db->string, db->stringlen, sv->name)) return 0 ;
    if (!s6rc_db_read_uint32(b, &sv->flags)) return -1 ;
    DBG("  flags is %x", sv->flags) ;
    if (!s6rc_db_read_uint32(b, &sv->timeout[0])) return -1 ;
    DBG("  timeout0 is %u", sv->timeout[0]) ;
    if (!s6rc_db_read_uint32(b, &sv->timeout[1])) return -1 ;
    DBG("  timeout1 is %u", sv->timeout[1]) ;
    if (!s6rc_db_read_uint32(b, &sv->ndeps[0])) return -1 ;
    DBG("  ndeps0 is %u", sv->ndeps[0]) ;
    if (!s6rc_db_read_uint32(b, &sv->ndeps[1])) return -1 ;
    DBG("  ndeps1 is %u", sv->ndeps[1]) ;
    if (!s6rc_db_read_uint32(b, &sv->deps[0])) return -1 ;
    DBG("  deps0 is %u", sv->deps[0]) ;
    if (sv->deps[0] > db->ndeps || sv->deps[0] + sv->ndeps[0] > db->ndeps)
      return 0 ;
    if (!s6rc_db_read_uint32(b, &sv->deps[1])) return -1 ;
    DBG("  deps1 is %u", sv->deps[1]) ;
    if (sv->deps[1] > db->ndeps || sv->deps[1] + sv->ndeps[1] > db->ndeps)
      return 0 ;
#ifdef DEBUG
    {
      unsigned int k = 0 ;
      for (; k < sv->ndeps[0] ; k++)
        DBG("   rev dep on %u", db->deps[sv->deps[0] + k]) ;
      for (k = 0 ; k < sv->ndeps[1] ; k++)
        DBG("   dep on %u", db->deps[db->ndeps + sv->deps[1] + k]) ;
    }
#endif
    if (buffer_get(b, &type, 1) < 1) return -1 ;
    if (type)
    {
      sv->type = 1 ;
      if (!s6rc_db_read_uint32(b, &sv->x.longrun.servicedir)) return -1 ;
      DBG("  longrun - servicedir is %u: %s", sv->x.longrun.servicedir, db->string + sv->x.longrun.servicedir) ;
      if (!s6rc_db_check_valid_string(db->string, db->stringlen, sv->x.longrun.servicedir)) return 0 ;
    }
    else
    {
      unsigned int i = 0 ;
      DBG("  oneshot") ;
      sv->type = 0 ;
      for (; i < 2 ; i++)
      {
        uint32 argvpos, argc ;
        if (!s6rc_db_read_uint32(b, &argc)) return -1 ;
        DBG("    argc[%u] is %u, nargvs is %u", i, argc, nargvs) ;
        if (argc > nargvs) return 0 ;
        if (!s6rc_db_read_uint32(b, &argvpos)) return -1 ;
        DBG("    argvpos[%u] is %u", i, argvpos) ;
        if (!s6rc_db_check_valid_strings(db->string, db->stringlen, argvpos, argc)) return 0 ;
        if (!env_make((char const **)argvs, argc, db->string + argvpos, db->stringlen - argvpos)) return -1 ;
        DBG("    first arg is %s", argvs[0]) ;
        sv->x.oneshot.argv[i] = argvpos ;
        sv->x.oneshot.argc[i] = argc ;
        argvs += argc ; nargvs -= argc ;
        if (!nargvs--) return 0 ; *argvs++ = 0 ;
      }
    }
    if (buffer_get(b, &type, 1) < 1) return -1 ;
    if (type != '\376') return 0 ;
    sv++ ;
  }
  if (nargvs) return 0 ;
  return 1 ;
}

static inline int s6rc_db_read_string (buffer *b, char *string, unsigned int len)
{
  if (buffer_get(b, string, len) < (int)len) return -1 ;
  return 1 ;
}

static inline int s6rc_db_read_buffer (buffer *b, s6rc_db_t *db)
{
  {
    char banner[S6RC_DB_BANNER_START_LEN] ;
    if (buffer_get(b, banner, S6RC_DB_BANNER_START_LEN) < S6RC_DB_BANNER_START_LEN) return -1 ;
    if (byte_diff(banner, S6RC_DB_BANNER_START_LEN, S6RC_DB_BANNER_START)) return 0 ;
  }

  {
    register int r = s6rc_db_read_string(b, db->string, db->stringlen) ;
    if (r < 1) return r ;
    r = s6rc_db_read_deps(b, db->nshort + db->nlong, db->deps, db->ndeps) ;
    if (r < 1) return r ;
    r = s6rc_db_read_services(b, db) ;
    if (r < 1) return r ;
  }

  {
    char banner[S6RC_DB_BANNER_END_LEN] ;
    if (buffer_get(b, banner, S6RC_DB_BANNER_END_LEN) < S6RC_DB_BANNER_END_LEN) return -1 ;
    if (byte_diff(banner, S6RC_DB_BANNER_END_LEN, S6RC_DB_BANNER_END)) return 0 ;
  }
  return 1 ;
}

int s6rc_db_read (int fdcompiled, s6rc_db_t *db)
{
  int r, e ;
  buffer b ;
  char buf[BUFFER_INSIZE] ;
  int fd = open_readatb(fdcompiled, "db") ;
  if (fd < 0) return -1 ;
  buffer_init(&b, &fd_readsv, fd, buf, BUFFER_INSIZE) ;
  r = s6rc_db_read_buffer(&b, db) ;
  e = errno ;
  fd_close(fd) ;
  errno = e ;
  return r ;
}
