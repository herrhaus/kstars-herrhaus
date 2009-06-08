/***************************************************************************
                          starcomponent.h  -  K Desktop Planetarium
                             -------------------
    begin                : 2005/14/08
    copyright            : (C) 2005 by Thomas Kabelmann
    email                : thomas.kabelmann@gmx.de
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef STARCOMPONENT_H
#define STARCOMPONENT_H

/**
 *@class StarComponent 
 *
 *@short Represents the stars on the sky map. For optimization reasons the
 *stars are not separate objects and are stored in a list. 
 *
 *The StarComponent class manages all stars drawn in KStars. While it
 *handles all stars having names using its own member methods, it
 *shunts the responsibility of unnamed stars to the class
 *'DeepStarComponent', objects of which it maintains.
 *
 *@author Thomas Kabelmann
 *@author Akarsh Simha
 *@version 1.0
 */

#include "listcomponent.h"
#include "kstarsdatetime.h"
#include "ksnumbers.h"
#include "starblock.h"
#include "skylabel.h"
#include "typedef.h"
#include "highpmstarlist.h"
#include "skyobjects/starobject.h"
#include "binfilehelper.h"
#include "starblockfactory.h"
#include "skymesh.h"

class SkyComponent;
class KStarsData;
class SkyMesh;
class StarObject;
class SkyLabeler;
class KStarsSplash;
class BinFileHelper;
class StarBlockFactory;
class MeshIterator;

#define MAX_LINENUMBER_MAG 90

class StarComponent: public ListComponent
{

 protected:

    StarComponent( SkyComponent* );

 public:

    virtual ~StarComponent();

    /**@short Create an instance of StarComponent
     */
    static StarComponent *Create( SkyComponent* );

    /**@return the instance of StarComponent if already created, NULL otherwise
     */    
    static StarComponent *Instance() { return pinstance; }

    //This function is empty; we need that so that the JiT update 
    //is the only one beiong used.
    void update( KStarsData *data, KSNumbers *num );

    bool selected();

    void reindex( KSNumbers *num );

    void draw( QPainter& psky );

    /* @short draw all the labels in the prioritized LabelLists and then
     * clear the LabelLists.
     */
    void drawLabels( QPainter& psky );

    void init(KStarsData *data);

    inline KStarsData *data() { return m_Data; }

    /**@return the current setting of the color mode for stars (0=real colors,
        *1=solid red, 2=solid white or 3=solid black).
        */
    int starColorMode( void ) const;

    /**@short Retrieve the color-intensity value for stars.
        *
        *When using the "realistic colors" mode for stars, stars are rendered as 
        *white circles with a colored border.  The "color intensity" setting modulates
        *the relative thickness of this colored border, so it effectively adjusts
        *the color-saturation level for star images.
        *@return the current setting of the color intensity setting for stars.
        */
    int starColorIntensity( void ) const;

    /**
     *@return the magnitude of the faintest star
     */
    float faintMagnitude() const;

    /**
     *@short Return the size of the star as rendered by KStars
     *@return the Size of the star's representation on the skymap in pixels
     */
    float starRenderingSize( float mag ) const;

    float zoomMagnitudeLimit() const;

    /**
     *@short Read data for stars which will remain static in the memory
     *
     *This method reads data for named stars (stars having names,
     *which are stored by default in "namedstars.dat") into
     *memory. These stars are always kept in memory, as against 'deep'
     *stars which are mostly loaded dynamically (KStars treats all
     *unnamed stars as 'deep' stars) into memory when required,
     *depending on region and magnitude limit. Once loading is
     *successful, this method sets the starsLoaded flag to true
     */

    bool loadStaticData();

    SkyObject* objectNearest(SkyPoint *p, double &maxrad );

    SkyObject* findStarByGenetiveName( const QString name );

    /**
     *@short Find stars by name (including genetive name)
     *
     * Overrides ListComponent::findByName() to include genetive names of stars
     * as well.
     *
     *@param name  Name to search for. Could be trivial name or genetive name
     *@return  Pointer to the star with the given name as a SkyObject, NULL if
     *         no match was found
     */

    SkyObject* findByName( const QString &name );

    /**
     *@short Find stars by HD catalog index
     *@param HDnum HD Catalog Number of the star to find
     *@return If the star is a static star, a pointer to the star will be returned
     *        If it is a dynamic star, a fake copy will be created that survives till
     *        the next findByHDIndex() call. If no match was found, returns NULL.
     */

    SkyObject* findByHDIndex( int HDnum );

    // TODO: Make byteSwap a template method and put it in byteswap.h
    // It should ideally handle 32-bit, 16-bit fields and starData and
    // deepStarData fields
    static void byteSwap( starData *stardata );

private:
    SkyMesh*       m_skyMesh;
    StarIndex*     m_starIndex;

    KSNumbers      m_reindexNum;
    double         m_reindexInterval;

    int            m_lineNumber[ MAX_LINENUMBER_MAG + 1 ];
    LabelList*     m_labelList[  MAX_LINENUMBER_MAG + 1 ];
    int            m_lastLineNum;
    bool           m_validLineNums;
    bool           m_hideLabels;

    KStarsData*    m_Data;
    float          m_zoomMagLimit;

    float          m_FaintMagnitude; // Limiting magnitude of the catalog currently loaded
    bool           starsLoaded;
    float          magLim;           // Current limiting magnitude for visible stars
    unsigned long  visibleStarCount;
    quint16        MSpT;             // Maximum number of stars in any given trixel

    StarObject     m_starObject;
    StarObject     *focusStar;       // This object is always drawn

    KStarsSplash*  m_reloadSplash;
    KStarsSplash*  m_reindexSplash;

    StarBlockFactory *m_StarBlockFactory;

    QVector<HighPMStarList*> m_highPMStars;
    QHash<QString, SkyObject*> m_genName;
    QHash<int, StarObject*> m_HDHash;
    QVector<DeepStarComponent*> m_DeepStarComponents;

    /**
     *@short adds a label to the lists of labels to be drawn prioritized
     *by magnitude.
     */
    void addLabel( const QPointF& p, StarObject *star );

    void reindexAll( KSNumbers *num );

    /**
     *@short load available deep star catalogs
     */
    int loadDeepStarCatalogs();

    bool addDeepStarCatalogIfExists( const QString &fileName, float trigMag, bool staticstars=false );

    /**
     *@short Structure that holds star name information, to be read as-is from the corresponding binary data file
     */
    typedef struct starName {
        char bayerName[8];
        char longName[32];
    } starName;

    starData stardata;
    starName starname;

    static StarComponent *pinstance;

};

#endif
