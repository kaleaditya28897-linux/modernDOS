/****************************************************************/
/*                                                              */
/*                           lfnapi.c                           */
/*                                                              */
/*      Native Windows 95 compatible long filename API          */
/*      (INT 21h AH=71h) for FAT12/16/32 volumes.               */
/*                                                              */
/* This file is part of modernDOS, derived from DOS-C/FreeDOS.  */
/*                                                              */
/* This is free software; you can redistribute it and/or modify */
/* it under the terms of the GNU General Public License as      */
/* published by the Free Software Foundation; either version 2, */
/* or (at your option) any later version.                       */
/****************************************************************/

/*
 * Design
 * ------
 * Long names are a translation layer on top of the existing 8.3 file
 * system code.  lfn_resolve() walks a path that may contain long (or
 * short) components and produces the equivalent fully qualified short
 * path.  Existing kernel functions (DosOpen, DosDelete, DosChangeDir,
 * ...) then do the actual work, so their semantics stay identical to the
 * classic INT 21h calls.
 *
 * Creating a long name (mkdir, create, rename target) generates a Win95
 * style short alias ("LONGNA~1.TXT") and arms a one-shot hook
 * (lfn_create_name).  fatfs.c then reserves enough contiguous directory
 * slots and calls lfn_write_entries() right after writing the 8.3 entry.
 *
 * Long names are exchanged with programs in the OEM code page.  On disk
 * they are UCS-2; conversion currently uses code page 437.
 *
 * Not handled here (returned as unsupported, AX=7100h): network and
 * redirected drives (MSCDEX, network clients implement their own LFN
 * support), and JOINed directories inside a path.
 */

#include "portab.h"
#include "globals.h"

#ifdef WITHLFNAPI

#define LFN_MAX        255      /* max long name length (chars)      */
#define LFN_PATHMAX    260      /* max long path length              */
#define LFN_CHARS      13       /* UCS-2 chars per LFN dir entry     */
#define LFN_FINDHNDLS  8        /* concurrent 714Eh search handles   */
#define LFN_PATLEN     80       /* max stored wildcard pattern       */
#define FINDDATA_SIZE  0x13e    /* Win95 WIN32_FIND_DATA as seen by DOS */

/* lfn_resolve() results (errors are negative) */
#define LR_FOUND    0           /* whole path exists                 */
#define LR_MISSING  1           /* last component missing (lfn_comp) */
#define LR_WILD     2           /* last component is a wildcard      */
/* LFN_PASS (proto.h): not ours, report as unsupported              */

/* lfn_resolve() flags */
#define LRF_MISSING_OK 1
#define LRF_WILD_OK    2

/* Hooks used by fatfs.c when creating directory entries */
const char *lfn_create_name;    /* long name for next created entry  */
UWORD lfn_free_need = 1;        /* contiguous free entries needed    */
UWORD lfn_free_run;             /* free entries found so far         */

STATIC char lfn_comp[LFN_MAX + 1];  /* path component / name to create */
STATIC char lfn_name[LFN_MAX + 1];  /* long (or display) name of entry */
STATIC BOOL lfn_has_long;           /* entry has a real long name      */
STATIC char lfn_path1[128];         /* resolved short paths            */
STATIC char lfn_path2[128];
STATIC UBYTE lfn_drive;             /* logical drive of last resolve   */
STATIC UWORD lfn_root_len;          /* length of physical root in path */
STATIC char FAR *lfn_lout;          /* long path output, NULL = off    */
STATIC char FAR *lfn_lend;

struct lfn_find {
  UWORD psp;                    /* owner, 0 = free slot              */
  UBYTE drive;
  UBYTE allow;                  /* allowed attributes (CL)           */
  UBYTE need;                   /* required attributes (CH)          */
  UBYTE done;
  CLUSTER dir;                  /* directory start cluster           */
  UWORD entry;                  /* next directory entry to examine   */
  char pat[LFN_PATLEN];
};
STATIC struct lfn_find lfn_finds[LFN_FINDHNDLS];

/* byte offsets of the 13 UCS-2 characters within an LFN entry */
STATIC const UBYTE lfn_ofs[LFN_CHARS] =
  { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

/* code page 437, characters 80h-FFh */
STATIC const UNICODE cp437_hi[128] = {
  0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
  0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
  0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
  0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
  0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
  0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
  0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
  0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
  0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
  0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
  0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
  0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
  0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
  0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
  0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
  0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0
};

/****************************************************************/
/* Character and name helpers                                   */
/****************************************************************/

STATIC UNICODE oem_to_uni(UBYTE c)
{
  return c < 0x80 ? c : cp437_hi[c - 0x80];
}

STATIC UBYTE uni_to_oem(UNICODE u)
{
  int i;

  if (u < 0x80)
    return (UBYTE)u;
  for (i = 0; i < 128; i++)
    if (cp437_hi[i] == u)
      return (UBYTE)(0x80 + i);
  return '_';
}

STATIC UBYTE lfn_checksum(const char *sfn)
{
  UBYTE sum = 0;
  int i;

  for (i = 0; i < FNAME_SIZE + FEXT_SIZE; i++)
    sum = (UBYTE)(((sum & 1) << 7) + (sum >> 1) + (UBYTE)sfn[i]);
  return sum;
}

STATIC BOOL is_sep(char c)
{
  return c == '\\' || c == '/';
}

/* case insensitive comparison in the current code page */
STATIC BOOL name_eq(const char *a, const char *b)
{
  for (;; a++, b++)
  {
    UBYTE ca = DosUpFChar(*a), cb = DosUpFChar(*b);
    if (ca != cb)
      return FALSE;
    if (ca == '\0')
      return TRUE;
  }
}

/* Win95 style wildcard match: '*' any run, '?' any char,
   "*.*" also matches names without an extension */
STATIC BOOL wild_match(const char *p, const char *s)
{
  const char *pstar = NULL, *sstar = NULL;

  while (*s)
  {
    if (*p == '*')
    {
      pstar = ++p;
      sstar = s;
    }
    else if (*p == '?' || (*p && DosUpFChar(*p) == DosUpFChar(*s)))
    {
      p++;
      s++;
    }
    else if (pstar)
    {
      p = pstar;
      s = ++sstar;
    }
    else
      return FALSE;
  }
  while (*p == '*' || *p == '?')
    p++;
  if (*p == '.')
  {
    p++;
    while (*p == '*' || *p == '?')
      p++;
  }
  return *p == '\0';
}

STATIC BOOL has_wild(const char *s)
{
  for (; *s; s++)
    if (*s == '*' || *s == '?')
      return TRUE;
  return FALSE;
}

/* 8.3 name as shown to programs, honouring the NT lower case flags
   (dir_case bit 3: base name lower case, bit 4: extension lower case) */
STATIC void sfn_display(char *dst, const struct dirent *de)
{
  BOOL ext = FALSE;

  ConvertName83ToNameSZ(dst, (BYTE *)de->dir_name);
  if (!(de->dir_case & 0x18))
    return;
  for (; *dst; dst++)
  {
    if (*dst == '.')
      ext = TRUE;
    else if ((de->dir_case & (ext ? 0x10 : 0x08)) &&
             *dst >= 'A' && *dst <= 'Z')
      *dst += 'a' - 'A';
  }
}

/****************************************************************/
/* Directory access                                             */
/****************************************************************/

/* raw on-disk image of the entry last read with dir_read() */
STATIC UBYTE FAR *dir_raw(f_node_ptr fnp, struct buffer FAR **bpp)
{
  struct buffer FAR *bp;

  bp = getblock(fnp->f_dirsector, fnp->f_dpb->dpb_unit);
  if (bp == NULL)
    return NULL;
  if (bpp)
    *bpp = bp;
  return &bp->b_buffer[fnp->f_diridx * DIRENT_SIZE];
}

/* Read entries from the current position (dm_entry) until a short
   entry is found and assemble the long name chain preceding it.
   On return lfn_name holds the long name, or the display form of the
   8.3 name if there is no valid long name (lfn_has_long FALSE).
   Returns 1: entry found (in fnp->f_dir, dm_entry is its index),
   0: end of directory, <0: error */
STATIC COUNT lfn_dir_next(f_node_ptr fnp)
{
  COUNT rc;
  UBYTE expect = 0;             /* next expected ordinal, 0 = none   */
  UBYTE chk = 0;
  BOOL complete = FALSE;

  for (;; fnp->f_dmp->dm_entry++)
  {
    rc = dir_read(fnp);
    if (rc == 0 || rc == DE_SEEK)
      return 0;
    if (rc < 0)
      return rc;

    if (fnp->f_dir.dir_name[0] == DELETED)
    {
      expect = 0;
      complete = FALSE;
    }
    else if (fnp->f_dir.dir_attrib == D_LFN)
    {
      UBYTE FAR *raw = dir_raw(fnp, NULL);
      UBYTE id, i;
      unsigned pos;

      if (raw == NULL)
        return DE_BLKINVLD;
      id = raw[0];
      complete = FALSE;
      if (id & 0x40)
      {
        expect = id & 0x3f;
        chk = raw[13];
        if (expect == 0 || expect > 20)
          expect = 0;
        else
        {
          pos = expect * LFN_CHARS;
          lfn_name[pos > LFN_MAX ? LFN_MAX : pos] = '\0';
        }
      }
      else if (expect == 0 || id != expect || raw[13] != chk)
        expect = 0;             /* orphaned or damaged chain */
      if (expect)
      {
        pos = (expect - 1) * LFN_CHARS;
        for (i = 0; i < LFN_CHARS && pos < LFN_MAX; i++, pos++)
        {
          UNICODE u = raw[lfn_ofs[i]] | ((UNICODE)raw[lfn_ofs[i] + 1] << 8);
          if (u == 0)
          {
            lfn_name[pos] = '\0';
            break;
          }
          lfn_name[pos] = uni_to_oem(u);
        }
        if (--expect == 0)
          complete = TRUE;
      }
    }
    else
    {
      lfn_has_long = complete && lfn_name[0] != '\0' &&
          chk == lfn_checksum(fnp->f_dir.dir_name);
      if (!lfn_has_long)
        sfn_display(lfn_name, &fnp->f_dir);
      return 1;
    }
  }
}

/* look up a long or short name in directory dir (short path);
   returns 1 found (fnode[0] positioned on it), 0 not found, <0 error */
STATIC COUNT lfn_lookup(const char *dir, const char *name)
{
  f_node_ptr fnp = dir_open(dir, FALSE, &fnode[0]);
  char sfn[13];
  COUNT rc;

  if (fnp == NULL)
    return DE_PATHNOTFND;
  while ((rc = lfn_dir_next(fnp)) == 1)
  {
    if (!(fnp->f_dir.dir_attrib & D_VOLID))
    {
      if (name_eq(lfn_name, name))
        return 1;
      ConvertName83ToNameSZ(sfn, fnp->f_dir.dir_name);
      if (name_eq(sfn, name))
        return 1;
    }
    fnp->f_dmp->dm_entry++;
  }
  return rc;
}

/* position fnode[0] on the entry named by short path `path';
   returns 1 found, 0 path is a root directory, <0 error */
STATIC COUNT lfn_locate(char *path)
{
  char *sep = NULL, *p;
  char sfn[13];
  COUNT rc;

  for (p = path; *p; p++)
    if (*p == '\\')
      sep = p;
  if (sep == NULL || sep[1] == '\0' || (UWORD)strlen(path) <= lfn_root_len)
    return 0;
  strcpy(sfn, sep + 1);
  if (sep == path + 2)
    sep[1] = '\0';
  else
    *sep = '\0';
  rc = lfn_lookup(path, sfn);
  if (sep == path + 2)
    sep[1] = sfn[0];
  else
    *sep = '\\';
  return rc == 0 ? DE_FILENOTFND : rc;
}

/* does the 8.3 name fcb exist in directory dir? 1 yes, 0 no, <0 error */
STATIC COUNT sfn_exists(const char *dir, const char *fcb)
{
  f_node_ptr fnp = dir_open(dir, FALSE, &fnode[0]);
  COUNT rc;

  if (fnp == NULL)
    return DE_PATHNOTFND;
  while ((rc = dir_read(fnp)) == 1)
  {
    if (fnp->f_dir.dir_name[0] != DELETED && fnp->f_dir.dir_attrib != D_LFN
        && memcmp(fnp->f_dir.dir_name, fcb, FNAME_SIZE + FEXT_SIZE) == 0)
      return 1;
    fnp->f_dmp->dm_entry++;
  }
  return rc == DE_SEEK ? 0 : rc;
}

/****************************************************************/
/* Short name generation                                        */
/****************************************************************/

/* convert one character for a short name; 0 = drop it */
STATIC UBYTE sfn_char(UBYTE c, BOOL *lossy)
{
  if (c == ' ' || c == '.')
  {
    *lossy = TRUE;
    return 0;
  }
  if (c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']')
  {
    *lossy = TRUE;
    return '_';
  }
  return DosUpFChar(c);
}

/* Build the 11 character basis name for lname (Win95 algorithm
   without numeric tail). Returns TRUE if the conversion lost
   information and a numeric tail is required. */
STATIC BOOL lfn_basis(const char *lname, char *fcb)
{
  const char *p, *start, *dot = NULL;
  BOOL lossy = FALSE;
  int n;
  UBYTE c;

  memset(fcb, ' ', FNAME_SIZE + FEXT_SIZE);
  for (start = lname; *start == '.'; start++)
    lossy = TRUE;
  for (p = start; *p; p++)
    if (*p == '.')
      dot = p;

  for (n = 0, p = start; *p && p != dot; p++)
    if ((c = sfn_char(*p, &lossy)) != 0)
    {
      if (n < FNAME_SIZE)
        fcb[n++] = c;
      else
        lossy = TRUE;
    }
  if (dot)
    for (n = FNAME_SIZE, p = dot + 1; *p; p++)
      if ((c = sfn_char(*p, &lossy)) != 0)
      {
        if (n < FNAME_SIZE + FEXT_SIZE)
          fcb[n++] = c;
        else
          lossy = TRUE;
      }
  if (fcb[0] == ' ')
  {
    fcb[0] = '_';
    lossy = TRUE;
  }
  if ((UBYTE)fcb[0] == 0xe5)
    fcb[0] = 0x05;
  return lossy;
}

/* generate a unique short name for lname in directory dir */
STATIC COUNT lfn_gen_sfn(const char *dir, const char *lname, char *fcb)
{
  char base[FNAME_SIZE], tail[8];
  int baselen, taillen, keep, i;
  ULONG num;
  COUNT rc;

  if (!lfn_basis(lname, fcb))
  {
    rc = sfn_exists(dir, fcb);
    if (rc <= 0)
      return rc;
  }
  memcpy(base, fcb, FNAME_SIZE);
  for (baselen = 0; baselen < FNAME_SIZE && base[baselen] != ' '; baselen++)
    ;

  for (num = 1; num < 1000000L; num++)
  {
    ULONG v = num;

    /* tail = "~<num>" */
    taillen = 0;
    do
    {
      tail[7 - taillen++] = (char)('0' + (UBYTE)(v % 10));
      v /= 10;
    } while (v);
    tail[7 - taillen++] = '~';

    keep = baselen < FNAME_SIZE - taillen ? baselen : FNAME_SIZE - taillen;
    memset(fcb, ' ', FNAME_SIZE);
    memcpy(fcb, base, keep);
    for (i = 0; i < taillen; i++)
      fcb[keep + i] = tail[8 - taillen + i];

    rc = sfn_exists(dir, fcb);
    if (rc <= 0)
      return rc;
  }
  return DE_ACCESS;
}

/* Prepare creating lfn_comp in directory `path': append the generated
   short name to path and arm the create hook if a long name is needed */
STATIC COUNT lfn_prepare_create(char *path)
{
  char fcb[FNAME_SIZE + FEXT_SIZE], sfn[13];
  size_t len;
  COUNT rc;

  if (has_wild(lfn_comp))
    return DE_FILENOTFND;
  rc = lfn_gen_sfn(path, lfn_comp, fcb);
  if (rc < 0)
    return rc;
  ConvertName83ToNameSZ(sfn, fcb);
  len = strlen(path);
  if (len + 1 + strlen(sfn) >= sizeof(lfn_path1))
    return DE_PATHNOTFND;
  if (path[len - 1] != '\\')
    path[len++] = '\\';
  strcpy(path + len, sfn);
  if (fstrcmp(sfn, lfn_comp) != 0)
    lfn_create_name = lfn_comp;
  return SUCCESS;
}

/* Write the long name entries for the short entry fnp was just
   written to.  fatfs.c reserved the entries in front of it.
   Called by fatfs.c; does nothing unless lfn_create_name is set. */
COUNT lfn_write_entries(f_node_ptr fnp)
{
  struct f_node save;
  const char *name = lfn_create_name;
  unsigned sfn_entry, len, n, i, k, pos;
  struct buffer FAR *bp;
  UBYTE FAR *raw;
  UBYTE chk;
  COUNT rc = SUCCESS;

  lfn_create_name = NULL;
  if (name == NULL || *name == '\0')
    return SUCCESS;

  memcpy(&save, fnp, sizeof(save));
  sfn_entry = fnp->f_dmp->dm_entry;
  chk = lfn_checksum(fnp->f_dir.dir_name);
  len = strlen(name);
  n = (len + LFN_CHARS - 1) / LFN_CHARS;
  if (n > sfn_entry)
    return DE_ACCESS;           /* slots were not reserved */

  /* forget the file context, walk the directory itself */
  dir_init_fnode(fnp, fnp->f_dmp->dm_dircluster);
  for (i = 1; i <= n; i++)
  {
    fnp->f_dmp->dm_entry = sfn_entry - i;
    if (dir_read(fnp) < 0 || (raw = dir_raw(fnp, &bp)) == NULL)
    {
      rc = DE_ACCESS;
      break;
    }
    raw[0] = (UBYTE)(i | (i == n ? 0x40 : 0));
    raw[11] = D_LFN;
    raw[12] = 0;
    raw[13] = chk;
    raw[26] = raw[27] = 0;
    pos = (i - 1) * LFN_CHARS;
    for (k = 0; k < LFN_CHARS; k++, pos++)
    {
      UNICODE u = pos < len ? oem_to_uni(name[pos]) : pos == len ? 0 : 0xffff;
      raw[lfn_ofs[k]] = (UBYTE)u;
      raw[lfn_ofs[k] + 1] = (UBYTE)(u >> 8);
    }
    bp->b_flag |= BFR_DIRTY | BFR_VALID;
  }

  memcpy(fnp, &save, sizeof(save));
  fnp->f_dmp->dm_entry = sfn_entry;
  if (!flush_buffers(fnp->f_dpb->dpb_unit))
    rc = DE_ACCESS;
  return rc;
}

/****************************************************************/
/* Path resolution                                              */
/****************************************************************/

/* append a component to the long path output */
STATIC BOOL lout_add(const char *s)
{
  if (lfn_lout == NULL)
    return TRUE;
  if (lfn_lout[-1] != '\\')
  {
    if (lfn_lout >= lfn_lend)
      return FALSE;
    *lfn_lout++ = '\\';
  }
  for (; *s; s++)
  {
    if (lfn_lout >= lfn_lend)
      return FALSE;
    *lfn_lout++ = *s;
  }
  *lfn_lout = '\0';
  return TRUE;
}

/* remove the last component from the long path output */
STATIC void lout_up(void)
{
  if (lfn_lout == NULL)
    return;
  while (lfn_lout[-1] != '\\')
    lfn_lout--;
  if (lfn_lout[-2] != ':')
    lfn_lout--;
  *lfn_lout = '\0';
}

/* Walk the components of src, appending their short names to dst
   (a short path ending in a directory).  See lfn_resolve(). */
STATIC COUNT lfn_walk(const char FAR *src, char *dst, int flags)
{
  char *end = dst + strlen(dst);
  char sfn[13];
  COUNT rc;

  for (;;)
  {
    const char FAR *p;
    int n = 0;
    BOOL wild = FALSE, last;

    while (is_sep(*src))
      src++;
    if (*src == '\0')
      return LR_FOUND;

    /* extract the next component */
    while (*src && !is_sep(*src))
    {
      UBYTE c = *src++;
      if (n >= LFN_MAX || c < ' ' || c == '"' || c == '<' || c == '>' ||
          c == '|' || c == ':')
        return DE_PATHNOTFND;
      if (c == '*' || c == '?')
        wild = TRUE;
      lfn_comp[n++] = c;
    }
    lfn_comp[n] = '\0';
    for (p = src; is_sep(*p); p++)
      ;
    last = *p == '\0';

    if (fstrcmp(lfn_comp, ".") == 0)
      continue;
    if (fstrcmp(lfn_comp, "..") == 0)
    {
      if ((UWORD)(end - dst) > lfn_root_len)
      {
        while (end[-1] != '\\')
          end--;
        if ((UWORD)(end - 1 - dst) >= lfn_root_len && end - 1 > dst + 2)
          end--;
        *end = '\0';
        lout_up();
      }
      continue;
    }
    /* trailing dots and spaces are not significant */
    while (n && (lfn_comp[n - 1] == '.' || lfn_comp[n - 1] == ' '))
      lfn_comp[--n] = '\0';
    if (n == 0)
      return last ? DE_FILENOTFND : DE_PATHNOTFND;

    if (wild)
    {
      if (last && (flags & LRF_WILD_OK))
        return lout_add(lfn_comp) ? LR_WILD : DE_PATHNOTFND;
      return last ? DE_FILENOTFND : DE_PATHNOTFND;
    }

    rc = lfn_lookup(dst, lfn_comp);
    if (rc < 0)
      return rc;
    if (rc == 0)
    {
      if (last && (flags & LRF_MISSING_OK))
        return lout_add(lfn_comp) ? LR_MISSING : DE_PATHNOTFND;
      return last ? DE_FILENOTFND : DE_PATHNOTFND;
    }
    if (!last && !(fnode[0].f_dir.dir_attrib & D_DIR))
      return DE_PATHNOTFND;

    ConvertName83ToNameSZ(sfn, fnode[0].f_dir.dir_name);
    if ((end - dst) + 1 + strlen(sfn) >= sizeof(lfn_path1))
      return DE_PATHNOTFND;
    if (end[-1] != '\\')
      *end++ = '\\';
    strcpy(end, sfn);
    end += strlen(sfn);
    if (!lout_add(lfn_name))
      return DE_PATHNOTFND;
    if (last)
      return LR_FOUND;
  }
}

/* Resolve the root (and for relative paths the current directory)
   of drive into dst */
STATIC COUNT lfn_start(UBYTE drive, char *dst, BOOL relative)
{
  struct cds FAR *cdsp = get_cds(drive);
  char root[4];
  COUNT rc;

  if (cdsp == NULL)
    return DE_INVLDDRV;
  if (cdsp->cdsFlags & CDSNETWDRV)
    return LFN_PASS;
  lfn_drive = drive;

  root[0] = 'A' + drive;
  root[1] = ':';
  root[2] = '\\';
  root[3] = '\0';
  rc = truename(root, dst, CDS_MODE_CHECK_DEV_PATH);
  if (rc < 0)
    return rc;
  if ((rc & IS_NETWORK) || dst[1] != ':' || dst[2] != '\\' ||
      get_dpb(dst[0] - 'A') == NULL)
    return LFN_PASS;
  lfn_root_len = strlen(dst);

  if (lfn_lout)
  {
    root[3] = '\0';
    fstrcpy(lfn_lout, root);
    lfn_lout += 3;
  }

  if (relative)
  {
    if (lfn_lout)
    {
      /* walk the logical current directory to get its long form */
      root[2] = '\0';
      rc = truename(root, PriPathName, CDS_MODE_SKIP_PHYSICAL);
      if (rc < 0)
        return rc;
      return lfn_walk(PriPathName + 3, dst, 0);
    }
    root[2] = '\0';
    rc = truename(root, dst, CDS_MODE_CHECK_DEV_PATH);
    if (rc < 0)
      return rc;
  }
  return SUCCESS;
}

/* Resolve a long or short path to its fully qualified short form.
   Returns LR_FOUND (dst = path), LR_MISSING / LR_WILD (dst = directory,
   last component in lfn_comp), LFN_PASS or a negative error.
   If lfn_lout is set, the long form is written there as well. */
STATIC COUNT lfn_resolve(const char FAR *src, char *dst, int flags)
{
  UBYTE drive = default_drive;
  COUNT rc;

  if (src[0] == '\0')
    return DE_PATHNOTFND;
  if (is_sep(src[0]) && is_sep(src[1]))
    return LFN_PASS;            /* UNC path */
  if (src[1] == ':')
  {
    drive = DosUpFChar(src[0]) - 'A';
    src += 2;
  }
  rc = lfn_start(drive, dst, !is_sep(src[0]));
  if (rc != SUCCESS)
    return rc;
  return lfn_walk(src, dst, flags);
}

/* is src a character device name (CON, NUL, ...)? */
STATIC BOOL lfn_is_device(const char FAR *src)
{
  COUNT rc = truename(src, SecPathName, CDS_MODE_CHECK_DEV_PATH);
  return rc >= 0 && (rc & IS_DEVICE) && !(rc & IS_NETWORK);
}

/****************************************************************/
/* Time conversion (Win95 FILETIME <-> DOS date/time)           */
/* DOS has no time zone, local time is treated as UTC.          */
/****************************************************************/

#define DAYS_1601_TO_1980 138426L

STATIC void q_muladd(UWORD *q, UWORD m, UWORD a)
{
  ULONG carry = a;
  int i;

  for (i = 0; i < 4; i++)
  {
    carry += (ULONG)q[i] * m;
    q[i] = (UWORD)carry;
    carry >>= 16;
  }
}

STATIC UWORD q_div(UWORD *q, UWORD d)
{
  ULONG rem = 0;
  int i;

  for (i = 3; i >= 0; i--)
  {
    rem = (rem << 16) | q[i];
    q[i] = (UWORD)(rem / d);
    rem %= d;
  }
  return (UWORD)rem;
}

STATIC BOOL is_leap(UWORD y)
{
  return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

STATIC const UWORD lfn_mdays[12] =
  { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

/* DOS date/time + 10ms units -> FILETIME */
STATIC void dos_to_filetime(UWORD date, UWORD time, UBYTE cs, UBYTE FAR *ft)
{
  UWORD q[4], y, m, d, i;
  ULONG days;

  if (date == 0)
  {
    fmemset(ft, 0, 8);
    return;
  }
  y = 1980 + (date >> 9);
  m = (date >> 5) & 15;
  d = date & 31;
  if (m < 1 || m > 12)
    m = 1;
  days = 0;
  for (i = 1980; i < y; i++)
    days += is_leap(i) ? 366 : 365;
  days += lfn_mdays[m - 1] + (m > 2 && is_leap(y)) + (d ? d - 1 : 0);
  days += DAYS_1601_TO_1980;

  q[0] = (UWORD)days;
  q[1] = (UWORD)(days >> 16);
  q[2] = q[3] = 0;
  /* seconds = days * 86400 + time of day */
  q_muladd(q, 43200U, (time >> 11) * 1800 + ((time >> 5) & 63) * 30 +
           (time & 31));
  q_muladd(q, 2, 0);
  /* milliseconds, then 100ns units */
  q_muladd(q, 1000, cs * 10);
  q_muladd(q, 10000, 0);
  for (i = 0; i < 4; i++)
    fputword(ft + 2 * i, q[i]);
}

/* FILETIME -> DOS date/time + 10ms units; FALSE if out of range */
STATIC BOOL filetime_to_dos(const UBYTE FAR *ft, UWORD *date, UWORD *time,
                            UBYTE *cs)
{
  UWORD q[4], ms, sec2, rem2, y, m;
  ULONG days, secs;
  int i;

  for (i = 0; i < 4; i++)
    q[i] = fgetword(ft + 2 * i);
  q_div(q, 10000);
  ms = q_div(q, 1000);
  sec2 = q_div(q, 43200U);       /* half-day seconds */
  rem2 = q_div(q, 2);
  if (q[2] || q[3])
    return FALSE;
  days = ((ULONG)q[1] << 16) | q[0];
  if (days < DAYS_1601_TO_1980)
    return FALSE;
  days -= DAYS_1601_TO_1980;
  secs = (ULONG)rem2 * 43200 + sec2;

  for (y = 1980;; y++)
  {
    UWORD ylen = is_leap(y) ? 366 : 365;
    if (days < ylen)
      break;
    days -= ylen;
  }
  if (y > 2107)
    return FALSE;
  for (m = 12; m > 1; m--)
    if (days >= lfn_mdays[m - 1] + (m > 2 && is_leap(y)))
      break;
  days -= lfn_mdays[m - 1] + (m > 2 && is_leap(y));

  *date = ((y - 1980) << 9) | (m << 5) | (UWORD)(days + 1);
  *time = (UWORD)((secs / 3600) << 11) | (UWORD)(((secs / 60) % 60) << 5) |
      (UWORD)((secs % 60) / 2);
  *cs = (UBYTE)(ms / 10 + (secs % 2) * 100);
  return TRUE;
}

STATIC void lfn_put_time(UBYTE FAR *d, UWORD date, UWORD time, UBYTE cs,
                         UWORD fmt)
{
  if (fmt == 1)
  {
    fputword(d, time);
    fputword(d + 2, date);
    fmemset(d + 4, 0, 4);
  }
  else
    dos_to_filetime(date, time, cs, d);
}

/****************************************************************/
/* Find first/next/close                                        */
/****************************************************************/

STATIC BOOL lfn_attr_ok(UBYTE a, UBYTE allow, UBYTE need)
{
  return (a & ~allow & (D_HIDDEN | D_SYSTEM | D_DIR | D_VOLID)) == 0 &&
      (a & need) == need;
}

/* fill a Win95 find data record from fnode[0] / lfn_name */
STATIC void lfn_finddata(f_node_ptr fnp, UBYTE FAR *d, UWORD fmt)
{
  struct dirent *de = &fnp->f_dir;

  fmemset(d, 0, FINDDATA_SIZE);
  d[0] = de->dir_attrib;
  lfn_put_time(d + 0x04, de->dir_crdate, de->dir_crtime, de->dir_crtimems,
               fmt);
  lfn_put_time(d + 0x0c, de->dir_accdate, 0, 0, fmt);
  lfn_put_time(d + 0x14, de->dir_date, de->dir_time, 0, fmt);
  fputlong(d + 0x20, de->dir_size);
  fstrcpy((char FAR *)(d + 0x2c), lfn_name);
  if (lfn_has_long)
    ConvertName83ToNameSZ((BYTE FAR *)(d + 0x130), de->dir_name);
}

STATIC COUNT lfn_find_next(struct lfn_find *h, lregs *r)
{
  f_node_ptr fnp = &fnode[0];
  struct dpb FAR *dpbp;
  char sfn[13];
  COUNT rc;

  if (h->done)
    return DE_NFILES;
  dpbp = get_dpb(h->drive);
  if (dpbp == NULL)
    return DE_NFILES;
  fnp->f_dpb = dpbp;
  dir_init_fnode(fnp, h->dir);
  fnp->f_dmp->dm_entry = h->entry;

  while ((rc = lfn_dir_next(fnp)) == 1)
  {
    fnp->f_dmp->dm_entry++;
    if (!lfn_attr_ok(fnp->f_dir.dir_attrib, h->allow, h->need))
      continue;
    ConvertName83ToNameSZ(sfn, fnp->f_dir.dir_name);
    if (!wild_match(h->pat, lfn_name) && !wild_match(h->pat, sfn))
      continue;
    h->entry = fnp->f_dmp->dm_entry;
    lfn_finddata(fnp, MK_FP(r->ES, r->DI), r->SI);
    return SUCCESS;
  }
  h->done = TRUE;
  return rc < 0 ? rc : DE_NFILES;
}

STATIC struct lfn_find *lfn_find_handle(UWORD hnd)
{
  struct lfn_find *h;

  if (hnd == 0 || hnd > LFN_FINDHNDLS)
    return NULL;
  h = &lfn_finds[hnd - 1];
  return h->psp ? h : NULL;
}

STATIC COUNT lfn_find_first(lregs *r)
{
  struct lfn_find *h;
  COUNT rc;
  UWORD i;

  for (i = 0; i < LFN_FINDHNDLS && lfn_finds[i].psp; i++)
    ;
  if (i == LFN_FINDHNDLS)
    return DE_TOOMANY;
  h = &lfn_finds[i];

  rc = lfn_resolve(MK_FP(r->DS, r->DX), lfn_path1, LRF_WILD_OK);
  if (rc < 0 || rc == LFN_PASS)
    return rc;

  h->allow = r->CL;
  h->need = r->CH;
  h->done = FALSE;
  h->drive = lfn_path1[0] - 'A';

  if (rc == LR_WILD)
  {
    f_node_ptr fnp;

    if (strlen(lfn_comp) >= LFN_PATLEN)
      return DE_PATHNOTFND;
    fnp = dir_open(lfn_path1, FALSE, &fnode[0]);
    if (fnp == NULL)
      return DE_PATHNOTFND;
    strcpy(h->pat, lfn_comp);
    h->dir = fnp->f_dmp->dm_dircluster;
    h->entry = 0;
    h->psp = cu_psp;
    rc = lfn_find_next(h, r);
    if (rc < 0)
    {
      h->psp = 0;
      return rc;
    }
  }
  else
  {
    /* no wildcards: at most one match, no need to keep a pattern */
    rc = lfn_locate(lfn_path1);
    if (rc <= 0)
      return rc == 0 ? DE_FILENOTFND : rc;
    if (!lfn_attr_ok(fnode[0].f_dir.dir_attrib, h->allow, h->need))
      return DE_FILENOTFND;
    lfn_finddata(&fnode[0], MK_FP(r->ES, r->DI), r->SI);
    h->done = TRUE;
    h->psp = cu_psp;
  }
  r->AX = i + 1;
  r->CX = 0;                    /* no unicode conversion problems */
  return SUCCESS;
}

/* release all search handles of a terminating process */
VOID lfn_close_finds(UWORD psp)
{
  int i;

  for (i = 0; i < LFN_FINDHNDLS; i++)
    if (lfn_finds[i].psp == psp)
      lfn_finds[i].psp = 0;
}

/****************************************************************/
/* Individual functions                                         */
/****************************************************************/

/* AX=7141h delete file(s) */
STATIC COUNT lfn_delete(lregs *r)
{
  f_node_ptr fnp;
  CLUSTER dir;
  UWORD entry = 0, deleted = 0;
  UBYTE drive;
  char sfn[13];
  size_t len;
  COUNT rc;

  rc = lfn_resolve(MK_FP(r->DS, r->DX), lfn_path1,
                   r->SI == 1 ? LRF_WILD_OK : 0);
  if (rc == LR_FOUND)
    return DosDelete(lfn_path1, D_ALL);
  if (rc != LR_WILD)
    return rc;

  /* wildcard delete: match long and short names */
  fnp = dir_open(lfn_path1, FALSE, &fnode[0]);
  if (fnp == NULL)
    return DE_PATHNOTFND;
  dir = fnp->f_dmp->dm_dircluster;
  drive = lfn_path1[0] - 'A';
  len = strlen(lfn_path1);
  if (lfn_path1[len - 1] != '\\')
    lfn_path1[len++] = '\\';
  lfn_path1[len] = '\0';

  for (;;)
  {
    fnp = &fnode[0];
    fnp->f_dpb = get_dpb(drive);
    dir_init_fnode(fnp, dir);
    fnp->f_dmp->dm_entry = entry;
    while ((rc = lfn_dir_next(fnp)) == 1)
    {
      fnp->f_dmp->dm_entry++;
      if (fnp->f_dir.dir_attrib & (D_DIR | D_VOLID) ||
          !lfn_attr_ok(fnp->f_dir.dir_attrib, r->CL, r->CH))
        continue;
      ConvertName83ToNameSZ(sfn, fnp->f_dir.dir_name);
      if (wild_match(lfn_comp, lfn_name) || wild_match(lfn_comp, sfn))
        break;
    }
    if (rc < 0)
      return rc;
    if (rc == 0)
      break;
    entry = fnp->f_dmp->dm_entry;
    if (len + strlen(sfn) >= sizeof(lfn_path2))
      return DE_PATHNOTFND;
    strcpy(lfn_path2, lfn_path1);
    strcpy(lfn_path2 + len, sfn);
    rc = DosDelete(lfn_path2, D_ALL);
    if (rc < 0)
      return rc;
    deleted++;
  }
  return deleted ? SUCCESS : DE_FILENOTFND;
}

/* AX=7143h get/set attributes and time stamps */
STATIC COUNT lfn_attrib(lregs *r)
{
  f_node_ptr fnp = &fnode[0];
  struct dirent *de = &fnp->f_dir;
  COUNT rc;

  rc = lfn_resolve(MK_FP(r->DS, r->DX), lfn_path1, 0);
  if (rc != LR_FOUND)
    return rc;

  switch (r->BL)
  {
    case 0x00:
      rc = DosGetFattr(lfn_path1);
      if (rc >= SUCCESS)
      {
        r->CX = rc;
        rc = SUCCESS;
      }
      return rc;
    case 0x01:
      return DosSetFattr(lfn_path1, r->CX);
  }

  rc = lfn_locate(lfn_path1);
  if (rc <= 0)
    return rc == 0 ? DE_ACCESS : rc;
  switch (r->BL)
  {
    case 0x02:                  /* physical size */
    {
      ULONG clsize = (ULONG)(fnp->f_dpb->dpb_clsmask + 1) *
          fnp->f_dpb->dpb_secsize;
      ULONG size = (de->dir_size + clsize - 1) / clsize * clsize;
      r->AX = (UWORD)size;
      r->DX = (UWORD)(size >> 16);
      return SUCCESS;
    }
    case 0x03:
      de->dir_date = r->DI;
      de->dir_time = r->CX;
      break;
    case 0x04:
      r->CX = de->dir_time;
      r->DI = de->dir_date;
      return SUCCESS;
    case 0x05:
      de->dir_accdate = r->DI;
      break;
    case 0x06:
      r->DI = de->dir_accdate;
      return SUCCESS;
    case 0x07:
      de->dir_crdate = r->DI;
      de->dir_crtime = r->CX;
      de->dir_crtimems = (UBYTE)r->SI;
      break;
    case 0x08:
      r->CX = de->dir_crtime;
      r->DI = de->dir_crdate;
      r->SI = de->dir_crtimems;
      return SUCCESS;
    default:
      return DE_INVLDFUNC;
  }
  return dir_write(fnp) ? SUCCESS : DE_ACCESS;
}

/* AX=7147h get current directory (long form) */
STATIC COUNT lfn_getcwd(lregs *r)
{
  UBYTE drive = r->DL ? r->DL - 1 : default_drive;
  char FAR *buf = MK_FP(r->DS, r->SI);
  char FAR *s;
  COUNT rc;

  lfn_lout = buf;
  lfn_lend = buf + LFN_PATHMAX;
  rc = lfn_start(drive, lfn_path1, TRUE);
  if (rc < 0 || rc == LFN_PASS)
    return rc;
  /* strip "X:\" */
  for (s = buf + 3;; s++)
  {
    s[-3] = *s;
    if (*s == '\0')
      break;
  }
  r->AX = 0x0100;
  return SUCCESS;
}

/* AX=7156h rename / move */
STATIC COUNT lfn_rename(lregs *r)
{
  CLUSTER odir;
  UWORD oentry;
  COUNT rc;

  rc = lfn_resolve(MK_FP(r->DS, r->DX), lfn_path1, 0);
  if (rc != LR_FOUND)
    return rc;
  rc = lfn_locate(lfn_path1);
  if (rc <= 0)
    return rc == 0 ? DE_ACCESS : rc;
  odir = fnode[0].f_dmp->dm_dircluster;
  oentry = fnode[0].f_dmp->dm_entry;

  rc = lfn_resolve(MK_FP(r->ES, r->DI), lfn_path2, LRF_MISSING_OK);
  if (rc == LR_FOUND)
  {
    /* only allowed for the same file: change the long name/case */
    char sfn[13];

    if (fstrcmp(lfn_path1, lfn_path2) != 0 || lfn_locate(lfn_path2) != 1 ||
        fnode[0].f_dmp->dm_dircluster != odir ||
        fnode[0].f_dmp->dm_entry != oentry)
      return DE_ACCESS;
    if (lfn_has_long && fstrcmp(lfn_name, lfn_comp) == 0)
      return SUCCESS;           /* nothing to do */
    ConvertName83ToNameSZ(sfn, fnode[0].f_dir.dir_name);
    if (fstrcmp(sfn, lfn_comp) == 0)
    {
      if (!lfn_has_long)
        return SUCCESS;
      lfn_create_name = "";     /* drop the long name */
    }
    else
      lfn_create_name = lfn_comp;
  }
  else if (rc == LR_MISSING)
  {
    rc = lfn_prepare_create(lfn_path2);
    if (rc < 0)
      return rc;
  }
  else
    return rc;

  rc = DosRename(lfn_path1, lfn_path2);
  lfn_create_name = NULL;
  return rc;
}

/* AX=7160h truename: CL=0 canonical, CL=1 short, CL=2 long */
STATIC COUNT lfn_truename(lregs *r)
{
  const char FAR *src = MK_FP(r->DS, r->SI);
  char FAR *dst = MK_FP(r->ES, r->DI);
  COUNT rc;

  if (r->CL > 2)
    return DE_INVLDFUNC;
  if (lfn_is_device(src))
  {
    fstrcpy(dst, SecPathName);
    return SUCCESS;
  }
  if (r->CL != 1)
  {
    lfn_lout = dst;
    lfn_lend = dst + LFN_PATHMAX;
  }
  rc = lfn_resolve(src, lfn_path1,
                   r->CL == 0 ? LRF_MISSING_OK | LRF_WILD_OK : 0);
  if (rc < 0 || rc == LFN_PASS)
    return rc;
  if (r->CL == 1)
    fstrcpy(dst, lfn_path1);
  return SUCCESS;
}

/* AX=716Ch extended open/create */
STATIC COUNT lfn_open(lregs *r)
{
  const char FAR *src = MK_FP(r->DS, r->SI);
  unsigned flags;
  long lrc;
  COUNT rc;

  /* high nibble must be <= 1, low nibble must be <= 2 */
  if ((r->DL & 0xef) > 0x2)
    return DE_INVLDFUNC;
  flags = (r->BX & 0x70ff) | ((r->DL & 3) << 8) | ((r->DL & 0x10) << 6);

  if (lfn_is_device(src))
    lrc = DosOpen((char FAR *)src, flags, r->CL);
  else
  {
    rc = lfn_resolve(src, lfn_path1, LRF_MISSING_OK);
    if (rc == LR_MISSING)
    {
      if (!(r->DL & 0x10))
        return DE_FILENOTFND;
      rc = lfn_prepare_create(lfn_path1);
      if (rc < 0)
        return rc;
    }
    else if (rc != LR_FOUND)
      return rc;
    lrc = DosOpen(lfn_path1, flags, r->CL);
    lfn_create_name = NULL;
  }
  if (lrc < SUCCESS)
    return (COUNT)lrc;
  r->AX = (UWORD)lrc;
  r->CX = (UWORD)(lrc >> 16);   /* action taken */
  return SUCCESS;
}

/* AX=71A0h get volume information */
STATIC COUNT lfn_volinfo(lregs *r)
{
  const char FAR *root = MK_FP(r->DS, r->DX);
  UBYTE drive = default_drive;
  struct cds FAR *cdsp;
  struct dpb FAR *dpbp;
  const char *fs;

  if (root[0] && root[1] == ':')
    drive = DosUpFChar(root[0]) - 'A';
  cdsp = get_cds(drive);
  if (cdsp == NULL)
    return DE_INVLDDRV;
  if (cdsp->cdsFlags & CDSNETWDRV)
    return LFN_PASS;
  dpbp = cdsp->cdsDpb;
  if (dpbp == NULL)
    return LFN_PASS;
  fs = ISFAT32(dpbp) ? "FAT32" : "FAT";
  if (r->CX > strlen(fs))
    fstrcpy(MK_FP(r->ES, r->DI), fs);
  r->BX = 0x4006;   /* case preserved, unicode on disk, LFN functions */
  r->CX = LFN_MAX;
  r->DX = LFN_PATHMAX;
  return SUCCESS;
}

/* AX=71A7h FILETIME <-> DOS date/time */
STATIC COUNT lfn_timeconv(lregs *r)
{
  if (r->BL == 0)
  {
    UWORD date, time;
    UBYTE cs;

    if (!filetime_to_dos(MK_FP(r->DS, r->SI), &date, &time, &cs))
      return DE_INVLDDATA;
    r->CX = time;
    r->DX = date;
    r->BH = cs;
    return SUCCESS;
  }
  if (r->BL == 1)
  {
    dos_to_filetime(r->DX, r->CX, r->BH, MK_FP(r->ES, r->DI));
    return SUCCESS;
  }
  return DE_INVLDFUNC;
}

/* AX=71A8h generate short name (basis, no numeric tail) */
STATIC COUNT lfn_genshort(lregs *r)
{
  const char FAR *src = MK_FP(r->DS, r->SI);
  char fcb[FNAME_SIZE + FEXT_SIZE], sfn[13];
  int n;

  for (n = 0; src[n]; n++)
    if (n >= LFN_MAX)
      return DE_INVLDDATA;
  fmemcpy(lfn_comp, src, n + 1);
  lfn_basis(lfn_comp, fcb);
  if (r->DH == 0)
    fmemcpy(MK_FP(r->ES, r->DI), fcb, FNAME_SIZE + FEXT_SIZE);
  else
  {
    ConvertName83ToNameSZ(sfn, fcb);
    fstrcpy(MK_FP(r->ES, r->DI), sfn);
  }
  return SUCCESS;
}

/****************************************************************/
/* INT 21h AH=71h dispatcher                                    */
/* Returns SUCCESS, a negative DOS error, or LFN_PASS for       */
/* subfunctions/drives the kernel does not handle itself.       */
/****************************************************************/

COUNT int21_lfn(lregs *r)
{
  struct lfn_find *h;
  COUNT rc;

  lfn_create_name = NULL;
  lfn_lout = NULL;

  switch (r->AL)
  {
    case 0x0d:                  /* reset drive */
      flush();
      return SUCCESS;

    case 0x39:                  /* make directory */
      rc = lfn_resolve(MK_FP(r->DS, r->DX), lfn_path1, LRF_MISSING_OK);
      if (rc == LR_FOUND)
        return DE_ACCESS;
      if (rc != LR_MISSING)
        return rc;
      rc = lfn_prepare_create(lfn_path1);
      if (rc == SUCCESS)
        rc = DosMkRmdir(lfn_path1, 0x39);
      lfn_create_name = NULL;
      return rc;

    case 0x3a:                  /* remove directory */
      rc = lfn_resolve(MK_FP(r->DS, r->DX), lfn_path1, 0);
      if (rc != LR_FOUND)
        return rc;
      return DosMkRmdir(lfn_path1, 0x3a);

    case 0x3b:                  /* change directory */
    {
      struct cds FAR *cdsp;
      UWORD off;

      rc = lfn_resolve(MK_FP(r->DS, r->DX), lfn_path1, 0);
      if (rc != LR_FOUND)
        return rc;
      /* use the logical drive so SUBSTed drives keep their own cwd */
      cdsp = get_cds(lfn_drive);
      off = cdsp->cdsBackslashOffset;
      if (off > 2 && strlen(lfn_path1) >= off)
      {
        lfn_path2[0] = 'A' + lfn_drive;
        lfn_path2[1] = ':';
        strcpy(lfn_path2 + 2, lfn_path1[off] ? lfn_path1 + off : "\\");
        return DosChangeDir(lfn_path2);
      }
      return DosChangeDir(lfn_path1);
    }

    case 0x41:
      return lfn_delete(r);

    case 0x43:
      return lfn_attrib(r);

    case 0x47:
      return lfn_getcwd(r);

    case 0x4e:
      return lfn_find_first(r);

    case 0x4f:
      h = lfn_find_handle(r->BX);
      if (h == NULL)
        return DE_INVLDHNDL;
      rc = lfn_find_next(h, r);
      if (rc == SUCCESS)
        r->CX = 0;
      return rc;

    case 0xa1:
      h = lfn_find_handle(r->BX);
      if (h == NULL)
        return DE_INVLDHNDL;
      h->psp = 0;
      return SUCCESS;

    case 0x56:
      return lfn_rename(r);

    case 0x60:
      return lfn_truename(r);

    case 0x6c:
      return lfn_open(r);

    case 0xa0:
      return lfn_volinfo(r);

    case 0xa7:
      return lfn_timeconv(r);

    case 0xa8:
      return lfn_genshort(r);
  }
  return LFN_PASS;
}

#endif
