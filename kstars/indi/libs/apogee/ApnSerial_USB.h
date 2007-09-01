/*  Apogee Control Library

Copyright (C) 2001-2006 Dave Mills  (rfactory@theriver.com)

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

*/
// ApnSerial_USB.h: interface for the CApnSerial_USB class.
//
//////////////////////////////////////////////////////////////////////

#if !defined(AFX_APNSERIAL_USB_H__D7A1A328_6505_438F_BCCE_FA3F3B5EECC2__INCLUDED_)
#define AFX_APNSERIAL_USB_H__D7A1A328_6505_438F_BCCE_FA3F3B5EECC2__INCLUDED_

#include "ApnSerial.h"

class CApnSerial_USB : public CApnSerial  
{
public:
	CApnSerial_USB();
	virtual ~CApnSerial_USB();

	bool InitPort( unsigned long	CamIdA,
				   unsigned short	CamIdB,
				   unsigned short	SerialId );

	bool ClosePort();

	bool GetBaudRate( unsigned long *BaudRate );
	bool SetBaudRate( unsigned long BaudRate );

	bool GetFlowControl( Apn_SerialFlowControl *FlowControl );
	bool SetFlowControl( Apn_SerialFlowControl FlowControl );

	bool GetParity( Apn_SerialParity *Parity );
	bool SetParity( Apn_SerialParity Parity );

	bool Read( char			  *ReadBuffer,
			   unsigned short *ReadCount );

	bool Write( char		   *WriteBuffer,
				unsigned short WriteCount );

};

#endif // !defined(AFX_APNSERIAL_USB_H__D7A1A328_6505_438F_BCCE_FA3F3B5EECC2__INCLUDED_)
