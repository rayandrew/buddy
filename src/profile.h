#pragma once

#ifdef HAVE_HPCTOOLKIT

#include <hpctoolkit.h>
#define profile_start() hpctoolkit_sampling_start()
#define profile_stop() hpctoolkit_sampling_stop()

#else

#define profile_start()
#define profile_stop()

#endif
