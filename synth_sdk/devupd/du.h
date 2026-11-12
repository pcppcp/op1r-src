/* Development update over SysEx (docs/35): protocol, staging + manifest, guarded boot-area flasher.
 * Pure C: file access and NAND access go through callbacks, so the same code runs in the host tests (in-memory
 * file system, fake NAND) and later in the firmware (YAFFS2 API, nand.cpp primitives).
 * Hard rule: NAND blocks 0..3 (te-boot) are never erased or written; every NAND call passes du_guard(). */
#ifndef DU_H
#define DU_H
#include <stdint.h>

/* ---- SysEx framing: F0 00 20 76 7D <cmd> <seq lo> <seq hi> <7-bit packed payload> F7 ---- */
#define DU_SYSEX_HDR      5                 /* F0 00 20 76 7D */
#define DU_MAX_MSG        256               /* the firmware's SysEx buffer has no length check: stay well below */
#define DU_MAX_DATA       192               /* raw bytes per DATA message (offset 4 + data packed -> <= 230 bytes) */
enum { DU_HELLO = 1, DU_BEGIN = 2, DU_DATA = 3, DU_END = 4, DU_COMMIT = 5, DU_ABORT = 6,
       DU_STAT = 7,                         /* path -> u32 size of /yaffs2/<path> (DU_E_NOFILE: missing); protocol 4:
                                               path [u32 offset, u32 crc] -> size, crc, offset: CRC-32 continued over at
                                               most DU_CRC_STEP bytes per STAT (ask again from offset until = size) */
       DU_IDENT = 8,                        /* -> running firmware's version text (boot screen "op1r <n> [time]") */
       DU_DELETE = 9,                       /* path (plugins/<name>.op1p only) -> staged delete, applied like files */
       DU_LIST = 10 };                      /* u16 index -> u32 size + name of the index-th plugins/ file (E_NOFILE: end) */
#define DU_PROTOCOL 4
#define DU_CRC_STEP 32768u                  /* STAT: bytes read per message (main loop: a few ms, not the whole file) */
#define DU_DEL_SIZE 0xFFFFFFFFu             /* manifest / session size of a delete entry (no staged data) */
enum { DU_REPLY = 0x40 };                   /* reply: cmd | 0x40, seq, status, payload */
enum { DU_OK = 0, DU_E_FORMAT = 1, DU_E_STATE = 2, DU_E_PATH = 3, DU_E_SIZE = 4, DU_E_OFFSET = 5, DU_E_CRC = 6,
       DU_E_IO = 7, DU_E_FULL = 8, DU_WAIT_KEY = 9, DU_E_NOFILE = 10 };
#define DU_MAX_REPLY 64                     /* reply payload bytes (status included) */

uint32_t du_crc32(uint32_t crc, const uint8_t *p, uint32_t n);    /* crc = 0 to start (zlib CRC-32) */
int du_pack7(const uint8_t *in, int n, uint8_t *out);             /* 7 bytes -> 8 (MSBs first); returns length */
int du_unpack7(const uint8_t *in, int n, uint8_t *out);           /* inverse; -1 if a byte has bit 7 set */
/* build a message (host / replies); returns its length or -1 if it would exceed DU_MAX_MSG */
int du_build(uint8_t *msg, int cmd, int seq, const uint8_t *payload, int n);
int du_path_ok(const char *path);                                 /* allow-list: OP1_vdk.ldr, content/..., plugins/<name>.op1p; never te-boot */
int du_plugin_file(const char *path);                             /* plugins/<name>.op1p (deletable) */

/* ---- device side session ---- */
#define DU_MAX_FILES 160                    /* a whole tree (--force) is ~125 files */
#define DU_MAX_PATH  96
typedef struct {                            /* file system callbacks (firmware: YAFFS2) */
    int (*open_w)(void *ctx, const char *path);                   /* create / truncate; 0 ok */
    int (*write)(void *ctx, const uint8_t *p, uint32_t n);         /* append to the open file; 0 ok */
    int (*close)(void *ctx);
    int (*remove)(void *ctx, const char *path);
    int (*key_pressed)(void *ctx);                                /* the user confirmed the commit on the device */
    void (*reboot)(void *ctx);
    void *ctx;
    int (*stat)(void *ctx, const char *path, uint32_t *size);     /* 0 ok, else missing */
    int (*ident)(void *ctx, char *out, int cap);                  /* version text, returns its length */
    int (*list)(void *ctx, int index, char *name, int cap, uint32_t *size);   /* plugins/ entry: 0 ok, else end */
    int (*read)(void *ctx, const char *path, uint32_t off, uint8_t *buf, uint32_t n);   /* /yaffs2/<path>: bytes, < 0 error */
} du_fs;
typedef struct {
    char path[DU_MAX_PATH];
    uint32_t size, crc;
} du_file;
typedef struct {
    const du_fs *fs;
    int open;                               /* a file is being received */
    uint32_t written, crc;                  /* of the open file */
    int n;                                  /* files completed (staged) */
    du_file f[DU_MAX_FILES + 1];
} du_session;
#define DU_STAGE_DIR  "/yaffs2/op1r_stage"
#define DU_MANIFEST   "/yaffs2/op1r_stage/manifest"

void du_init(du_session *s, const du_fs *fs);
void du_abort(du_session *s);                                     /* drop the session + whole staging dir */
/* one received SysEx message (with F0 / F7); writes the reply into rep, returns its length (0: not ours) */
int du_handle(du_session *s, const uint8_t *msg, int n, uint8_t *rep);
/* manifest text for the staged files (written on COMMIT); parse: 0 ok, files into f[], count into *n */
int du_manifest_write(const du_session *s, char *out, int cap);
int du_manifest_parse(const char *text, du_file *f, int *n);
#define DU_TEXT_CAP (DU_MAX_FILES * (DU_MAX_PATH + 32) + 64)
extern char du_text[DU_TEXT_CAP];                                 /* manifest text: COMMIT (running) or apply (boot) */
void du_stage_name(int i, char *out);                             /* /yaffs2/op1r_stage/fNNN */

/* ---- guarded boot-area flasher ---- */
#define DU_BLOCK        0x20000             /* 128 KB per boot block (64 x 2 KB pages) */
#define DU_FIRST_BLOCK  4                   /* main firmware starts here; 0..3 = te-boot, NEVER touched */
#define DU_LAST_BLOCK   131                 /* last block of the boot area (YAFFS2 from 132) */
typedef struct {                            /* NAND callbacks, physical block numbers (firmware: nand.cpp) */
    int (*is_bad)(void *ctx, int block);                          /* factory / runtime bad block */
    int (*erase)(void *ctx, int block);                           /* 0 ok */
    int (*write)(void *ctx, int block, const uint8_t *data);       /* one 128 KB block; 0 ok */
    int (*read)(void *ctx, int block, uint8_t *data);              /* 0 ok */
    void *ctx;
} du_nand;
int du_guard(int block);                    /* 1 if block may be touched (DU_FIRST_BLOCK..DU_LAST_BLOCK) */
int du_ldr_ok(const uint8_t *ldr, uint32_t size);                 /* boot-ROM header checks */
enum { DU_F_OK = 0, DU_F_BADLDR = 1, DU_F_NOSPACE = 2, DU_F_GUARD = 3, DU_F_ERASE = 4, DU_F_WRITE = 5, DU_F_VERIFY = 6 };
/* flash ldr into the boot area from block 4, skipping bad blocks; buf: 128 KB scratch. Plans before touching. */
int du_flash_ldr(const du_nand *nand, const uint8_t *ldr, uint32_t size, uint8_t *buf);
#endif
