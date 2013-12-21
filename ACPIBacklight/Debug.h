//
//  Debug.h
//

#ifndef ACPIBacklightDisplay_Debug_h
#define ACPIBacklightDisplay_Debug_h

#if defined(DEBUG)
#define DbgLog(arg...) do { IOLog(arg); } while (0)
#else
#define DbgLog(arg...) do { } while (0)
#endif

#if defined(DEBUG)
#define setDebugProperty(arg...) do { setProperty(arg); } while (0)
#else
#define setDebugProperty(arg...) do { } while (0)
#endif

#endif
