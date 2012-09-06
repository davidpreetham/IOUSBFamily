#include <IOKit/usb/IOUSBLog.h>

#include "AppleUSBEHCI.h"

// these could go in some public header, or can just be copied by hand
enum
{
    kEHCITestMode_Off		= 0,
    kEHCITestMode_J_State	= 1,
    kEHCITestMode_K_State 	= 2,
    kEHCITestMode_SE0_NAK	= 3,
    kEHCITestMode_Packet	= 4,
    kEHCITestMode_ForceEnable	= 5,
    kEHCITestMode_Start		= 10,
    kEHCITestMode_End		= 11
};

IOReturn
AppleUSBEHCI::EnterTestMode()
{
    UInt32	usbcmd, usbsts;
    UInt8	numPorts;
    int		i;
    
    USBLog(1, "%s[%p]::EnterTestMode", getName(), this);
    // see section 4.14 of the EHCI spec
    
    // disable the periodic and async schedules
    usbcmd = USBToHostLong(_pEHCIRegisters->USBCMD);
    usbcmd &= ~kEHCICMDAsyncEnable;
    usbcmd &= ~kEHCICMDPeriodicEnable;
    _pEHCIRegisters->USBCMD = HostToUSBLong(usbcmd);
    USBLog(1, "%s[%p]::EnterTestMode - async and periodic lists disabled", getName(), this);
    
    // suspend all enabled ports
    GetNumberOfPorts(&numPorts);
    USBLog(1, "%s[%p]::EnterTestMode - suspending %d ports", getName(), this, numPorts);
    for (i=0; i < numPorts; i++)
    {
	UInt32 portStat;
	portStat = USBToHostLong(_pEHCIRegisters->PortSC[i]);
	if (portStat & kEHCIPortSC_Owner)
	{
	    USBLog(1, "%s[%p]::EnterTestMode - port %d owned by OHCI", getName(), this, i);
	    // should i return an error here? probably not
	}
	else if (portStat & kEHCIPortSC_Enabled)
	{
	    portStat |= kEHCIPortSC_Suspend;
	    _pEHCIRegisters->PortSC[i] = HostToUSBLong(portStat);
	    USBLog(1, "%s[%p]::EnterTestMode - port %d now suspended", getName(), this, i);
	}
	else
	{
	    USBLog(1, "%s[%p]::EnterTestMode - port %d not enabled", getName(), this, i);
	}
    }
    
    // set run/stop
    usbcmd &= ~kEHCICMDRunStop;
    _pEHCIRegisters->USBCMD = HostToUSBLong(usbcmd);
    _ehciBusState = kEHCIBusStateOff;
    USBLog(1, "%s[%p]::EnterTestMode - HC stop set, waiting for halted", getName(), this);
    
    // wait for halted bit
    do
    {
	usbsts = USBToHostLong(_pEHCIRegisters->USBSTS);
    } while (!(usbsts & kEHCIHCHaltedBit));
    USBLog(1, "%s[%p]::EnterTestMode - HC halted - now in test mode", getName(), this);
    
    _testModeEnabled = true;
    return kIOReturnSuccess;
}



IOReturn
AppleUSBEHCI::PlacePortInMode(UInt32 port, UInt32 mode)
{
    UInt32	portStat;
    UInt8	numPorts;
    
    USBLog(1, "%s[%p]::PlacePortinMode(port %d, mode %d)", getName(), this, port, mode);
    // see section 4.14 of the EHCI spec
    if (!_testModeEnabled)
    {
	USBLog(1, "%s[%p]::PlacePortinMode - ERROR test mode not enabled", getName(), this);
	return kIOReturnInternalError;
    }


    numPorts = USBToHostLong(_pEHCICapRegisters->HCSParams) & kEHCINumPortsMask;
    if (port >= numPorts)
    {
	USBLog(1, "%s[%p]::PlacePortinMode - ERROR invalid port %d", getName(), this, port);
	return kIOReturnInternalError;
    }
	
    portStat = USBToHostLong(_pEHCIRegisters->PortSC[port]);
    if (portStat & kEHCIPortSC_Owner)
    {
	USBLog(1, "%s[%p]::PlacePortinMode - ERROR port %d owned by OHCI", getName(), this, port);
	return kIOReturnInternalError;
    }
   
    USBLog(1, "%s[%p]::PlacePortinMode - old portStat = %x", getName(), this, portStat);
    portStat &= ~kEHCIPortSC_TestControl;
    portStat |= (mode << kEHCIPortSC_TestControlPhase);
    USBLog(1, "%s[%p]::PlacePortinMode - new portStat = %x", getName(), this, portStat);
    _pEHCIRegisters->PortSC[port] = HostToUSBLong(portStat);
    
    return kIOReturnSuccess;
}



IOReturn
AppleUSBEHCI::LeaveTestMode()
{
    UInt32	usbcmd, usbsts;
    USBLog(1, "%s[%p]::LeaveTestMode", getName(), this);
    // see section 4.14 of the EHCI spec

    // make sure we are halted
    usbcmd = USBToHostLong(_pEHCIRegisters->USBCMD);
    usbsts = USBToHostLong(_pEHCIRegisters->USBSTS);
    
    if (!(usbsts & kEHCIHCHaltedBit))
	return kIOReturnInternalError;
    
    // place controller in reset
    usbcmd |= kEHCICMDHCReset;
    _pEHCIRegisters->USBCMD = HostToUSBLong(usbcmd);
    USBLog(1, "%s[%p]::LeaveTestMode - leaving with HC in reset", getName(), this);

    // do i need to reconfigure here?
    
    _testModeEnabled = false;
    return kIOReturnSuccess;
}



// this is the one entry point for all methods in this file. this is the only virtual method.
IOReturn
AppleUSBEHCI::UIMSetTestMode(UInt32 mode, UInt32 port)
{
    IOReturn ret = kIOReturnInternalError;
    
    USBLog(1, "%s[%p]::UIMSetTestMode(%d, %d)", getName(), this, mode, port);
    
    switch (mode)
    {
	case kEHCITestMode_Off:
	case kEHCITestMode_J_State:
	case kEHCITestMode_K_State:
	case kEHCITestMode_SE0_NAK:
	case kEHCITestMode_Packet:
	case kEHCITestMode_ForceEnable:
	    if (_testModeEnabled)
		ret = PlacePortInMode(port, mode);
	    break;

	case kEHCITestMode_Start:
	    ret = EnterTestMode();
	    break;
	    
	case kEHCITestMode_End:
	    ret = LeaveTestMode();
	    break;
    }

    return ret;
}