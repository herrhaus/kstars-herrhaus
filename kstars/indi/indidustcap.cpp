/*
    SPDX-FileCopyrightText: 2015 Jasem Mutlaq <mutlaqja@ikarustech.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <basedevice.h>
#include <KLocalizedString>
#include <QtDBus/qdbusmetatype.h>

#include "indidustcap.h"
#include "clientmanager.h"

namespace ISD
{

const QList<const char *> DustCap::capStates = { I18N_NOOP("Idle"), I18N_NOOP("Parking"), I18N_NOOP("UnParking"),
                                                 I18N_NOOP("Parked"), I18N_NOOP("Error")
                                               };

DustCap::DustCap(GenericDevice *parent): ConcreteDevice(parent)
{
    qRegisterMetaType<ISD::DustCap::Status>("ISD::DustCap::Status");
    qDBusRegisterMetaType<ISD::DustCap::Status>();
}

void DustCap::processSwitch(ISwitchVectorProperty *svp)
{
    if (!strcmp(svp->name, "CAP_PARK"))
    {
        Status currentStatus = CAP_ERROR;

        switch (svp->s)
        {
            case IPS_IDLE:
                if (svp->sp[0].s == ISS_ON)
                    currentStatus = CAP_PARKED;
                else if (svp->sp[1].s == ISS_ON)
                    currentStatus = CAP_IDLE;
                break;

            case IPS_OK:
                if (svp->sp[0].s == ISS_ON)
                    currentStatus = CAP_PARKED;
                else
                    currentStatus = CAP_IDLE;
                break;

            case IPS_BUSY:
                if (svp->sp[0].s == ISS_ON)
                    currentStatus = CAP_PARKING;
                else
                    currentStatus = CAP_UNPARKING;
                break;

            case IPS_ALERT:
                currentStatus = CAP_ERROR;
        }

        if (currentStatus != m_Status)
        {
            m_Status = currentStatus;
            emit newStatus(m_Status);
        }
    }
}

bool DustCap::canPark()
{
    auto parkSP = getSwitch("CAP_PARK");
    if (!parkSP)
        return false;
    else
        return true;
}

bool DustCap::isParked()
{
    auto parkSP = getSwitch("CAP_PARK");
    if (!parkSP)
        return false;

    return ((parkSP->getState() == IPS_OK || parkSP->getState() == IPS_IDLE) && parkSP->at(0)->getState() == ISS_ON);
}

bool DustCap::isUnParked()
{
    auto parkSP = getSwitch("CAP_PARK");
    if (!parkSP)
        return false;

    return ( (parkSP->getState() == IPS_OK || parkSP->getState() == IPS_IDLE) && parkSP->at(1)->getState() == ISS_ON);
}

bool DustCap::Park()
{
    auto parkSP = getSwitch("CAP_PARK");
    if (!parkSP)
        return false;

    auto parkSW = parkSP->findWidgetByName("PARK");
    if (!parkSW)
        return false;

    parkSP->reset();
    parkSW->setState(ISS_ON);
    sendNewSwitch(parkSP);

    return true;
}

bool DustCap::UnPark()
{
    auto parkSP = getSwitch("CAP_PARK");
    if (!parkSP)
        return false;

    auto parkSW = parkSP->findWidgetByName("UNPARK");
    if (!parkSW)
        return false;

    parkSP->reset();
    parkSW->setState(ISS_ON);
    sendNewSwitch(parkSP);

    return true;
}

bool DustCap::hasLight()
{
    auto lightSP = getSwitch("FLAT_LIGHT_CONTROL");
    if (!lightSP)
        return false;
    else
        return true;
}

bool DustCap::isLightOn()
{
    auto lightSP = getSwitch("FLAT_LIGHT_CONTROL");
    if (!lightSP)
        return false;

    auto lightON = lightSP->findWidgetByName("FLAT_LIGHT_ON");
    if (!lightON)
        return false;

    return lightON->getState() == ISS_ON;
}

bool DustCap::SetLightEnabled(bool enable)
{
    auto lightSP = getSwitch("FLAT_LIGHT_CONTROL");

    if (!lightSP)
        return false;

    auto lightON  = lightSP->findWidgetByName("FLAT_LIGHT_ON");
    auto lightOFF = lightSP->findWidgetByName("FLAT_LIGHT_OFF");

    if (!lightON || !lightOFF)
        return false;

    lightSP->reset();

    if (enable)
        lightON->setState(ISS_ON);
    else
        lightOFF->setState(ISS_ON);

    sendNewSwitch(lightSP);

    return true;
}

bool DustCap::SetBrightness(uint16_t val)
{
    auto lightIntensity = getNumber("FLAT_LIGHT_INTENSITY");
    if (!lightIntensity)
        return false;

    lightIntensity->at(0)->setValue(val);
    sendNewNumber(lightIntensity);
    return true;
}

const QString DustCap::getStatusString(DustCap::Status status, bool translated)
{
    return translated ? i18n(capStates[status]) : capStates[status];
}

}

QDBusArgument &operator<<(QDBusArgument &argument, const ISD::DustCap::Status &source)
{
    argument.beginStructure();
    argument << static_cast<int>(source);
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, ISD::DustCap::Status &dest)
{
    int a;
    argument.beginStructure();
    argument >> a;
    argument.endStructure();
    dest = static_cast<ISD::DustCap::Status>(a);
    return argument;
}
