// SAINTS ROW: ReInstated - ReXGlue recompiled app (sr1: Saints Row, sr2: Saints Row 2).

#include "sr_app.h"

#if SR_GAME == 2
REX_DEFINE_APP(sr2, SrApp::Create)
#else
REX_DEFINE_APP(sr1, SrApp::Create)
#endif
