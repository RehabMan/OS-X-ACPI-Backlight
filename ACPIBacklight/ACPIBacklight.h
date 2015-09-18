/*
 * Copyright (c) 1998-2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#ifndef ACPIBacklightDisplay_ACPIBacklightDevice_h
#define ACPIBacklightDisplay_ACPIBacklightDevice_h

#include <IOKit/IOService.h>
#include <IOKit/graphics/IODisplay.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLocks.h>

#define NOINLINE __attribute__((noinline))
#define EXPORT __attribute__((visibility("default")))
#define PRIVATE __attribute__((visibility("hidden"))) NOINLINE

class ACPIBacklightPanel;

struct BacklightHandlerParams
{
    UInt32 _xrgl, _xrgh, _klvx, _lmax, _kpch;
};

class EXPORT BacklightHandler : public IOService
{
    OSDeclareDefaultStructors(BacklightHandler)
    typedef IOService super;

public:
    // BacklightHandler
    virtual void setBacklightLevel(UInt32 level);
    virtual UInt32 getBacklightLevel();
};

class EXPORT IntelBacklightHandler : public BacklightHandler
{
    OSDeclareDefaultStructors(IntelBacklightHandler)
    typedef BacklightHandler super;

private:
    IOMemoryMap *_baseMap;
    volatile void *_baseAddr;
    ACPIBacklightPanel* _panel;
    UInt32 _fbtype;
    BacklightHandlerParams _params;

    enum { kFBTypeIvySandy = 1, kFBTypeHaswellBroadwell = 2, };

public:
    // IOService
    virtual bool init();
    virtual bool start(IOService * provider);
    virtual void stop(IOService * provider);

    // BacklightHandler
    virtual void setBacklightLevel(UInt32 level);
    virtual UInt32 getBacklightLevel();
};

class EXPORT ACPIBacklightPanel : public IODisplayParameterHandler
{
    OSDeclareDefaultStructors(ACPIBacklightPanel)
    typedef IODisplayParameterHandler super;

public:
	// IOService
    virtual bool init();
    virtual IOService * probe( IOService * provider, SInt32 * score );
	virtual bool start( IOService * provider );
    virtual void stop( IOService * provider );
    virtual void free();
    virtual IOReturn setProperties(OSObject* props);

    // IODisplayParameterHandler
    virtual bool setDisplay( IODisplay * display );
    virtual bool doIntegerSet( OSDictionary * params,
                              const OSSymbol * paramName, UInt32 value );
    virtual bool doDataSet( const OSSymbol * paramName, OSData * value );
    virtual bool doUpdate( void );

    // ACPIBacklightPanel
    virtual bool setBacklightHandler(BacklightHandler* handler, BacklightHandlerParams* params);
    
private:
    BacklightHandler* _backlightHandler;
    IODisplay * _display;
#if 0 //REVIEW: experimental stuff
    IOService * _provider;
#endif

    IOACPIPlatformDevice *  gpuDevice, * backLightDevice;

    IOInterruptEventSource* _workSource;
    enum { kWorkSave = 0x01, kWorkSetBrightness = 0x02 };
    unsigned _workPending;
    PRIVATE void scheduleWork(unsigned newWork);
    
    IOTimerEventSource* _smoothTimer;
    IOCommandGate* _cmdGate;
    IORecursiveLock* _lock;
    bool _extended;
    int _smoothIndex;

    PRIVATE bool findDevices(IOService * provider);
    PRIVATE IOACPIPlatformDevice *  getGPU();
    //IOACPIPlatformDevice *  getGPUACPIDevice(IOService *provider);
	PRIVATE bool hasBacklightMethods(IOACPIPlatformDevice * acpiDevice);
    PRIVATE bool hasDOSMethod(IOACPIPlatformDevice * acpiDevice);
    PRIVATE bool hasSAVEMethod(IOACPIPlatformDevice * acpiDevice);
    
	PRIVATE IOACPIPlatformDevice * getChildWithBacklightMethods(IOACPIPlatformDevice * GPUdevice);
    
    PRIVATE OSString * getACPIPath(IOACPIPlatformDevice * acpiDevice);
    
	PRIVATE OSArray * queryACPISupportedBrightnessLevels();
	PRIVATE void setACPIBrightnessLevel(UInt32 level);
    PRIVATE void saveACPIBrightnessLevel(UInt32 level);
	PRIVATE UInt32 queryACPICurentBrightnessLevel();
    PRIVATE void setBrightnessLevel(UInt32 level);
    PRIVATE void setBrightnessLevelSmooth(UInt32 level);
	
	PRIVATE UInt32 setupIndexedLevels();
	PRIVATE UInt32 findIndexForLevel(UInt32 BCLvalue);
    
	UInt32* BCLlevels;
	UInt32 BCLlevelsCount;
	UInt32 minAC, maxBat, min, max;
    
    UInt32 _options;
    enum { kDisableSmooth = 0x01, kWaitForHandler = 0x02, kForceUseHandler = 0x04, };
    BacklightHandlerParams _handlerParams;
    PRIVATE bool useBacklightHandler();

	bool hasSaveMethod;
    int _value;  // osx value
    int _from_value; // current value working towards _value
    int _committed_value;
    int _saved_value;
    
	PRIVATE void getDeviceControl();
    
	PRIVATE IOService * getBatteryDevice();
	PRIVATE bool getACStatus();

    PRIVATE void  processWorkQueue(IOInterruptEventSource *, int);
    PRIVATE void  onSmoothTimer(void);
    PRIVATE void saveACPIBrightnessLevelNVRAM(UInt32 level);
    PRIVATE UInt32 loadFromNVRAM(void);
    PRIVATE NOINLINE UInt32 indexForLevel(UInt32 value, UInt32* rem = NULL);
    PRIVATE NOINLINE UInt32 levelForIndex(UInt32 level);
    PRIVATE UInt32 levelForValue(UInt32 value);

    PRIVATE IOReturn setPropertiesGated(OSObject* props);
#ifdef DEBUG
    PRIVATE void setKLVX(UInt32 levx);
#endif
};

#endif //ACPIBacklightDisplay_ACPIBacklightDevice_h
