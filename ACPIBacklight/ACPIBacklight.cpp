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


#include <IOKit/IONVRAM.h>
#include <IOKit/IOLib.h>
#include "ACPIBacklight.h"
#include "Debug.h"

#define super IODisplayParameterHandler

OSDefineMetaClassAndStructors(ACPIBacklightPanel, IODisplayParameterHandler)

#define kACPIBacklightLevel "acpi-backlight-level"
#define kRawBrightness "RawBrightness"

#define kBacklightLevelMin  0
#define kBacklightLevelMax  0x400

#define kSmoothDelta "SmoothDelta%d"
#define kSmoothStep "SmoothStep%d"
#define kSmoothTimeout "SmoothTimeout%d"
#define kSmoothBufSize 16

#define countof(x) (sizeof(x)/sizeof(x[0]))
#define abs(x) ((x) < 0 ? -(x) : (x));

struct SmoothData
{
    int delta;
    int step;
    int timeout;
};

struct SmoothData smoothData[] =
{
    0x10,   1,  10000,
    0x40,   4,  10000,
    0xFFFF, 16, 10000,
};

#pragma mark -
#pragma mark IOService functions override
#pragma mark -


bool ACPIBacklightPanel::init()
{
	DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
    backLightDevice = NULL;
    BCLlevels = NULL;
    gpuDevice = NULL;
    _display = NULL;

    _workSource = NULL;
    _smoothTimer = NULL;
    _cmdGate = NULL;

    _extended = false;
    _options = 0;
    _lock = NULL;

	return super::init();
}


IOService * ACPIBacklightPanel::probe( IOService * provider, SInt32 * score )
{
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
    bool hasFound = findDevices(provider);
    DbgLog("%s: probe(devices found : %s)\n", this->getName(), (hasFound ? "true" : "false") );
    
    if (!hasFound)
        return NULL;
    
    DbgLog("%s: %s has backlight Methods\n", this->getName(), backLightDevice->getName());
    
    return super::probe(provider, score);
}


bool ACPIBacklightPanel::start( IOService * provider )
{
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);

    _lock = IORecursiveLockAlloc();
    if (!_lock)
        return false;
    
    findDevices(provider);
    getDeviceControl();
    hasSaveMethod = hasSAVEMethod(backLightDevice);
    min = 0;
    max = setupIndexedLevels();

    // add interrupt source for delayed actions...
    _workSource = IOInterruptEventSource::interruptEventSource(this, OSMemberFunctionCast(IOInterruptEventAction, this, &ACPIBacklightPanel::processWorkQueue));
    if (!_workSource)
        return false;
    IOWorkLoop* workLoop = getWorkLoop();
    if (!workLoop)
    {
        _workSource->release();
        _workSource = NULL;
        return false;
    }
    workLoop->addEventSource(_workSource);
    _workPending = 0;

    // add timer for smooth fade ins
    if (!(_options & kDisableSmooth))
    {
        _smoothTimer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &ACPIBacklightPanel::onSmoothTimer));
        if (_smoothTimer)
            workLoop->addEventSource(_smoothTimer);
    }

    _cmdGate = IOCommandGate::commandGate(this);
    if (_cmdGate)
        workLoop->addEventSource(_cmdGate);

    // initialize from properties
    OSDictionary* dict = getPropertyTable();
    setPropertiesGated(dict);

    // write current values from smoothData
    for (int i = 0; i < countof(smoothData); i++)
    {
        char buf[kSmoothBufSize];
        snprintf(buf, sizeof(buf), kSmoothDelta, i);
        setProperty(buf, smoothData[i].delta, 32);
        snprintf(buf, sizeof(buf), kSmoothStep, i);
        setProperty(buf, smoothData[i].step, 32);
        snprintf(buf, sizeof(buf), kSmoothTimeout, i);
        setProperty(buf, smoothData[i].timeout, 32);
    }
    setProperty(kRawBrightness, queryACPICurentBrightnessLevel(), 32);
    
#ifdef DEBUG
    setProperty("CycleTest", 1, 32);
    setProperty("KLVX", 1, 32);
#endif

    // load and set default brightness level
    UInt32 value = loadFromNVRAM();
    DbgLog("%s: loadFromNVRAM returns %d\n", this->getName(), value);
    UInt32 current = queryACPICurentBrightnessLevel();
    _committed_value = _value = _from_value = levelForValue(current);
    DbgLog("%s: current brightness: %d (%d)\n", this->getName(), _from_value, current);
    if (-1 != value)
    {
        _committed_value = value;
        DbgLog("%s: setting to value from nvram %d\n", this->getName(), value);
        setBrightnessLevelSmooth(value);
    }

    // make the service available for clients like 'ioio'...
    registerService();

    DbgLog("%s: min = %u, max = %u\n", this->getName(), min, max);
	IOLog("ACPIBacklight: Version 2.0.2\n");

	return true;
}


void ACPIBacklightPanel::stop( IOService * provider )
{
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);

    IOWorkLoop* workLoop = getWorkLoop();
    if (workLoop)
    {
        if (_workSource)
        {
            workLoop->removeEventSource(_workSource);
            _workSource->release();
            _workSource = NULL;
        }
        if (_smoothTimer)
        {
            workLoop->removeEventSource(_smoothTimer);
            _smoothTimer->release();
            _smoothTimer = NULL;
        }
        if (_cmdGate)
        {
            workLoop->removeEventSource(_cmdGate);
            _cmdGate->release();
            _cmdGate = NULL;
        }
    }
    _extended = false;

    if (_lock)
    {
        IORecursiveLockFree(_lock);
        _lock = NULL;
    }

    return super::stop(provider);
}

void ACPIBacklightPanel::free()
{
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
	
	if (gpuDevice)
    {
		gpuDevice->release();
        gpuDevice = NULL;
    }
    
	if (backLightDevice)
    {
        backLightDevice->release();
        backLightDevice = NULL;
    }
    
	if (BCLlevels)
    {
		IODelete(BCLlevels, SInt32, BCLlevelsCount);
        BCLlevels = NULL;
    }
	
    if (_display)
    {
        _display->release();
        _display = NULL;
    }
    
    super::free();
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
#pragma mark -
#pragma mark IODisplayParameterHandler functions override
#pragma mark -

UInt32 ACPIBacklightPanel::indexForLevel(UInt32 value, UInt32* rem)
{
    UInt32 index = value * (max-min);
    if (rem)
        *rem = index % kBacklightLevelMax;
    index = index / kBacklightLevelMax + min;
    return index;
}

UInt32 ACPIBacklightPanel::levelForIndex(UInt32 index)
{
    UInt32 value = ((index-min) * kBacklightLevelMax + (max-min)/2) / (max-min);
    return value;
}

UInt32 ACPIBacklightPanel::levelForValue(UInt32 value)
{
    // return approx. OS X level for ACPI value
    UInt32 index = findIndexForLevel(value);
    UInt32 level = levelForIndex(index);
    if (index < BCLlevelsCount-1)
    {
        // pro-rate between levels
        int diff = levelForIndex(index+1) - level;
        // now pro-rate diff for value as between BCLLevels[index] and BCLLevels[index+1]
        diff *= value - BCLlevels[index];
        diff /= BCLlevels[index+1] - BCLlevels[index];
        level += diff;
    }
    return level;
}

bool ACPIBacklightPanel::setDisplay( IODisplay * display )
{    
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);

    // retain new display (also allow setting to same instance as previous)
    if (display)
        display->retain();
    OSSafeRelease(_display);
    _display = display;
    if (_display)
    {
        // automatically commit a non-zero value on display change
        if (_value)
            _committed_value = _value;
        // update brightness levels
        doUpdate();
    }
    return true;
}


bool ACPIBacklightPanel::doIntegerSet( OSDictionary * params, const OSSymbol * paramName, UInt32 value)
{
    DbgLog("%s::%s(\"%s\", %d)\n", this->getName(), __FUNCTION__, paramName->getCStringNoCopy(), value);
    if ( gIODisplayBrightnessKey->isEqualTo(paramName))
    {   
        //DbgLog("%s::%s(%s) map %d -> %d\n", this->getName(),__FUNCTION__, paramName->getCStringNoCopy(), value, indexForLevel(value));
        setBrightnessLevelSmooth(value);
        return true;
    }
    else if (gIODisplayParametersCommitKey->isEqualTo(paramName))
    {
        UInt32 index = indexForLevel(_value);
        //DbgLog("%s::%s(%s) map %d -> %d\n", this->getName(),__FUNCTION__, paramName->getCStringNoCopy(), value, index);
        _committed_value = _value;
        IODisplay::setParameter(params, gIODisplayBrightnessKey, _committed_value);
        // save to NVRAM in work loop
        scheduleWork(kWorkSave|kWorkSetBrightness);
        // save to BIOS nvram via ACPI
        if (hasSaveMethod)
            saveACPIBrightnessLevel(BCLlevels[index]);
        return true;
    }
    return false;
}


bool ACPIBacklightPanel::doDataSet( const OSSymbol * paramName, OSData * value )
{
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    return true;
}


bool ACPIBacklightPanel::doUpdate( void )
{
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    bool result = false;

    OSDictionary* newDict = 0;
	OSDictionary* allParams = OSDynamicCast(OSDictionary, _display->copyProperty(gIODisplayParametersKey));
    if (allParams)
    {
        newDict = OSDictionary::withDictionary(allParams);
        allParams->release();
    }
    
    OSDictionary* backlightParams = OSDictionary::withCapacity(2);
    ////OSDictionary* linearParams = OSDictionary::withCapacity(2);

    //REVIEW_REHABMAN: myParams is not used...
    OSDictionary* myParams  = OSDynamicCast(OSDictionary, copyProperty(gIODisplayParametersKey));
    if (/*linearParams && */backlightParams && myParams)
	{				
		//DbgLog("%s: ACPILevel min %d, max %d, value %d\n", this->getName(), min, max, _value);
		
        IODisplay::addParameter(backlightParams, gIODisplayBrightnessKey, kBacklightLevelMin, kBacklightLevelMax);
        IODisplay::setParameter(backlightParams, gIODisplayBrightnessKey, _committed_value);

        ////IODisplay::addParameter(linearParams, gIODisplayLinearBrightnessKey, 0, 0x710);
        ////IODisplay::setParameter(linearParams, gIODisplayLinearBrightnessKey, ((_index-min) * 0x710 + (max-min)/2) / (max-min));

        OSNumber * num = OSNumber::withNumber(0ULL, 32);
        OSDictionary * commitParams = OSDictionary::withCapacity(1);
        commitParams->setObject("reg", num);
        backlightParams->setObject(gIODisplayParametersCommitKey, commitParams);
        num->release();
        commitParams->release();
                
        if (newDict)
        {
            newDict->merge(backlightParams);
            ////newDict->merge(linearParams);
            _display->setProperty(gIODisplayParametersKey, newDict);
            newDict->release();
        }
        else
            _display->setProperty(gIODisplayParametersKey, backlightParams);

        //refresh properties here too
        setProperty(gIODisplayParametersKey, backlightParams);
        
        backlightParams->release();
        myParams->release();
        ////linearParams->release();

        result = true;
	}

    return result;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
#pragma mark -
#pragma mark ACPI related functions
#pragma mark -


bool ACPIBacklightPanel::findDevices(IOService * provider)
{
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
    if (!gpuDevice || !backLightDevice)
    {
        IOACPIPlatformDevice * dev = OSDynamicCast(IOACPIPlatformDevice, provider);
        if (hasBacklightMethods(dev))
        {
            DbgLog("%s: PNLF has backlight Methods\n", this->getName());
            backLightDevice = dev;
            gpuDevice = dev;
            gpuDevice->retain();
            backLightDevice->retain();
        }
        else
        {            
            gpuDevice = getGPU();
            
            if (NULL == gpuDevice)
                return false;
            
            gpuDevice->retain();
            
            if (hasBacklightMethods(gpuDevice))
            {
                backLightDevice = gpuDevice;
            }
            else
            {
                backLightDevice = getChildWithBacklightMethods(gpuDevice);
            }
            
            if (backLightDevice == NULL)
                return false;
            
            backLightDevice->retain();
        }

#ifdef DEBUG
        if (gpuDevice != backLightDevice)
        {
            OSArray * devicePaths = OSArray::withCapacity(2);
            OSString* path = getACPIPath(gpuDevice);
            IOLog("ACPIBacklight: ACPI Method _DOS found. Device path: %s\n", path->getCStringNoCopy());
            devicePaths->setObject(path);
            path->release();
            path = getACPIPath(backLightDevice);
            IOLog("ACPIBacklight: ACPI Methods _BCL _BCM _BQC found. Device path: %s\n", path->getCStringNoCopy());
            devicePaths->setObject(path);
            path->release();
            setProperty("ACPI Devices Paths", devicePaths);
            devicePaths->release();
        }
        else
        {
            OSString* path = getACPIPath(backLightDevice);
            IOLog("ACPIBacklight: ACPI Methods _DOS _BCL _BCM _BQC found. Device path: %s\n", path->getCStringNoCopy());
            setProperty("ACPI Device Path", path);
            path->release();
        }
#endif
    }
    return true;
}


OSString * ACPIBacklightPanel::getACPIPath(IOACPIPlatformDevice * acpiDevice)
{
    OSString * separator = OSString::withCStringNoCopy(".");
    OSArray * array = OSArray::withCapacity(10);
    
    char devicePath[512];
    bzero(devicePath, sizeof(devicePath));
    IOACPIPlatformDevice * parent = acpiDevice;
    
    IORegistryIterator * iter = IORegistryIterator::iterateOver(acpiDevice, gIOACPIPlane, kIORegistryIterateParents | kIORegistryIterateRecursively);
    if (iter)
    {
        do {
            array->setObject(parent->copyName(gIOACPIPlane));
            array->setObject(separator);
            parent = OSDynamicCast(IOACPIPlatformDevice, iter->getNextObject());
        } while (parent);
        iter->release();
        
        int offset = 0;
        OSString * str = OSDynamicCast(OSString, array->getLastObject());
        for (int i = array->getCount()-2; ((i>=0) || ((offset + str->getLength()) > sizeof(devicePath))) ; i--)
        {
            str = OSDynamicCast(OSString, array->getObject(i));
            strncpy(devicePath + offset, str->getCStringNoCopy(), str->getLength());
            offset += str->getLength();
        }
    }
    return OSString::withCString(devicePath);
}


IOACPIPlatformDevice *  ACPIBacklightPanel::getGPU()
{
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
    IORegistryIterator * iter = IORegistryIterator::iterateOver(gIOACPIPlane, kIORegistryIterateRecursively);
    IOACPIPlatformDevice * look = NULL, * ret = NULL;
    IORegistryEntry * entry;
    
    if (iter)
    {
        do
        {
            entry = iter->getNextObject();
            look = OSDynamicCast(IOACPIPlatformDevice, entry);
            if (look)
            {
                DbgLog("%s: testing device: %s\n", this->getName(), look->getName());
                if (hasDOSMethod(look))
                {
                    ret = look;
                    break;
                }
            }
        }
        while (entry) ;
        iter->release();
    }
    return ret;
}


bool ACPIBacklightPanel::hasBacklightMethods(IOACPIPlatformDevice * acpiDevice)
{
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
	bool ret = true;
	if (kIOReturnSuccess == acpiDevice->validateObject("_BCL"))
		DbgLog("%s: ACPI device %s has _BCL\n", this->getName(), acpiDevice->getName());
	else
		ret = false;

    if (kIOReturnSuccess == acpiDevice->validateObject("XBCM") && kIOReturnSuccess == acpiDevice->validateObject("XBQC"))
    {
        DbgLog("%s: ACPI device %s has XBCM/XBQC\n", this->getName(), acpiDevice->getName());
        _extended = true;
        UInt32 options = 0;
        if (kIOReturnSuccess == acpiDevice->evaluateInteger("XOPT", &options))
            _options = options;
    }

    if (!_extended)
    {
        if (kIOReturnSuccess == acpiDevice->validateObject("_BCM"))
            DbgLog("%s: ACPI device %s has _BCM\n", this->getName(), acpiDevice->getName());
        else
            ret = false;
        
        if (kIOReturnSuccess == acpiDevice->validateObject("_BQC"))
            DbgLog("%s: ACPI device %s has _BQC\n", this->getName(), acpiDevice->getName());	
        else
            ret = false;
    }

	return ret;
}


bool ACPIBacklightPanel::hasDOSMethod(IOACPIPlatformDevice * acpiDevice)
{
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
	bool ret = true;
	if (kIOReturnSuccess == acpiDevice->validateObject("_DOS"))
		DbgLog("%s: ACPI device %s has _DOS\n", this->getName(), acpiDevice->getName());
	else
		ret = false;
    	
	return ret;
}


bool ACPIBacklightPanel::hasSAVEMethod(IOACPIPlatformDevice * acpiDevice)
{
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
	bool ret = true;
	if (kIOReturnSuccess == acpiDevice->validateObject("SAVE"))
		DbgLog("%s: ACPI device %s has SAVE\n", this->getName(), acpiDevice->getName());
	else
		ret = false;
    
	return ret;
}

IOACPIPlatformDevice * ACPIBacklightPanel::getChildWithBacklightMethods(IOACPIPlatformDevice * GPUdevice)
{
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
	OSIterator * 		iter = NULL;
	OSObject *		entry;
    
	iter =  GPUdevice->getChildIterator(gIOACPIPlane);
	if (iter)
	{
		while ( true )
		{			
			entry = iter->getNextObject();
			if (NULL == entry)
				break;
			
			if (entry->metaCast("IOACPIPlatformDevice"))
			{
				IOACPIPlatformDevice * device = (IOACPIPlatformDevice *) entry;
				
				if (hasBacklightMethods(device))
				{
					IOLog("ACPIBacklight: Found Backlight Device: %s\n", device->getName());
					return device;
				}
			}
			else {
				DbgLog("%s: getChildWithBacklightMethods() Cast Error\n", this->getName());
			}
		} //end while
		iter->release();
		DbgLog("%s: getChildWithBacklightMethods() iterator end\n", this->getName());
	}
	return NULL;
}


OSArray * ACPIBacklightPanel::queryACPISupportedBrightnessLevels()
{
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
	OSObject * ret;
	backLightDevice->evaluateObject("_BCL", &ret);
	OSArray * data = OSDynamicCast(OSArray, ret);
	if (data)
	{
		DbgLog("%s: %s _BCL %d\n", this->getName(), backLightDevice->getName(), data->getCount() );
		return data;
	}
	else
    {
		DbgLog("%s: Cast Error _BCL is %s\n", this->getName(), ret->getMetaClass()->getClassName());
	}
	OSSafeRelease(ret);
	return NULL;
}


void ACPIBacklightPanel::setACPIBrightnessLevel(UInt32 level)
{
    //DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
	OSObject * ret = NULL;
	OSNumber * number = OSNumber::withNumber(level, 32);
    const char* method = _extended ? "XBCM" : "_BCM";

	if (number && kIOReturnSuccess == backLightDevice->evaluateObject(method, &ret, (OSObject**)&number, 1))
    {
        OSSafeRelease(ret);

        DbgLog("%s: setACPIBrightnessLevel %s(%u)\n", this->getName(), method, level);

        // just FYI... set RawBrightness property to actual current level
        setProperty(kRawBrightness, queryACPICurentBrightnessLevel(), 32);
    }
    else
        IOLog("ACPIBacklight: Error in setACPIBrightnessLevel %s(%u)\n",  method, level);
    OSSafeRelease(number);
}

void ACPIBacklightPanel::setBrightnessLevel(UInt32 level)
{
    //DbgLog("%s::%s(%d)\n", this->getName(), __FUNCTION__, level);

    UInt32 rem;
    UInt32 index = indexForLevel(level, &rem);
    UInt32 value = BCLlevels[index];
    //DbgLog("%s: level=%d, index=%d, value=%d\n", this->getName(), level, index, value);
    if (_extended)
    {
        // can set "in between" level
        UInt32 next = index+1;
        if (next < BCLlevelsCount)
        {
            // prorate the difference...
            UInt32 diff = BCLlevels[next] - value;
            value += (diff * rem) / kBacklightLevelMax;
            //DbgLog("%s: diff=%d, rem=%d, value=%d\n", this->getName(), diff, rem, value);
        }
    }
    setACPIBrightnessLevel(value);
}

void ACPIBacklightPanel::setBrightnessLevelSmooth(UInt32 level)
{
    //DbgLog("%s::%s(%d)\n", this->getName(), __FUNCTION__, level);

    //DbgLog("%s: _from_value=%d, _value=%d\n", this->getName(), _from_value, _value);

    if (_smoothTimer && _extended)
    {
        IORecursiveLockLock(_lock);

        if (level != _value)
        {
            // find appropriate movemement params in smoothData
            int diff = abs((int)level - _from_value);
            _smoothIndex = countof(smoothData)-1; // defensive
            for (int i = 0; i < countof(smoothData); i++)
            {
                if (diff <= smoothData[i].delta)
                {
                    _smoothIndex = i;
                    break;
                }
            }
            // kick off timer if not already started
            bool start = (_from_value == _value);
            _value = level;
            if (start)
                onSmoothTimer();
        }
        else if (_from_value == _value)
        {
            // in the case of already set to that value, set it for sure
            setBrightnessLevel(_value);
        }

        IORecursiveLockUnlock(_lock);
    }
    else
    {
        _from_value = _value = level;
        setBrightnessLevel(_value);
    }
}

void ACPIBacklightPanel::onSmoothTimer()
{
    //DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);

    IORecursiveLockLock(_lock);

    DbgLog("%s::%s(): _from_value=%d, _value=%d, _smoothIndex=%d\n", this->getName(), __FUNCTION__, _from_value, _value, _smoothIndex);

    // adjust smooth index based on current delta
    int diff = abs(_value - _from_value);
    if (_smoothIndex > 0 && diff <= smoothData[_smoothIndex-1].delta)
        --_smoothIndex;

    // move _from_value in the direction of _value
    SmoothData* data = &smoothData[_smoothIndex];
    if (_value > _from_value)
        _from_value = min(_value, _from_value + data->step);
    else
        _from_value = max(_value, _from_value - data->step);

    // set new brigthness level
    //DbgLog("%s::%s(): _from_value=%d, _value=%d\n", this->getName(), __FUNCTION__, _from_value, _value);
    setBrightnessLevel(_from_value);
    // set new timer if not reached desired brightness previously set
    if (_from_value != _value)
        _smoothTimer->setTimeoutUS(data->timeout);

    IORecursiveLockUnlock(_lock);
}

void ACPIBacklightPanel::saveACPIBrightnessLevel(UInt32 level)
{
    //DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
	OSObject * ret = NULL;
	OSNumber * number = OSNumber::withNumber(level, 32);
    
	if (number && kIOReturnSuccess == backLightDevice->evaluateObject("SAVE", &ret, (OSObject**)&number,1))
    {
        OSSafeRelease(ret);

        //DbgLog("%s: saveACPIBrightnessLevel SAVE(%u)\n", this->getName(), (unsigned int) level);
    }
    else
        IOLog("ACPIBacklight: Error in saveACPIBrightnessLevel SAVE(%u)\n", (unsigned int) level);
    OSSafeRelease(number);
}

void ACPIBacklightPanel::saveACPIBrightnessLevelNVRAM(UInt32 level1)
{
    //DbgLog("%s::%s(): level=%d\n", this->getName(),__FUNCTION__, level1);

    UInt16 level = (UInt16)level1;
    if (IORegistryEntry *nvram = OSDynamicCast(IORegistryEntry, fromPath("/options", gIODTPlane)))
    {
        if (const OSSymbol* symbol = OSSymbol::withCString(kACPIBacklightLevel))
        {
            if (OSData* number = OSData::withBytes(&level, sizeof(level)))
            {
                //DbgLog("%s: saveACPIBrightnessLevelNVRAM got nvram %p\n", this->getName(), nvram);
                if (!nvram->setProperty(symbol, number))
                {
                    DbgLog("%s: nvram->setProperty failed\n", this->getName());
                }
                number->release();
            }
            symbol->release();
        }
        nvram->release();
    }
}

UInt32 ACPIBacklightPanel::loadFromNVRAM(void)
{
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);

    IORegistryEntry* nvram = IORegistryEntry::fromPath("/chosen/nvram", gIODTPlane);
    if (!nvram)
    {
        DbgLog("%s: no /chosen/nvram, trying IODTNVRAM\n", this->getName());
        // probably booting w/ Clover
        if (OSDictionary* matching = serviceMatching("IODTNVRAM"))
        {
            nvram = waitForMatchingService(matching, 1000000000ULL * 15);
            matching->release();
        }
    }
    else DbgLog("%s: have nvram from /chosen/nvram\n", this->getName());
    UInt32 val = -1;
    if (nvram)
    {
        // need to serialize as getProperty on nvram does not work
        if (OSSerialize* serial = OSSerialize::withCapacity(0))
        {
            nvram->serializeProperties(serial);
            if (OSDictionary* props = OSDynamicCast(OSDictionary, OSUnserializeXML(serial->text())))
            {
                if (OSData* number = OSDynamicCast(OSData, props->getObject(kACPIBacklightLevel)))
                {
                    val = 0;
                    unsigned l = number->getLength();
                    if (l <= sizeof(val))
                        memcpy(&val, number->getBytesNoCopy(), l);
                    DbgLog("%s: read level from nvram = %d\n", this->getName(), val);
                    //number->release();
                }
                else DbgLog("%s: no acpi-backlight-level in nvram\n", this->getName());
                props->release();
            }
            serial->release();
        }
        nvram->release();
    }
    return val;
}

UInt32 ACPIBacklightPanel::queryACPICurentBrightnessLevel()
{
    //DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
	UInt32 level = minAC;
    const char* method = _extended ? "XBQC" : "_BQC";
	if (kIOReturnSuccess == backLightDevice->evaluateInteger(method, &level))
	{
		//DbgLog("%s: queryACPICurentBrightnessLevel %s = %d\n", this->getName(), method, level);
        
        OSBoolean * useIdx = OSDynamicCast(OSBoolean, getProperty("BQC use index"));
        if (useIdx && useIdx->isTrue())
        {
            OSArray * levels = queryACPISupportedBrightnessLevels();
            if (levels)
            {
                OSNumber *num = OSDynamicCast(OSNumber, levels->getObject(level));
                if (num)
                    level = num->unsigned32BitValue();
                levels->release();
            }
        }
        //DbgLog("%s: queryACPICurentBrightnessLevel returning %d\n", this->getName(), level);
	}
	else {
		IOLog("ACPIBacklight: Error in queryACPICurentBrightnessLevel %s\n", method);
	}
    //some laptops didn't return anything on startup, return then max value (first entry in _BCL):
	return level;
}


/*
 * Switch from direct hardware controled to software controled mode
 */
void ACPIBacklightPanel::getDeviceControl()
{
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
	OSNumber * number = OSNumber::withNumber(0x4, 32); //bit 2 = 1
	OSObject * ret = NULL;
	
	if (number && kIOReturnSuccess == gpuDevice->evaluateObject("_DOS", &ret, (OSObject**)&number, 1))
    {
        OSSafeRelease(ret);
        DbgLog("%s: BIOS control disabled: _DOS\n", this->getName());
    }
    else
       IOLog("ACPIBacklight: Error in getDeviceControl _DOS\n");
    OSSafeRelease(number);
}


SInt32 ACPIBacklightPanel::findIndexForLevel(SInt32 BCLvalue)
{
	for (int i = 0; i < BCLlevelsCount; i++)
	{
		if (BCLlevels[i] >= BCLvalue)
		{
            i = i > 0 ? i-1 : 0;
			DbgLog("%s: findIndexForLevel(%d) is %d\n", this->getName(), BCLvalue, i);
			return i;
		}
	}
    DbgLog("%s: findIndexForLevel(%d) did not find\n", this->getName(), BCLvalue);
	return BCLlevelsCount-1;
}


SInt32 ACPIBacklightPanel::setupIndexedLevels()
{
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
	OSNumber * num;
	OSArray * levels = queryACPISupportedBrightnessLevels();
	if (levels)
	{
		BCLlevelsCount = levels->getCount();
		
		if (BCLlevelsCount < 3)
			return 0;
		
		//verify the types of objects is good once for all
		for (int i = 0; i< BCLlevelsCount; i++) {
			if (!OSDynamicCast(OSNumber, levels->getObject(i)))
				return 0;
		}
		
        //TODO : manage the case when the list has no order! Linux do that
		//test the order of the list
		SInt32 min, max;
		num = OSDynamicCast(OSNumber, levels->getObject(2));
		min = num->unsigned32BitValue();
		num = OSDynamicCast(OSNumber, levels->getObject(BCLlevelsCount-1));
		max = num->unsigned32BitValue();
		
		if (max < min) //list is reverted !
		{
			BCLlevels = IONew(SInt32, BCLlevelsCount);
			for (int i = 0; i< BCLlevelsCount; i++) {
				num = OSDynamicCast(OSNumber, levels->getObject(BCLlevelsCount -1 -i));
				BCLlevels[i] = num->unsigned32BitValue();
			}
		}
		else
		{
			BCLlevelsCount = BCLlevelsCount -2;
			BCLlevels = IONew(SInt32, BCLlevelsCount);
			for (int i = 0; i< BCLlevelsCount; i++) {
				num = OSDynamicCast(OSNumber, levels->getObject(i+2));
				BCLlevels[i] = num->unsigned32BitValue();
			}
		}

		//2 first items are min on ac and max on bat
		num = OSDynamicCast(OSNumber, levels->getObject(0));
		minAC = findIndexForLevel(num->unsigned32BitValue());
		setDebugProperty("BCL: Min on AC", num);
		num = OSDynamicCast(OSNumber, levels->getObject(1));
		maxBat = findIndexForLevel(num->unsigned32BitValue());
		setDebugProperty("BCL: Max on Bat", num);
		setDebugProperty("Brightness Control Levels", levels);
        levels->release();
		
		return BCLlevelsCount -1;
	}
	return 0;
}


#pragma mark -
#pragma mark AC DC managment for init
#pragma mark -


IOService * ACPIBacklightPanel::getBatteryDevice()
{
	DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
    OSDictionary * matching = IOService::serviceMatching("IOPMPowerSource");
	OSIterator *   iter = NULL;
	IOService * bat = NULL;
	
	if (matching)
	{
		DbgLog("%s: getBatteryDevice() serviceMatching OK\n", this->getName());
		iter = IOService::getMatchingServices(matching);
		matching->release();
	}
	
	if (iter)
	{
		DbgLog("%s: getBatteryDevice() iter OK\n", this->getName());
		
		bat = OSDynamicCast(IOService, iter->getNextObject());
		if (bat)
		{
			DbgLog("%s: getBatteryDevice() bat is of class %s\n", this->getName(), bat->getMetaClass()->getClassName());	
		}
		
		iter->release();
	}
	
	return bat;
}


bool ACPIBacklightPanel::getACStatus()
{
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
	IOService * batteryDevice = getBatteryDevice();
	
	if (NULL != batteryDevice)
	{
		OSObject * obj = batteryDevice->getProperty("ExternalConnected");
		OSBoolean * status = OSDynamicCast(OSBoolean, obj);
        if (status)
        {
            DbgLog("%s: getACStatus() AC is %d\n", this->getName(), status->getValue());
            return status->getValue();
        }
        else
            DbgLog("%s: getACStatus() unable to get \"ExternalConnected\" property\n", this->getName());
	}
    return true;
}


void ACPIBacklightPanel::processWorkQueue(IOInterruptEventSource *, int)
{
    DbgLog("%s::%s() _workPending=%x\n", this->getName(),__FUNCTION__, _workPending);
    
    IORecursiveLockLock(_lock);
    if (_workPending & kWorkSave)
        saveACPIBrightnessLevelNVRAM(_committed_value);
    if (_workPending & kWorkSetBrightness)
        setBrightnessLevel(_committed_value);
    _workPending = 0;
    IORecursiveLockUnlock(_lock);
}

void ACPIBacklightPanel::scheduleWork(unsigned newWork)
{
    IORecursiveLockLock(_lock);
    _workPending |= newWork;
    _workSource->interruptOccurred(0, 0, 0);
    IORecursiveLockUnlock(_lock);
}

IOReturn ACPIBacklightPanel::setPropertiesGated(OSObject* props)
{
    //DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);

    OSDictionary* dict = OSDynamicCast(OSDictionary, props);
    if (!dict)
        return kIOReturnSuccess;

    // set brightness
	if (OSNumber* num = OSDynamicCast(OSNumber, dict->getObject(kRawBrightness)))
    {
		UInt32 raw = (int)num->unsigned32BitValue();
        setACPIBrightnessLevel(raw);
        setProperty(kRawBrightness, queryACPICurentBrightnessLevel(), 32);
    }

    for (int i = 0; i < countof(smoothData); i++)
    {
        char buf[kSmoothBufSize];
        snprintf(buf, sizeof(buf), kSmoothDelta, i);
        if (OSNumber* num = OSDynamicCast(OSNumber, dict->getObject(buf)))
        {
            smoothData[i].delta = (int)num->unsigned32BitValue();
            setProperty(buf, smoothData[i].delta, 32);
        }
        snprintf(buf, sizeof(buf), kSmoothStep, i);
        if (OSNumber* num = OSDynamicCast(OSNumber, dict->getObject(buf)))
        {
            smoothData[i].step = (int)num->unsigned32BitValue();
            setProperty(buf, smoothData[i].step, 32);
        }
        snprintf(buf, sizeof(buf), kSmoothTimeout, i);
        if (OSNumber* num = OSDynamicCast(OSNumber, dict->getObject(buf)))
        {
            smoothData[i].timeout = (int)num->unsigned32BitValue();
            setProperty(buf, smoothData[i].timeout, 32);
        }
    }
    
#ifdef DEBUG
    // special cycle test
    if (OSNumber* num = OSDynamicCast(OSNumber, dict->getObject("CycleTest")))
    {
        IOLog("%s: CycleTest UP\n", this->getName());
		UInt32 raw = (int)num->unsigned32BitValue();
        for (int i = 1; i <= raw; i++)
        {
            setACPIBrightnessLevel(i);
            IOSleep(16);
        }
        IOSleep(500);
        IOLog("%s: CycleTest DOWN\n", this->getName());
        for (int i = raw; i > 0; i--)
        {
            setACPIBrightnessLevel(i);
            IOSleep(16);
        }
    }
    // allow setting of KLVX at runtime
	if (OSNumber* num = OSDynamicCast(OSNumber, dict->getObject("KLVX")))
    {
		UInt32 raw = (int)num->unsigned32BitValue();
        setKLVX(raw);
        setProperty("KLVX", raw, 32);
    }
#endif
    return kIOReturnSuccess;
}

#ifdef DEBUG
void ACPIBacklightPanel::setKLVX(UInt32 levx)
{
    //DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
	OSObject * ret = NULL;
	OSNumber * number = OSNumber::withNumber(levx, 32);
    const char* method = "DEB1";
    
	if (number && kIOReturnSuccess == backLightDevice->evaluateObject(method, &ret, (OSObject**)&number, 1))
    {
        OSSafeRelease(ret);
    }
    else
        IOLog("ACPIBacklight: Error in setKLVX %s(%u)\n",  method, levx);
    OSSafeRelease(number);
}
#endif

IOReturn ACPIBacklightPanel::setProperties(OSObject* props)
{
    //DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);

    if (_cmdGate)
    {
        // syncronize through workloop...
        IOReturn result = _cmdGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &ACPIBacklightPanel::setPropertiesGated), props);
        if (kIOReturnSuccess != result)
            return result;
    }
    return kIOReturnSuccess;
}
