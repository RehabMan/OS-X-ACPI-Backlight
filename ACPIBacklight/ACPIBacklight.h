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


class ACPIBacklightPanel : public IODisplayParameterHandler
{
    OSDeclareDefaultStructors(ACPIBacklightPanel)

public:
	// IOService
    virtual bool init();
    virtual IOService * probe( IOService * provider, SInt32 * score );
	virtual bool start( IOService * provider );
    virtual void stop( IOService * provider );
    virtual void free();
    virtual IOReturn setProperties(OSObject* props);

    //IODisplayParameterHandler
    virtual bool setDisplay( IODisplay * display );
    virtual bool doIntegerSet( OSDictionary * params,
                              const OSSymbol * paramName, UInt32 value );
    virtual bool doDataSet( const OSSymbol * paramName, OSData * value );
    virtual bool doUpdate( void );
    
private:
    IODisplay * _display;
    
    IOACPIPlatformDevice *  gpuDevice, * backLightDevice;
	OSData * supportedLevels;

    IOInterruptEventSource* _workSource;
    IOTimerEventSource* _smoothTimer;
    IOCommandGate* _cmdGate;
    bool _extended;

    bool findDevices(IOService * provider);
    IOACPIPlatformDevice *  getGPU();
    //IOACPIPlatformDevice *  getGPUACPIDevice(IOService *provider);
	bool hasBacklightMethods(IOACPIPlatformDevice * acpiDevice);
    bool hasDOSMethod(IOACPIPlatformDevice * acpiDevice);
    bool hasSAVEMethod(IOACPIPlatformDevice * acpiDevice);
    
	IOACPIPlatformDevice * getChildWithBacklightMethods(IOACPIPlatformDevice * GPUdevice);
    
    OSString * getACPIPath(IOACPIPlatformDevice * acpiDevice);
    
	OSArray * queryACPISupportedBrightnessLevels();
	void setACPIBrightnessLevel(UInt32 level);
    void saveACPIBrightnessLevel(UInt32 level);
	UInt32 queryACPICurentBrightnessLevel();
    void setBrightnessLevel(UInt32 level);
	
	SInt32 setupIndexedLevels();
	SInt32 findIndexForLevel(SInt32 BCLvalue);
	SInt32 * BCLlevels;
	UInt32 BCLlevelsCount;
	UInt32 minAC, maxBat, min, max;
	bool hasSaveMethod;
    UInt32 _value;  // osx value
    
	void getDeviceControl();
    
	IOService * getBatteryDevice();
	bool getACStatus();

    void  processWorkQueue(IOInterruptEventSource *, int);
    void  onSmoothTimer(void);
    void saveACPIBrightnessLevelNVRAM(UInt32 level);
    UInt32 loadFromNVRAM(void);
    UInt32 indexForLevel(UInt32 value, UInt32* rem = NULL);
    UInt32 levelForIndex(UInt32 level);

    IOReturn setPropertiesGated(OSObject* props);
};
#endif
