/* ISC license. */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <skalibs/uint16.h>
#include <skalibs/bytestr.h>
#include <skalibs/tai.h>
#include <skalibs/direntry.h>
#include <skalibs/djbunix.h>
#include <skalibs/genalloc.h>
#include <s6/s6-supervise.h>
#include <s6/ftrigr.h>
#include <s6/ftrigw.h>
#include <s6-rc/s6rc-servicedir.h>

int s6rc_servicedir_manage (char const *live, tain_t const *deadline, tain_t *stamp)
{
  ftrigr_t a = FTRIGR_ZERO ;
  genalloc ids = GENALLOC_ZERO ; /* uint16 */
  gid_t gid = getgid() ;
  unsigned int livelen = str_len(live) ;
  int ok = 1 ;
  int e = 0 ;
  DIR *dir ;
  char dirfn[livelen + 13] ;
  if (!ftrigr_startf(&a, deadline, stamp)) return 0 ;
  byte_copy(dirfn, livelen, live) ;
  byte_copy(dirfn + livelen, 13, "/servicedirs") ;
  dir = opendir(dirfn) ;
  if (!dir) goto closederr ;
  for (;;)
  {
    direntry *d ;
    errno = 0 ;
    d = readdir(dir) ;
    if (!d) break ;
    if (d->d_name[0] == '.') continue ;
    {
      unsigned int len = str_len(d->d_name) ;
      int r ;
      uint16 id ;
      char srcfn[livelen + 20 + len] ;
      char dstfn[livelen + 10 + len] ;
      byte_copy(srcfn, livelen + 12, dirfn) ;
      srcfn[livelen + 12] = '/' ;
      byte_copy(srcfn + livelen + 13, len + 1, d->d_name) ;
      r = s6_svc_ok(srcfn) ;
      if (r < 0) { e = errno ; goto err ; }
      if (!r)
      {
        byte_copy(srcfn + livelen + 13 + len, 6, "/down") ;
        if (!touch(srcfn)) { e = errno ; goto err ; }
        byte_copy(srcfn + livelen + 14 + len, 6, "event") ;
        if (!ftrigw_fifodir_make(srcfn, gid, 0)) { e = errno ; goto err ; }
        id = ftrigr_subscribe(&a, srcfn, "s", 0, deadline, stamp) ;
        if (!id) { e = errno ; goto err ; }
        if (!genalloc_append(uint16, &ids, &id)) { e = errno ; goto err ; }
        srcfn[livelen + 13 + len] = 0 ;
      }
      byte_copy(dstfn, livelen, live) ;
      byte_copy(dstfn + livelen, 9, "/scandir/") ;
      byte_copy(dstfn + livelen + 9, len + 1, d->d_name) ;
      if (symlink(srcfn, dstfn) < 0 && (!r || errno != EEXIST)) { e = errno ; goto err ; }
    }
  }
  if (errno) { e = errno ; goto err ; }
  dir_close(dir) ;
  {
    char scanfn[livelen + 9] ;
    register int r ;
    byte_copy(scanfn, livelen, live) ;
    byte_copy(scanfn + livelen, 9, "/scandir") ;
    r = s6_svc_writectl(scanfn, S6_SVSCAN_CTLDIR, "a", 1) ;
    if (r < 0) { e = errno ; goto closederr ; }
    if (!r) ok = 3 ;
    else if (ftrigr_wait_and(&a, genalloc_s(uint16, &ids), genalloc_len(uint16, &ids), deadline, stamp) < 0)
    { e = errno ; goto closederr ; }
  }

  genalloc_free(uint16, &ids) ;
  ftrigr_end(&a) ;
  return ok ;

 err:
  dir_close(dir) ;
 closederr:
  genalloc_free(uint16, &ids) ;
  ftrigr_end(&a) ;
  errno = e ;  
  return 0 ;
}