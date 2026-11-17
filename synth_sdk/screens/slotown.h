/* One screen slot, several modules that put their own screen object in it for a while (the tape page's: the MIDI seq
 * view seqdev/ms_fw.c, FREE's screen screens/tfscreen.c, the update prompt devupd/du_fw.c). Each install remembers
 * what it covered; a restore puts that back only while the slot still holds our object. If another owner covered us
 * since, it inherits what we covered (its restore then skips us); if a writer outside these owners did, the slot is
 * left alone. So a slot never ends up holding the object of an owner that is off. */
#ifndef SLOTOWN_H
#define SLOTOWN_H
typedef struct so_owner {
    void *obj, *prev;                       /* ours; what our install covered */
    int id;                                 /* the slot, while in */
    unsigned char in;
    struct so_owner *next;                  /* every owner seen (linked on first install) */
} so_owner;
void so_install(so_owner *o, void **tab, int id);   /* no-op while in */
void so_restore(so_owner *o, void **tab);           /* no-op while out */
#endif
