/***************************************************************************
                          finddialog.cpp  -  K Desktop Planetarium
                             -------------------
    begin                : Wed Jul 4 2001
    copyright            : (C) 2001 by Jason Harris
    email                : jharris@30doradus.org
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "finddialog.h"

#include <QSortFilterProxyModel>
#include <QStringListModel>
#include <QTimer>

#include <kmessagebox.h>

#include "kstars.h"
#include "kstarsdata.h"
#include "Options.h"
#include "skyobject.h"

FindDialogUI::FindDialogUI( QWidget *parent ) : QFrame( parent ) {
	setupUi( this );

	FilterType->addItem( i18n ("Any") );
	FilterType->addItem( i18n ("Stars") );
	FilterType->addItem( i18n ("Solar System") );
	FilterType->addItem( i18n ("Open Clusters") );
	FilterType->addItem( i18n ("Globular Clusters") );
	FilterType->addItem( i18n ("Gaseous Nebulae") );
	FilterType->addItem( i18n ("Planetary Nebulae") );
	FilterType->addItem( i18n ("Galaxies") );
	FilterType->addItem( i18n ("Comets") );
	FilterType->addItem( i18n ("Asteroids") );
	FilterType->addItem( i18n ("Constellations") );

	SearchList->setMinimumWidth( 256 );
	SearchList->setMinimumHeight( 320 );
}

FindDialog::FindDialog( QWidget* parent )
    : KDialog( parent ), timer(0)
{
	ui = new FindDialogUI( this );
	setMainWidget( ui );
	setCaption( i18n( "Find Object" ) );
	setButtons( KDialog::Ok|KDialog::Cancel );
	ui->FilterType->setCurrentIndex(0);  // show all types of objects

	fModel = new QStringListModel( this );
	sortModel = new QSortFilterProxyModel( ui->SearchList );
	ui->SearchList->setModel( sortModel );
	sortModel->setSourceModel( fModel );
	ui->SearchList->setModel( sortModel );

//Connect signals to slots
	connect( this, SIGNAL( okClicked() ), this, SLOT( slotOk() ) );
	connect( this, SIGNAL( cancelClicked() ), this, SLOT( reject() ) );
	connect( ui->SearchBox, SIGNAL( textChanged( const QString & ) ), SLOT( enqueueSearch() ) );
	connect( ui->SearchBox, SIGNAL( returnPressed() ), SLOT( slotOk() ) );
	connect( ui->FilterType, SIGNAL( activated( int ) ), this, SLOT( filterByType( int ) ) );
	connect( ui->SearchList, SIGNAL( doubleClicked( const QModelIndex & ) ), SLOT( slotOk() ) );


	// first create and paint dialog and then load list
	QTimer::singleShot(0, this, SLOT( init() ));
}

FindDialog::~FindDialog() {
}

void FindDialog::init() {
	ui->SearchBox->clear();

	KStars *p = (KStars *)parent();

	switch ( ui->FilterType->currentIndex() ) {
		case 0: // All object types
		{
			QStringList allObjects;
			foreach ( int type, p->data()->skyComposite()->objectNames().keys() )
				allObjects += p->data()->skyComposite()->objectNames( type );

			fModel->setStringList( allObjects );
			break;
		}
		case 1: //Stars
		{
			QStringList starObjects;
			starObjects += p->data()->skyComposite()->objectNames( SkyObject::STAR );
			starObjects += p->data()->skyComposite()->objectNames( SkyObject::CATALOG_STAR );
			fModel->setStringList( starObjects );
			break;
		}
		case 2: //Solar system
		{
			QStringList ssObjects;
			ssObjects += p->data()->skyComposite()->objectNames( SkyObject::PLANET );
			ssObjects += p->data()->skyComposite()->objectNames( SkyObject::COMET );
			ssObjects += p->data()->skyComposite()->objectNames( SkyObject::ASTEROID );
			ssObjects += i18n("Sun");
			fModel->setStringList( ssObjects );
			break;
		}
		case 3: //Open Clusters
			fModel->setStringList( p->data()->skyComposite()->objectNames( SkyObject::OPEN_CLUSTER ) );
			break;
		case 4: //Open Clusters
			fModel->setStringList( p->data()->skyComposite()->objectNames( SkyObject::GLOBULAR_CLUSTER ) );
			break;
		case 5: //Gaseous nebulae
			fModel->setStringList( p->data()->skyComposite()->objectNames( SkyObject::GASEOUS_NEBULA ) );
			break;
		case 6: //Planetary nebula
			fModel->setStringList( p->data()->skyComposite()->objectNames( SkyObject::PLANETARY_NEBULA ) );
			break;
		case 7: //Galaxies
			fModel->setStringList( p->data()->skyComposite()->objectNames( SkyObject::GALAXY ) );
			break;
		case 8: //Comets
			fModel->setStringList( p->data()->skyComposite()->objectNames( SkyObject::COMET ) );
			break;
		case 9: //Asteroids
			fModel->setStringList( p->data()->skyComposite()->objectNames( SkyObject::ASTEROID ) );
			break;
		case 10: //Constellations
			fModel->setStringList( p->data()->skyComposite()->objectNames( SkyObject::CONSTELLATION ) );
			break;
	}

	sortModel->sort( 0 );

	initSelection();
}

void FindDialog::initSelection() {
	if ( sortModel->rowCount() <= 0 ) {
		button( Ok )->setEnabled( false );
		return;
	}

	//Pre-select the first item
	QModelIndex selectItem = sortModel->index( 0, sortModel->filterKeyColumn(), QModelIndex() );
	switch ( ui->FilterType->currentIndex() ) {
		case 0: //All objects, choose Andromeda galaxy
		{
			QModelIndex qmi = fModel->index( fModel->stringList().indexOf( i18n("Andromeda Galaxy") ) );
			selectItem = sortModel->mapFromSource( qmi );
			break;
		}
		case 1: //Stars, choose Aldebaran
		{
			QModelIndex qmi = fModel->index( fModel->stringList().indexOf( i18n("Aldebaran") ) );
			selectItem = sortModel->mapFromSource( qmi );
			break;
		}
		case 2: //Solar system or Asteroids, choose Aaltje
		case 9:
		{
			QModelIndex qmi = fModel->index( fModel->stringList().indexOf( i18n("Aaltje") ) );
			selectItem = sortModel->mapFromSource( qmi );
			break;
		}
		case 8: //Comets, choose 'Aarseth-Brewington (1989 W1)'
		{
			QModelIndex qmi = fModel->index( fModel->stringList().indexOf( i18n("Aarseth-Brewington (1989 W1)") ) );
			selectItem = sortModel->mapFromSource( qmi );
			break;
		}
	
	}

	if ( selectItem.isValid() ) {
		ui->SearchList->selectionModel()->select( selectItem, QItemSelectionModel::ClearAndSelect );
		ui->SearchList->scrollTo( selectItem );
		ui->SearchList->setCurrentIndex( selectItem );
		button( Ok )->setEnabled( true );
	}
}

void FindDialog::filterByType( int f ) {
	if ( timer ) {
		timer->stop();
	}

	init();
}

void FindDialog::filterByName() {  //Filter the list of names with the string in the SearchBox
	sortModel->setFilterFixedString( ui->SearchBox->text() );
	initSelection();
}

SkyObject* FindDialog::selectedObject() const {
	QModelIndex i = ui->SearchList->currentIndex();
	SkyObject *obj = 0;
	if ( i.isValid() ) {
		KStars *p = (KStars*)parent();
		QString ObjName = i.data().toString();
		obj = p->data()->skyComposite()->findByName( ObjName );
	}
	
	return obj;
}

void FindDialog::enqueueSearch() {
	if ( timer ) {
		timer->stop();
	} else {
		timer = new QTimer( this );
		timer->setSingleShot( true );
		connect( timer, SIGNAL( timeout() ), this, SLOT( filterByName() ) );
	}
	timer->start( 500 );
}

void FindDialog::slotOk() {
	//If no valid object selected, show a sorry-box.  Otherwise, emit accept()
	if ( selectedObject() == 0 ) {
		QString message = i18n( "No object named %1 found.", ui->SearchBox->text() );
		KMessageBox::sorry( 0, message, i18n( "Bad object name" ) );
	} else {
		accept();
	}
}

void FindDialog::keyPressEvent( QKeyEvent *e ) {
	switch( e->key() ) {
		case Qt::Key_Escape :
			reject();
			break;

	}
}

#include "finddialog.moc"
