#ifndef MORROW_GLES_SMOKE_H
#define MORROW_GLES_SMOKE_H

// One-way graphics diagnostic. The SDL renderer must be fully shut down before
// Mesa takes ownership of the default Horizon window; exit returns to hbmenu.
void morrow_gles_smoke_run(void);

#endif
