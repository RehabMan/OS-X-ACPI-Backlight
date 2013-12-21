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

#include "ACPIBacklight.h"
#include "Debug.h"

#define super IODisplayParameterHandler

OSDefineMetaClassAndStructors(ACPIBacklightPanel, IODisplayParameterHandler)

#define kACPIBacklightLevel "acpi-backlight-level"
#define kRawBrightness "RawBrightness"

#define kBacklightLevelMin  0
#define kBacklightLevelMax  0x400

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
    
    findDevices(provider);
    getDeviceControl();
    hasSaveMethod = hasSAVEMethod(backLightDevice);
    min = 0;
    max = setupIndexedLevels();

    // load and set default brightness level
    UInt32 value = loadFromNVRAM();
    DbgLog("ACPIBacklightPanel: loadFromNVRAM returns %d\n", value);
    UInt32 index;
    if (-1 != value)
    {
        DbgLog("ACPIBacklightPanel: setting to value from nvram %d\n", value);
        _value = value;
        setBrightnessLevel(_value);
    }
    else
    {
        DbgLog("ACPIBacklightPanel: no value from nvram, setting value from current\n");
        index = findIndexForLevel(queryACPICurentBrightnessLevel());
        _value = levelForIndex(index);
        // just FYI... set RawBrightness property to actual current level
        setProperty(kRawBrightness, queryACPICurentBrightnessLevel(), 32);
    }

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

    // add timer for smooth fade ins
    _smoothTimer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &ACPIBacklightPanel::onSmoothTimer));
    if (_smoothTimer)
        workLoop->addEventSource(_smoothTimer);

    _cmdGate = IOCommandGate::commandGate(this);
    if (_cmdGate)
        workLoop->addEventSource(_cmdGate);

    registerService();

    DbgLog("%s: min = %u, max = %u, index = %u, value = %u\n", this->getName(), min, max, index, _value);
	IOLog("%s: Version 1.2\n", this->getName());

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

bool ACPIBacklightPanel::setDisplay( IODisplay * display )
{    
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);

    // retain new display (also allow setting to same instance as previous)
    if (display)
        display->retain();
    OSSafeRelease(_display);
    _display = display;
    // update brigthness levels
    if (_display)
    {
        doUpdate();
        setBrightnessLevel(_value);
    }
    return true;
}


bool ACPIBacklightPanel::doIntegerSet( OSDictionary * params, const OSSymbol * paramName, UInt32 value )
{
    //DbgLog("ACPIBacklight: doIntegerSet(\"%s\", %d)\n", paramName->getCStringNoCopy(), value);
    if ( gIODisplayBrightnessKey->isEqualTo(paramName))
    {   
        DbgLog("%s::%s(%s) map %d -> %d\n", this->getName(),__FUNCTION__, paramName->getCStringNoCopy(), value, indexForLevel(value));
        _value = value;
        setBrightnessLevel(_value);
        // save to NVRAM in work loop
        _workSource->interruptOccurred(0, 0, 0);
        return true;
    }
    else if (gIODisplayParametersCommitKey->isEqualTo(paramName))
    {
        UInt32 index = indexForLevel(value);
        DbgLog("%s::%s(%s) map %d -> %d\n", this->getName(),__FUNCTION__, paramName->getCStringNoCopy(), value, index);
        IODisplay::setParameter(params, gIODisplayBrightnessKey, value);
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
    //DbgLog("Entering %s::%s()\n", this->getName(),__FUNCTION__);
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
		DbgLog("%s: ACPILevel min %d, max %d, value %d\n", this->getName(), min, max, _value);
		
        IODisplay::addParameter(backlightParams, gIODisplayBrightnessKey, kBacklightLevelMin, kBacklightLevelMax);
        IODisplay::setParameter(backlightParams, gIODisplayBrightnessKey, _value);

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

    //DbgLog("Leaving %s::%s()\n", this->getName(),__FUNCTION__);
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
        
        OSBoolean * showPath = OSDynamicCast(OSBoolean, getProperty("Show Device Paths"));
        if (showPath && showPath->isTrue())
        {
            OSString * path;
            if (gpuDevice != backLightDevice)
            {
                OSArray * devicePaths = OSArray::withCapacity(2);
                path = getACPIPath(gpuDevice);
                IOLog("%s: ACPI Method _DOS found. Device path: %s\n", this->getName(), path->getCStringNoCopy());
                devicePaths->setObject(path);
                path->release();
                path = getACPIPath(backLightDevice);
                IOLog("%s: ACPI Methods _BCL _BCM _BQC found. Device path: %s\n", this->getName(), path->getCStringNoCopy());
                devicePaths->setObject(path);
                path->release();
                setProperty("ACPI Devices Paths", devicePaths);
            }
            else
            {
                path = getACPIPath(backLightDevice);
                IOLog("%s: ACPI Methods _DOS _BCL _BCM _BQC found. Device path: %s\n", this->getName(), path->getCStringNoCopy());
                setProperty("ACPI Device Path", path);
                path->release();
            }
        }
        
    }
    return true;
}


#define lgth 512
OSString * ACPIBacklightPanel::getACPIPath(IOACPIPlatformDevice * acpiDevice)
{
    OSString * separator = OSString::withCStringNoCopy(".");
    OSArray * array = OSArray::withCapacity(10);
    
    char devicePath[lgth];
    bzero(devicePath, lgth);
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
        for (int i = array->getCount()-2; ((i>=0) || ((offset + str->getLength()) > lgth)) ; i--)
        {
            str = OSDynamicCast(OSString, array->getObject(i));
            strncpy(devicePath + offset, str->getCStringNoCopy(), str->getLength());
            offset += str->getLength();
        }
        array->release();
        separator->release();
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
					IOLog("%s: Found Backlight Device: %s\n", this->getName(), device->getName());
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
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
	OSObject * ret = NULL;
	OSNumber * number = OSNumber::withNumber(level, 32);
    const char* method = _extended ? "XBCM" : "_BCM";

	if (number && kIOReturnSuccess == backLightDevice->evaluateObject(method, &ret, (OSObject**)&number, 1))
    {
        OSSafeRelease(ret);
        number->release();
        
        DbgLog("%s: setACPIBrightnessLevel %s(%u)\n", this->getName(), method, level);

        // just FYI... set RawBrightness property to actual current level
        setProperty(kRawBrightness, queryACPICurentBrightnessLevel(), 32);
    }
    else
        IOLog("%s: Error in setACPIBrightnessLevel %s(%u)\n", this->getName(), method, level);
}

void ACPIBacklightPanel::setBrightnessLevel(UInt32 level)
{
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);

    UInt32 rem;
    UInt32 index = indexForLevel(level, &rem);
    UInt32 value = BCLlevels[index];
    DbgLog("ACPIBack: level=%d, index=%d, value=%d\n", level, index, value);
    if (_extended)
    {
        // can set "in between" level
        UInt32 next = index+1;
        if (next < BCLlevelsCount)
        {
            // prorate the difference...
            UInt32 diff = BCLlevels[next] - value;
            value += (diff * rem) / kBacklightLevelMax;
            DbgLog("ACPIBack: diff=%d, rem=%d, value=%d\n", diff, rem, value);
        }
    }
    setACPIBrightnessLevel(value);
}

void ACPIBacklightPanel::saveACPIBrightnessLevel(UInt32 level)
{
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
	OSObject * ret = NULL;
	OSNumber * number = OSNumber::withNumber(level, 32);
    
	if (number && kIOReturnSuccess == backLightDevice->evaluateObject("SAVE", &ret, (OSObject**)&number,1))
    {
        OSSafeRelease(ret);
        number->release();

        DbgLog("%s: saveACPIBrightnessLevel SAVE(%u)\n", this->getName(), (unsigned int) level);
    }
    else
        IOLog("%s: Error in saveACPIBrightnessLevel SAVE(%u)\n", this->getName(), (unsigned int) level);
}

#include <IOKit/IONVRAM.h>
#include <IOKit/IOLib.h>

void ACPIBacklightPanel::saveACPIBrightnessLevelNVRAM(UInt32 level1)
{
    DbgLog("%s::%s(): level=%d\n", this->getName(),__FUNCTION__, level1);

    UInt16 level = (UInt16)level1;
    if (IORegistryEntry *nvram = OSDynamicCast(IORegistryEntry, fromPath("/options", gIODTPlane)))
    {
        if (const OSSymbol* symbol = OSSymbol::withCString(kACPIBacklightLevel))
        {
            if (OSData* number = OSData::withBytes(&level, sizeof(level)))
            {
                DbgLog("saveACPIBrightnessLevelNVRAM got nvram %p\n", nvram);
                if (!nvram->setProperty(symbol, number))
                    DbgLog("nvram->setProperty failed\n");
                number->release();
            }
            symbol->release();
        }
        nvram->release();
    }
}

UInt32 ACPIBacklightPanel::loadFromNVRAM(void)
{
    IORegistryEntry* nvram = IORegistryEntry::fromPath("/chosen/nvram", gIODTPlane);
    if (!nvram)
    {
        DbgLog("ACPIBacklight: no /chosen/nvram, trying IODTNVRAM");
        // probably booting w/ Clover
        if (OSDictionary* matching = serviceMatching("IODTNVRAM"))
        {
            nvram = waitForMatchingService(matching, 1000000000ULL * 15);
            matching->release();
        }
    }
    else DbgLog("ACPIBacklight: have nvram from /chosen/nvram");
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
                    DbgLog("read level from nvram = %d\n", val);
                    //number->release();
                }
                else DbgLog("no acpi-backlight-level in nvram\n");
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
    DbgLog("%s::%s()\n", this->getName(),__FUNCTION__);
    
	UInt32 level = minAC;
    const char* method = _extended ? "XBQC" : "_BQC";
	if (kIOReturnSuccess == backLightDevice->evaluateInteger(method, &level))
	{
		DbgLog("%s: queryACPICurentBrightnessLevel %s = %d\n", this->getName(), method, level);
        
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
        DbgLog("%s: queryACPICurentBrightnessLevel returning %d\n", this->getName(), level);
	}
	else {
		IOLog("%s: Error in queryACPICurentBrightnessLevel %s\n", this->getName(), method);
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
        number->release();

        DbgLog("%s: BIOS control disabled: _DOS\n", this->getName());
    }
    else
       IOLog("%s: Error in getDeviceControl _DOS\n", this->getName()); 
}


SInt32 ACPIBacklightPanel::findIndexForLevel(SInt32 BCLvalue)
{
	for (SInt32 i = BCLlevelsCount-1; i>=0 ; i--)
	{
		if (BCLlevels[i] == BCLvalue)
		{
			DbgLog("%s: getIndexForLevel(%d) is %d\n", this->getName(), (int) BCLvalue, (int) i);
			return i;
		}
	}
	IOLog("%s: getIndexForLevel(%d) not found in _BCL table !\n", this->getName(), (int) BCLvalue);
	return 0;
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
		setProperty("Brightness Control Levels", levels);
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
   saveACPIBrightnessLevelNVRAM(_value);
}

void ACPIBacklightPanel::onSmoothTimer()
{
    //TODO: gradually fade in new brightness
}

IOReturn ACPIBacklightPanel::setPropertiesGated(OSObject* props)
{
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
    return kIOReturnSuccess;
}

IOReturn ACPIBacklightPanel::setProperties(OSObject* props)
{
    if (_cmdGate)
    {
        // syncronize through workloop...
        IOReturn result = _cmdGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &ACPIBacklightPanel::setPropertiesGated), props);
        if (kIOReturnSuccess != result)
            return result;
    }
    return kIOReturnSuccess;
}
