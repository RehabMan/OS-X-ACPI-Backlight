//
//  Debug.h
//

#ifndef ACPIBacklightDisplay_Debug_h
#define ACPIBacklightDisplay_Debug_h

#if defined(DEBUG)
#define DbgLog(arg...)	IOLog(arg)
#else
#define DbgLog(arg...)
#endif

#if defined(DEBUG)
#define setDebugProperty(arg...)	setProperty(arg)
#else
#define setDebugProperty(arg...)
#endif



#endif
