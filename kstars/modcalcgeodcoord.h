/***************************************************************************
                          modcalcgeodcoord.h  -  description
                             -------------------
    begin                : Tue Jan 15 2002
    copyright            : (C) 2002 by Pablo de Vicente
    email                : vicente@oan.es
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef MODCALCGEODCOORD_H
#define MODCALCGEODCOORD_H

#include <qvbox.h>

/**
  *@author Pablo de Vicente
  * Class which implements the KStars calculator module to compute
  * Geodetic coordinates to/from Cartesian coordinates.
  *  
  * Inherits QVBox
  */

class QWidget;
class VBox;
class QLineEdit;
class QRadioButton;
class dmsBox;
class GeoLocation;

class modCalcGeodCoord : public QVBox  {

	Q_OBJECT
	public:
	
		modCalcGeodCoord(QWidget *p, const char *n);
		~modCalcGeodCoord();
	
		void genGeoCoords(void);
		void getCartGeoCoords (void);
		void getSphGeoCoords (void);
		void showSpheGeoCoords(void);
		void showCartGeoCoords(void);

	public slots:
	
		void slotComputeGeoCoords (void);
		void slotClearGeoCoords (void);		
		void setEllipsoid(int i);

	private:

		QRadioButton *cartRadio, *spheRadio;
		QVBox *vbox, *rightBox;
		QLineEdit *xGeoName, *yGeoName, *zGeoName, *altGeoName;
		dmsBox *timeBox, *dateBox, *lonGeoBox, *latGeoBox;

		GeoLocation *geoPlace;

};
	
#endif
