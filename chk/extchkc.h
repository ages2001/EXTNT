/* EXTNT - checking and repairing an ext2/ext3/ext4 file system (see extchkc.c) */
#ifndef _EXTCHKC_H_
#define _EXTCHKC_H_

#define EXT_CHECK_FIX       0x0001      /* repair what is wrong */
#define EXT_CHECK_VERBOSE   0x0002      /* say what is checked */
#define EXT_CHECK_FORCE     0x0004      /* check even a clean volume */
#define EXT_CHECK_QUIET     0x0008      /* only the summary */

/* Results (combined) */
#define EXT_CHECK_CLEAN     0
#define EXT_CHECK_FIXED     1           /* errors were found and repaired */
#define EXT_CHECK_ERRORS    4           /* errors remain */
#define EXT_CHECK_FAILED    8           /* the check could not run */

int ExtCheckVolume(PEXT_IMAGE Img, ULONG Flags);

#endif
