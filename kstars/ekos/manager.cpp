/*
    SPDX-FileCopyrightText: 2012 Jasem Mutlaq <mutlaqja@ikartech.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "manager.h"

#include "ekosadaptor.h"
#include "kstars.h"
#include "kstarsdata.h"
#include "opsekos.h"
#include "Options.h"
#include "profileeditor.h"
#include "profilewizard.h"
#include "indihub.h"
#include "skymap.h"
#include "auxiliary/darklibrary.h"
#include "auxiliary/QProgressIndicator.h"
#include "auxiliary/ksmessagebox.h"
#include "capture/sequencejob.h"
#include "fitsviewer/fitstab.h"
#include "fitsviewer/fitsview.h"
#include "fitsviewer/fitsdata.h"
#include "indi/clientmanager.h"
#include "indi/driverinfo.h"
#include "indi/drivermanager.h"
#include "indi/guimanager.h"
#include "indi/indielement.h"
#include "indi/indilistener.h"
#include "indi/indiproperty.h"
#include "indi/indiwebmanager.h"
#include "indi/indigps.h"
#include "indi/indiguider.h"

#include "ekoslive/ekosliveclient.h"
#include "ekoslive/message.h"
#include "ekoslive/media.h"

#include <basedevice.h>

#include <KConfigDialog>
#include <KMessageBox>
#include <KActionCollection>
#include <KNotifications/KNotification>

#include <QFutureWatcher>
#include <QComboBox>

#include <ekos_debug.h>

#define MAX_REMOTE_INDI_TIMEOUT 15000
#define MAX_LOCAL_INDI_TIMEOUT  10000

namespace Ekos
{

Manager *Manager::_Manager = nullptr;

Manager *Manager::Instance()
{
    if (_Manager == nullptr)
        _Manager = new Manager(Options::independentWindowEkos() ? nullptr : KStars::Instance());

    return _Manager;
}

void Manager::release()
{
    delete _Manager;
}

Manager::Manager(QWidget * parent) : QDialog(parent)
{
#ifdef Q_OS_OSX

    if (Options::independentWindowEkos())
        setWindowFlags(Qt::Window);
    else
    {
        setWindowFlags(Qt::Window | Qt::WindowStaysOnTopHint);
        connect(QApplication::instance(), SIGNAL(applicationStateChanged(Qt::ApplicationState)), this,
                SLOT(changeAlwaysOnTop(Qt::ApplicationState)));
    }
#else
    if (Options::independentWindowEkos())
        //setWindowFlags(Qt::Window | Qt::WindowStaysOnTopHint);
        setWindowFlags(Qt::Window);
#endif
    setupUi(this);

    // position the vertical splitter by 2/3
    deviceSplitter->setSizes(QList<int>({20000, 10000}));

    qRegisterMetaType<Ekos::CommunicationStatus>("Ekos::CommunicationStatus");
    qDBusRegisterMetaType<Ekos::CommunicationStatus>();

    new EkosAdaptor(this);
    QDBusConnection::sessionBus().registerObject("/KStars/Ekos", this);

    setWindowIcon(QIcon::fromTheme("kstars_ekos"));

    profileModel.reset(new QStandardItemModel(0, 4));
    profileModel->setHorizontalHeaderLabels(QStringList() << "id"
                                            << "name"
                                            << "host"
                                            << "port");

    m_CountdownTimer.setInterval(1000);
    connect(&m_CountdownTimer, &QTimer::timeout, this, &Ekos::Manager::updateCaptureCountDown);

    toolsWidget->setIconSize(QSize(48, 48));
    connect(toolsWidget, &QTabWidget::currentChanged, this, &Ekos::Manager::processTabChange, Qt::UniqueConnection);

    // Enable scheduler Tab
    toolsWidget->setTabEnabled(1, false);

    // Enable analyze Tab
    toolsWidget->setTabEnabled(2, false);

    // Start/Stop INDI Server
    connect(processINDIB, &QPushButton::clicked, this, &Ekos::Manager::processINDI);
    processINDIB->setIcon(QIcon::fromTheme("media-playback-start"));
    processINDIB->setToolTip(i18n("Start"));

    // Connect/Disconnect INDI devices
    connect(connectB, &QPushButton::clicked, this, &Ekos::Manager::connectDevices);
    connect(disconnectB, &QPushButton::clicked, this, &Ekos::Manager::disconnectDevices);

    ekosLiveB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    ekosLiveClient.reset(new EkosLive::Client(this));
    connect(ekosLiveClient.get(), &EkosLive::Client::connected, this, [this]()
    {
        emit ekosLiveStatusChanged(true);
    });
    connect(ekosLiveClient.get(), &EkosLive::Client::disconnected, this, [this]()
    {
        emit ekosLiveStatusChanged(false);
    });

    // INDI Control Panel
    //connect(controlPanelB, &QPushButton::clicked, GUIManager::Instance(), SLOT(show()));
    connect(ekosLiveB, &QPushButton::clicked, this, [&]()
    {
        ekosLiveClient.get()->show();
        ekosLiveClient.get()->raise();
    });

    connect(this, &Manager::ekosStatusChanged, ekosLiveClient.get()->message(), &EkosLive::Message::setEkosStatingStatus);
    connect(ekosLiveClient.get()->message(), &EkosLive::Message::connected, this, [&]()
    {
        ekosLiveB->setIcon(QIcon(":/icons/cloud-online.svg"));
    });
    connect(ekosLiveClient.get()->message(), &EkosLive::Message::disconnected, this, [&]()
    {
        ekosLiveB->setIcon(QIcon::fromTheme("folder-cloud"));
    });
    connect(ekosLiveClient.get()->media(), &EkosLive::Media::newBoundingRect, ekosLiveClient.get()->message(),
            &EkosLive::Message::setBoundingRect);
    connect(ekosLiveClient.get()->message(), &EkosLive::Message::resetPolarView, ekosLiveClient.get()->media(),
            &EkosLive::Media::resetPolarView);
    connect(KSMessageBox::Instance(), &KSMessageBox::newMessage, ekosLiveClient.get()->message(),
            &EkosLive::Message::sendDialog);

    // Port Selector
    m_PortSelectorTimer.setInterval(500);
    m_PortSelectorTimer.setSingleShot(true);
    connect(&m_PortSelectorTimer, &QTimer::timeout, this, [this]()
    {
        if (m_PortSelector && currentProfile->portSelector)
        {
            if (m_PortSelector->shouldShow())
            {
                m_PortSelector->show();
                m_PortSelector->raise();

                ekosLiveClient.get()->message()->requestPortSelection(true);
            }
            // If port selector is enabled, but we have zero ports to work with, let's proceed to connecting if it is enabled.
            else if (currentProfile->autoConnect)
                setPortSelectionComplete();
        }
    });
    connect(portSelectorB, &QPushButton::clicked, this, [&]()
    {
        if (m_PortSelector)
        {
            m_PortSelector->show();
            m_PortSelector->raise();
        }
    });

    connect(this, &Ekos::Manager::ekosStatusChanged, this, [&](Ekos::CommunicationStatus status)
    {
        indiControlPanelB->setEnabled(status == Ekos::Success);
        connectB->setEnabled(false);
        disconnectB->setEnabled(false);
        profileGroup->setEnabled(status == Ekos::Idle || status == Ekos::Error);
        m_isStarted = (status == Ekos::Success || status == Ekos::Pending);
        if (status == Ekos::Success)
        {
            processINDIB->setIcon(QIcon::fromTheme("media-playback-stop"));
            processINDIB->setToolTip(i18n("Stop"));
            setWindowTitle(i18nc("@title:window", "Ekos - %1 Profile", currentProfile->name));
        }
        else if (status == Ekos::Error || status == Ekos::Idle)
        {
            processINDIB->setIcon(QIcon::fromTheme("media-playback-start"));
            processINDIB->setToolTip(i18n("Start"));
        }
        else
        {
            processINDIB->setIcon(QIcon::fromTheme("call-stop"));
            processINDIB->setToolTip(i18n("Connection in progress. Click to abort."));
        }
    });
    connect(indiControlPanelB, &QPushButton::clicked, this, [&]()
    {
        KStars::Instance()->actionCollection()->action("show_control_panel")->trigger();
    });
    connect(optionsB, &QPushButton::clicked, this, [&]()
    {
        KStars::Instance()->actionCollection()->action("configure")->trigger();
    });
    // Save as above, but it appears in all modules
    connect(ekosOptionsB, &QPushButton::clicked, this, &Ekos::Manager::showEkosOptions);

    // Clear Ekos Log
    connect(clearB, &QPushButton::clicked, this, &Ekos::Manager::clearLog);

    // Logs
    KConfigDialog * dialog = new KConfigDialog(this, "logssettings", Options::self());
    opsLogs = new Ekos::OpsLogs();
    KPageWidgetItem * page = dialog->addPage(opsLogs, i18n("Logging"));
    page->setIcon(QIcon::fromTheme("configure"));
    connect(logsB, &QPushButton::clicked, dialog, &KConfigDialog::show);
    connect(dialog->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &Ekos::Manager::updateDebugInterfaces);
    connect(dialog->button(QDialogButtonBox::Ok), &QPushButton::clicked, this, &Ekos::Manager::updateDebugInterfaces);

    // Profiles
    connect(addProfileB, &QPushButton::clicked, this, &Ekos::Manager::addProfile);
    connect(editProfileB, &QPushButton::clicked, this, &Ekos::Manager::editProfile);
    connect(deleteProfileB, &QPushButton::clicked, this, &Ekos::Manager::deleteProfile);
    connect(profileCombo, static_cast<void(QComboBox::*)(const QString &)>(&QComboBox::currentTextChanged), this,
            [ = ](const QString & text)
    {
        Options::setProfile(text);
        if (text == "Simulators")
        {
            editProfileB->setEnabled(false);
            deleteProfileB->setEnabled(false);
        }
        else
        {
            editProfileB->setEnabled(true);
            deleteProfileB->setEnabled(true);
        }
    });

    // Settle timer
    // Debounce until property stream settles down for a second.
    settleTimer.setInterval(1000);
    connect(&settleTimer, &QTimer::timeout, this, [&]()
    {
        if (m_settleStatus != Ekos::Success)
        {
            m_settleStatus = Ekos::Success;
            emit settleStatusChanged(m_settleStatus);
        }
    });

    // Ekos Wizard
    connect(wizardProfileB, &QPushButton::clicked, this, &Ekos::Manager::wizardProfile);

    addProfileB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    editProfileB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    deleteProfileB->setAttribute(Qt::WA_LayoutUsesWidgetRect);

    // Set Profile icons
    addProfileB->setIcon(QIcon::fromTheme("list-add"));
    addProfileB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    editProfileB->setIcon(QIcon::fromTheme("document-edit"));
    editProfileB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    deleteProfileB->setIcon(QIcon::fromTheme("list-remove"));
    deleteProfileB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    wizardProfileB->setIcon(QIcon::fromTheme("tools-wizard"));
    wizardProfileB->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    customDriversB->setIcon(QIcon::fromTheme("roll"));
    customDriversB->setAttribute(Qt::WA_LayoutUsesWidgetRect);

    connect(customDriversB, &QPushButton::clicked, DriverManager::Instance(), &DriverManager::showCustomDrivers);

    // Load all drivers
    loadDrivers();

    // Load add driver profiles
    loadProfiles();

    // INDI Control Panel and Ekos Options
    optionsB->setIcon(QIcon::fromTheme("configure", QIcon(":/icons/ekos_setup.png")));
    optionsB->setAttribute(Qt::WA_LayoutUsesWidgetRect);

    // Setup Tab
    toolsWidget->tabBar()->setTabIcon(0, QIcon(":/icons/ekos_setup.png"));
    toolsWidget->tabBar()->setTabToolTip(0, i18n("Setup"));

    // Initialize Ekos Scheduler Module
    schedulerProcess.reset(new Scheduler());
    int index = addModuleTab(EkosModule::Scheduler, schedulerProcess.get(), QIcon(":/icons/ekos_scheduler.png"));
    toolsWidget->tabBar()->setTabToolTip(index, i18n("Scheduler"));
    capturePreview->shareSchedulerProcess(schedulerProcess.get());
    connect(schedulerProcess.get(), &Scheduler::newLog, this, &Ekos::Manager::updateLog);
    connect(schedulerProcess.get(), &Ekos::Scheduler::newTarget, [&](const QString & target)
    {
        mountTarget->setText(target);
        ekosLiveClient.get()->message()->updateMountStatus(QJsonObject({{"target", target}}));
    });

    // Initialize Ekos Analyze Module
    analyzeProcess.reset(new Ekos::Analyze());
    index = addModuleTab(EkosModule::Analyze, analyzeProcess.get(), QIcon(":/icons/ekos_analyze.png"));
    toolsWidget->tabBar()->setTabToolTip(index, i18n("Analyze"));

    numPermanentTabs = index + 1;

    // Temporary fix. Not sure how to resize Ekos Dialog to fit contents of the various tabs in the QScrollArea which are added
    // dynamically. I used setMinimumSize() but it doesn't appear to make any difference.
    // Also set Layout policy to SetMinAndMaxSize as well. Any idea how to fix this?
    // FIXME
    //resize(1000,750);

    m_SummaryView.reset(new SummaryFITSView(capturePreview->previewWidget));
    m_SummaryView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // sterne-jaeger 2021-08-08: Do not set base size here, otherwise the zoom will be incorrect
    // summaryPreview->setBaseSize(capturePreview->previewWidget->size());
    m_SummaryView->createFloatingToolBar();
    m_SummaryView->setCursorMode(FITSView::dragCursor);
    m_SummaryView->showProcessInfo(false);
    capturePreview->setSummaryFITSView(m_SummaryView.get());

    // JM 2019-01-19: Why cloud images depend on summary preview?
    //    connect(summaryPreview.get(), &FITSView::loaded, [&]()
    //    {
    //        // UUID binds the cloud & preview frames by a common key
    //        QString uuid = QUuid::createUuid().toString();
    //        uuid = uuid.remove(QRegularExpression("[-{}]"));
    //        //ekosLiveClient.get()->media()->sendPreviewImage(summaryPreview.get(), uuid);
    //        ekosLiveClient.get()->cloud()->sendPreviewImage(summaryPreview.get(), uuid);
    //    });

    if (Options::ekosLeftIcons())
    {
        toolsWidget->setTabPosition(QTabWidget::West);
        QTransform trans;
        trans.rotate(90);

        for (int i = 0; i < numPermanentTabs; ++i)
        {
            QIcon icon  = toolsWidget->tabIcon(i);
            QPixmap pix = icon.pixmap(QSize(48, 48));
            icon        = QIcon(pix.transformed(trans));
            toolsWidget->setTabIcon(i, icon);
        }
    }

    //Note:  This is to prevent a button from being called the default button
    //and then executing when the user hits the enter key such as when on a Text Box

    QList<QPushButton *> qButtons = findChildren<QPushButton *>();
    for (auto &button : qButtons)
        button->setAutoDefault(false);


    resize(Options::ekosWindowWidth(), Options::ekosWindowHeight());
}

void Manager::changeAlwaysOnTop(Qt::ApplicationState state)
{
    if (isVisible())
    {
        if (state == Qt::ApplicationActive)
            setWindowFlags(Qt::Window | Qt::WindowStaysOnTopHint);
        else
            setWindowFlags(windowFlags() & ~Qt::WindowStaysOnTopHint);
        show();
    }
}

Manager::~Manager()
{
    toolsWidget->disconnect(this);
}

void Manager::closeEvent(QCloseEvent * event)
{
    //    QAction * a = KStars::Instance()->actionCollection()->action("show_ekos");
    //    a->setChecked(false);

    // 2019-02-14 JM: Close event, for some reason, make all the children disappear
    // when the widget is shown again. Applying a workaround here

    event->ignore();
    hide();
}

void Manager::hideEvent(QHideEvent * /*event*/)
{
    Options::setEkosWindowWidth(width());
    Options::setEkosWindowHeight(height());

    QAction * a = KStars::Instance()->actionCollection()->action("show_ekos");
    a->setChecked(false);
}

void Manager::showEvent(QShowEvent * /*event*/)
{
    QAction * a = KStars::Instance()->actionCollection()->action("show_ekos");
    a->setChecked(true);

    // Just show the profile wizard ONCE per session
    if (profileWizardLaunched == false && profiles.count() == 1)
    {
        profileWizardLaunched = true;
        wizardProfile();
    }
}

void Manager::resizeEvent(QResizeEvent *)
{
    focusManager->updateFocusDetailView();
    guideManager->updateGuideDetailView();
}

void Manager::loadProfiles()
{
    profiles.clear();
    KStarsData::Instance()->userdb()->GetAllProfiles(profiles);

    profileModel->clear();

    for (auto &pi : profiles)
    {
        QList<QStandardItem *> info;

        info << new QStandardItem(pi->id) << new QStandardItem(pi->name) << new QStandardItem(pi->host)
             << new QStandardItem(pi->port);
        profileModel->appendRow(info);
    }

    profileModel->sort(0);
    profileCombo->blockSignals(true);
    profileCombo->setModel(profileModel.get());
    profileCombo->setModelColumn(1);
    profileCombo->blockSignals(false);

    // Load last used profile from options
    int index = profileCombo->findText(Options::profile());
    // If not found, set it to first item
    if (index == -1)
        index = 0;
    profileCombo->setCurrentIndex(index);
}

int Manager::addModuleTab(Manager::EkosModule module, QWidget *tab, const QIcon &icon)
{
    int index = 0;
    switch(module)
    {
        case EkosModule::Observatory:
            index += guideProcess ? 1 : 0; /* FALLTHRU */
        case EkosModule::Guide:
            index += alignProcess ? 1 : 0; /* FALLTHRU */
        case EkosModule::Align:
            index += mountProcess ? 1 : 0; /* FALLTHRU */
        case EkosModule::Mount:
            index += focusProcess ? 1 : 0; /* FALLTHRU */
        case EkosModule::Focus:
            index += captureProcess ? 1 : 0; /* FALLTHRU */
        case EkosModule::Capture:
            index += analyzeProcess ? 1 : 0; /* FALLTHRU */
        case EkosModule::Analyze:
            index += schedulerProcess ? 1 : 0; /* FALLTHRU */
        case EkosModule::Scheduler:
            index += 1; /* FALLTHRU */
        case EkosModule::Setup:
            // do nothing
            break;
        default:
            index = toolsWidget->count();
            break;
    }

    toolsWidget->insertTab(index, tab, icon, "");
    return index;
}

void Manager::loadDrivers()
{
    for (auto &dv : DriverManager::Instance()->getDrivers())
    {
        if (dv->getDriverSource() != HOST_SOURCE)
            driversList[dv->getLabel()] = dv;
    }
}

void Manager::reset()
{
    qCDebug(KSTARS_EKOS) << "Resetting Ekos Manager...";

    // Filter Manager
    filterManager.reset(new Ekos::FilterManager());

    nDevices = 0;

    removeTabs();

    m_GenericDevices.clear();

    captureProcess.reset();
    focusProcess.reset();
    guideProcess.reset();
    alignProcess.reset();
    mountProcess.reset();
    //observatoryProcess.reset();

    DarkLibrary::Release();
    m_PortSelector.reset();
    m_PortSelectorTimer.stop();

    Ekos::CommunicationStatus previousStatus;

    previousStatus = m_settleStatus;
    m_settleStatus = Ekos::Idle;
    if (previousStatus != m_settleStatus)
        emit settleStatusChanged(m_settleStatus);

    previousStatus = m_ekosStatus;
    m_ekosStatus   = Ekos::Idle;
    if (previousStatus != m_ekosStatus)
        emit ekosStatusChanged(m_ekosStatus);

    previousStatus = m_indiStatus;
    m_indiStatus = Ekos::Idle;
    if (previousStatus != m_indiStatus)
        emit indiStatusChanged(m_indiStatus);

    connectB->setEnabled(false);
    disconnectB->setEnabled(false);
    //controlPanelB->setEnabled(false);
    processINDIB->setEnabled(true);

    mountGroup->setEnabled(false);
    capturePreview->setEnabled(false);
    capturePreview->reset();
    mountStatus->setText(i18n("Idle"));
    mountStatus->setStyleSheet(QString());
    focusManager->reset();
    guideManager->reset();

    m_isStarted = false;

    processINDIB->setIcon(QIcon::fromTheme("media-playback-start"));
    processINDIB->setToolTip(i18n("Start"));
}

void Manager::processINDI()
{
    if (m_isStarted == false)
        start();
    else
        stop();
}

void Manager::stop()
{
    cleanDevices();
    m_PortSelector.reset();
    m_PortSelectorTimer.stop();
    m_CountdownTimer.stop();
    portSelectorB->setEnabled(false);

    if (indiHubAgent)
        indiHubAgent->terminate();

    profileGroup->setEnabled(true);

    setWindowTitle(i18nc("@title:window", "Ekos"));
}

void Manager::start()
{
    // Don't start if it is already started before
    if (m_ekosStatus == Ekos::Pending || m_ekosStatus == Ekos::Success)
    {
        qCWarning(KSTARS_EKOS) << "Ekos Manager start called but current Ekos Status is" << m_ekosStatus << "Ignoring request.";
        return;
    }

    if (m_LocalMode)
        qDeleteAll(managedDrivers);
    managedDrivers.clear();

    // If clock was paused, unpaused it and sync time
    if (KStarsData::Instance()->clock()->isActive() == false)
    {
        KStarsData::Instance()->changeDateTime(KStarsDateTime::currentDateTimeUtc());
        KStarsData::Instance()->clock()->start();
    }

    // Reset Ekos Manager
    reset();

    // Get Current Profile
    currentProfile = getCurrentProfile();
    m_LocalMode      = currentProfile->isLocal();

    // Load profile location if one exists
    updateProfileLocation(currentProfile);

    bool haveCCD = false, haveGuider = false;

    // If external guide is specified in the profile, set the
    // corresponding options
    if (currentProfile->guidertype == Ekos::Guide::GUIDE_PHD2)
    {
        Options::setPHD2Host(currentProfile->guiderhost);
        Options::setPHD2Port(currentProfile->guiderport);
    }
    else if (currentProfile->guidertype == Ekos::Guide::GUIDE_LINGUIDER)
    {
        Options::setLinGuiderHost(currentProfile->guiderhost);
        Options::setLinGuiderPort(currentProfile->guiderport);
    }

    // Parse script, if any
    QJsonParseError jsonError;
    QJsonArray profileScripts;
    QJsonDocument doc = QJsonDocument::fromJson(currentProfile->scripts, &jsonError);

    if (jsonError.error == QJsonParseError::NoError)
        profileScripts = doc.array();

    // For locally running INDI server
    if (m_LocalMode)
    {
        DriverInfo * drv = driversList.value(currentProfile->mount());

        if (drv != nullptr)
            managedDrivers.append(drv->clone());

        drv = driversList.value(currentProfile->ccd());
        if (drv != nullptr)
        {
            managedDrivers.append(drv->clone());
            haveCCD = true;
        }

        Options::setGuiderType(currentProfile->guidertype);

        drv = driversList.value(currentProfile->guider());
        if (drv != nullptr)
        {
            haveGuider = true;

            // If the guider and ccd are the same driver, we have two cases:
            // #1 Drivers that only support ONE device per driver (such as sbig)
            // #2 Drivers that supports multiples devices per driver (such as sx)
            // For #1, we modify guider_di to make a unique label for the other device with postfix "Guide"
            // For #2, we set guider_di to nullptr and we prompt the user to select which device is primary ccd and which is guider
            // since this is the only way to find out in real time.
            if (haveCCD && currentProfile->guider() == currentProfile->ccd())
            {
                if (checkUniqueBinaryDriver( driversList.value(currentProfile->ccd()), drv))
                {
                    drv = nullptr;
                }
                else
                {
                    drv->setUniqueLabel(drv->getLabel() + " Guide");
                }
            }

            if (drv)
                managedDrivers.append(drv->clone());
        }

        drv = driversList.value(currentProfile->ao());
        if (drv != nullptr)
            managedDrivers.append(drv->clone());

        drv = driversList.value(currentProfile->filter());
        if (drv != nullptr)
            managedDrivers.append(drv->clone());

        drv = driversList.value(currentProfile->focuser());
        if (drv != nullptr)
            managedDrivers.append(drv->clone());

        drv = driversList.value(currentProfile->dome());
        if (drv != nullptr)
            managedDrivers.append(drv->clone());

        drv = driversList.value(currentProfile->weather());
        if (drv != nullptr)
            managedDrivers.append(drv->clone());

        drv = driversList.value(currentProfile->aux1());
        if (drv != nullptr)
        {
            if (!checkUniqueBinaryDriver(driversList.value(currentProfile->ccd()), drv) &&
                    !checkUniqueBinaryDriver(driversList.value(currentProfile->guider()), drv))
                managedDrivers.append(drv->clone());
        }
        drv = driversList.value(currentProfile->aux2());
        if (drv != nullptr)
        {
            if (!checkUniqueBinaryDriver(driversList.value(currentProfile->ccd()), drv) &&
                    !checkUniqueBinaryDriver(driversList.value(currentProfile->guider()), drv))
                managedDrivers.append(drv->clone());
        }

        drv = driversList.value(currentProfile->aux3());
        if (drv != nullptr)
        {
            if (!checkUniqueBinaryDriver(driversList.value(currentProfile->ccd()), drv) &&
                    !checkUniqueBinaryDriver(driversList.value(currentProfile->guider()), drv))
                managedDrivers.append(drv->clone());
        }

        drv = driversList.value(currentProfile->aux4());
        if (drv != nullptr)
        {
            if (!checkUniqueBinaryDriver(driversList.value(currentProfile->ccd()), drv) &&
                    !checkUniqueBinaryDriver(driversList.value(currentProfile->guider()), drv))
                managedDrivers.append(drv->clone());
        }

        // Add remote drivers if we have any
        if (currentProfile->remotedrivers.isEmpty() == false && currentProfile->remotedrivers.contains("@"))
        {
            for (auto remoteDriver : currentProfile->remotedrivers.split(","))
            {
                QString name, label, host("localhost"), port("7624"), hostport(host + ':' + port);

                // Possible configurations:
                // - device
                // - device@host
                // - device@host:port
                // - @host
                // - @host:port

                {
                    QStringList device_location = remoteDriver.split('@');

                    // device or device@host:port
                    if (device_location.length() > 0)
                        name = device_location[0];

                    // device@host:port or @host:port
                    if (device_location.length() > 1)
                        hostport = device_location[1];
                }

                {
                    QStringList location = hostport.split(':');

                    // host or host:port
                    if (location.length() > 0)
                        host = location[0];

                    // host:port
                    if (location.length() > 1)
                        port = location[1];
                }

                DriverInfo * dv = new DriverInfo(name);
                dv->setRemoteHost(host);
                dv->setRemotePort(port);

                label = name;
                // Remove extra quotes
                label.remove("\"");
                dv->setLabel(label);
                dv->setUniqueLabel(label);
                managedDrivers.append(dv);
            }
        }


        if (haveCCD == false && haveGuider == false && currentProfile->remotedrivers.isEmpty())
        {
            KSNotification::error(i18n("Ekos requires at least one CCD or Guider to operate."));
            managedDrivers.clear();
            m_ekosStatus = Ekos::Error;
            emit ekosStatusChanged(m_ekosStatus);
            return;
        }

        nDevices = managedDrivers.count();
    }
    else
    {
        DriverInfo * remote_indi = new DriverInfo(QString("Ekos Remote Host"));

        remote_indi->setHostParameters(currentProfile->host, currentProfile->port);

        remote_indi->setDriverSource(GENERATED_SOURCE);

        managedDrivers.append(remote_indi);

        haveCCD    = currentProfile->drivers.contains("CCD");
        haveGuider = currentProfile->drivers.contains("Guider");

        Options::setGuiderType(currentProfile->guidertype);

        if (haveCCD == false && haveGuider == false && currentProfile->remotedrivers.isEmpty())
        {
            KSNotification::error(i18n("Ekos requires at least one CCD or Guider to operate."));
            delete (remote_indi);
            nDevices = 0;
            m_ekosStatus = Ekos::Error;
            emit ekosStatusChanged(m_ekosStatus);
            return;
        }

        nDevices = currentProfile->drivers.count();
    }


    // Prioritize profile script drivers over other drivers
    QList<DriverInfo *> sortedList;
    for (const auto &oneRule : qAsConst(profileScripts))
    {
        auto matchingDriver = std::find_if(managedDrivers.begin(), managedDrivers.end(), [oneRule](const auto & oneDriver)
        {
            return oneDriver->getLabel() == oneRule.toObject()["Driver"].toString();
        });

        if (matchingDriver != managedDrivers.end())
        {
            (*matchingDriver)->setStartupRule(oneRule.toObject());
            sortedList.append(*matchingDriver);
        }
    }

    // If we have any profile scripts drivers, let's re-sort managed drivers
    // so that profile script drivers
    if (!sortedList.isEmpty())
    {
        for (auto &oneDriver : managedDrivers)
        {
            if (sortedList.contains(oneDriver) == false)
                sortedList.append(oneDriver);
        }

        managedDrivers = sortedList;
    }

    connect(DriverManager::Instance(), &DriverManager::serverStarted, this,
            &Manager::setServerStarted, Qt::UniqueConnection);
    connect(DriverManager::Instance(), &DriverManager::serverFailed, this,
            &Manager::setServerFailed, Qt::UniqueConnection);
    //    connect(DriverManager::Instance(), &DriverManager::serverTerminated, this,
    //            &Manager::setServerTerminated, Qt::UniqueConnection);
    connect(DriverManager::Instance(), &DriverManager::clientStarted, this,
            &Manager::setClientStarted, Qt::UniqueConnection);
    connect(DriverManager::Instance(), &DriverManager::clientFailed, this,
            &Manager::setClientFailed, Qt::UniqueConnection);
    connect(DriverManager::Instance(), &DriverManager::clientTerminated, this,
            &Manager::setClientTerminated, Qt::UniqueConnection);

    connect(INDIListener::Instance(), &INDIListener::newDevice, this, &Ekos::Manager::processNewDevice);
    connect(INDIListener::Instance(), &INDIListener::deviceRemoved, this, &Ekos::Manager::removeDevice, Qt::DirectConnection);


#ifdef Q_OS_OSX
    if (m_LocalMode || currentProfile->host == "localhost")
    {
        if (isRunning("PTPCamera"))
        {
            if (KMessageBox::Yes ==
                    (KMessageBox::questionYesNo(nullptr,
                                                i18n("Ekos detected that PTP Camera is running and may prevent a Canon or Nikon camera from connecting to Ekos. Do you want to quit PTP Camera now?"),
                                                i18n("PTP Camera"), KStandardGuiItem::yes(), KStandardGuiItem::no(),
                                                "ekos_shutdown_PTPCamera")))
            {
                //TODO is there a better way to do this.
                QProcess p;
                p.start("killall PTPCamera");
                p.waitForFinished();
            }
        }
    }
#endif
    if (m_LocalMode)
    {
        auto executeStartINDIServices = [this]()
        {
            appendLogText(i18n("Starting INDI services..."));

            m_ekosStatus = Ekos::Pending;
            emit ekosStatusChanged(m_ekosStatus);

            DriverManager::Instance()->startDevices(managedDrivers);
        };

        // If INDI server is already running, let's see if we need to shut it down first
        if (isRunning("indiserver"))
        {
            connect(KSMessageBox::Instance(), &KSMessageBox::accepted, this, [this, executeStartINDIServices]()
            {
                KSMessageBox::Instance()->disconnect(this);
                DriverManager::Instance()->stopAllDevices();
                //TODO is there a better way to do this.
                QProcess p;
                const QString program = "pkill";
                QStringList arguments;
                arguments << "indiserver";
                p.start(program, arguments);
                p.waitForFinished();

                QTimer::singleShot(1000, this, executeStartINDIServices);
            });
            connect(KSMessageBox::Instance(), &KSMessageBox::rejected, this, [this, executeStartINDIServices]()
            {
                KSMessageBox::Instance()->disconnect(this);
                executeStartINDIServices();
            });

            KSMessageBox::Instance()->questionYesNo(i18n("Ekos detected an instance of INDI server running. Do you wish to "
                                                    "shut down the existing instance before starting a new one?"),
                                                    i18n("INDI Server"), 5);
        }
        else
            executeStartINDIServices();

    }
    else
    {
        auto runConnection = [this]()
        {
            // If it got cancelled by the user, return immediately.
            if (m_ekosStatus != Ekos::Pending)
                return;

            appendLogText(
                i18n("Connecting to remote INDI server at %1 on port %2 ...", currentProfile->host, currentProfile->port));

            DriverManager::Instance()->connectRemoteHost(managedDrivers.first());
        };

        auto runProfile = [this, runConnection]()
        {
            // If it got cancelled by the user, return immediately.
            if (m_ekosStatus != Ekos::Pending)
                return;

            INDI::WebManager::syncCustomDrivers(currentProfile);
            INDI::WebManager::checkVersion(currentProfile);

            if (INDI::WebManager::areDriversRunning(currentProfile) == false)
            {
                INDI::WebManager::stopProfile(currentProfile);

                if (INDI::WebManager::startProfile(currentProfile) == false)
                {
                    appendLogText(i18n("Failed to start profile on remote INDI Web Manager."));
                    return;
                }

                appendLogText(i18n("Starting profile on remote INDI Web Manager..."));
                m_RemoteManagerStart = true;
            }

            runConnection();
        };

        m_ekosStatus = Ekos::Pending;
        emit ekosStatusChanged(m_ekosStatus);

        // If we need to use INDI Web Manager
        if (currentProfile->INDIWebManagerPort > 0)
        {
            appendLogText(i18n("Establishing communication with remote INDI Web Manager..."));
            m_RemoteManagerStart = false;
            QFutureWatcher<bool> *watcher = new QFutureWatcher<bool>();
            connect(watcher, &QFutureWatcher<bool>::finished, this, [this, runConnection, runProfile, watcher]()
            {
                watcher->deleteLater();

                // If it got cancelled by the user, return immediately.
                if (m_ekosStatus != Ekos::Pending)
                    return;

                // If web manager is online, try to run the profile in it
                if (watcher->result())
                {
                    runProfile();
                }
                // Else, try to connect directly to INDI server as there could be a chance
                // that it is already running.
                else
                {
                    appendLogText(i18n("Warning: INDI Web Manager is not online."));
                    runConnection();
                }

            });

            QFuture<bool> result = INDI::AsyncWebManager::isOnline(currentProfile);
            watcher->setFuture(result);
        }
        else
        {
            runConnection();
        }
    }
}

void Manager::setClientStarted(const QString &host, int port)
{
    if (managedDrivers.size() > 0)
    {
        if (m_LocalMode)
        {
            if (currentProfile->autoConnect)
                appendLogText(i18n("INDI services started on port %1.", port));
            else
                appendLogText(
                    i18n("INDI services started on port %1. Please connect devices.", port));
        }
        else
        {
            appendLogText(
                i18n("INDI services started. Connection to remote INDI server %1:%2 is successful. Waiting for devices...", host, port));
        }
    }

    QTimer::singleShot(MAX_LOCAL_INDI_TIMEOUT, this, &Ekos::Manager::checkINDITimeout);
}

void Manager::setClientFailed(const QString &host, int port, const QString &errorMessage)
{
    if (m_LocalMode)
        appendLogText(i18n("Failed to connect to local INDI server %1:%2", host, port));
    else
        appendLogText(i18n("Failed to connect to remote INDI server %1:%2", host, port));

    //INDIListener::Instance()->disconnect(this);
    //    qDeleteAll(managedDrivers);
    //    managedDrivers.clear();
    m_ekosStatus = Ekos::Error;
    emit ekosStatusChanged(m_ekosStatus);
    KSNotification::error(errorMessage, i18n("Error"), 15);
}

void Manager::setClientTerminated(const QString &host, int port, const QString &errorMessage)
{
    if (m_LocalMode)
        appendLogText(i18n("Lost connection to local INDI server %1:%2", host, port));
    else
        appendLogText(i18n("Lost connection to remote INDI server %1:%2", host, port));

    //INDIListener::Instance()->disconnect(this);
    //    qDeleteAll(managedDrivers);
    //    managedDrivers.clear();
    m_ekosStatus = Ekos::Error;
    emit ekosStatusChanged(m_ekosStatus);
    KSNotification::error(errorMessage, i18n("Error"), 15);
}

void Manager::setServerStarted(const QString &host, int port)
{
    if (m_LocalMode && currentProfile->indihub != INDIHub::None)
    {
        if (QFile(Options::iNDIHubAgent()).exists())
        {
            indiHubAgent = new QProcess();
            QStringList args;

            args << "--indi-server" << QString("%1:%2").arg(host).arg(port);
            if (currentProfile->guidertype == Ekos::Guide::GUIDE_PHD2)
                args << "--phd2-server" << QString("%1:%2").arg(currentProfile->guiderhost).arg(currentProfile->guiderport);
            args << "--mode" << INDIHub::toString(currentProfile->indihub);
            indiHubAgent->start(Options::iNDIHubAgent(), args);

            qCDebug(KSTARS_EKOS) << "Started INDIHub agent.";
        }
    }
}

void Manager::setServerFailed(const QString &host, int port, const QString &message)
{
    Q_UNUSED(host)
    Q_UNUSED(port)
    //INDIListener::Instance()->disconnect(this);
    qDeleteAll(managedDrivers);
    managedDrivers.clear();
    m_ekosStatus = Ekos::Error;
    emit ekosStatusChanged(m_ekosStatus);
    KSNotification::error(message, i18n("Error"), 15);
}

//void Manager::setServerTerminated(const QString &host, int port, const QString &message)
//{
//    if ((m_LocalMode && managedDrivers.first()->getPort() == port) ||
//            (currentProfile->host == host && currentProfile->port == port))
//    {
//        cleanDevices(false);
//        if (indiHubAgent)
//            indiHubAgent->terminate();
//    }

//    INDIListener::Instance()->disconnect(this);
//    qDeleteAll(managedDrivers);
//    managedDrivers.clear();
//    m_ekosStatus = Ekos::Error;
//    emit ekosStatusChanged(m_ekosStatus);
//    KSNotification::error(message, i18n("Error"), 15);
//}

void Manager::checkINDITimeout()
{
    // Don't check anything unless we're still pending
    if (m_ekosStatus != Ekos::Pending)
        return;

    if (nDevices <= 0)
    {
        m_ekosStatus = Ekos::Success;
        emit ekosStatusChanged(m_ekosStatus);
        return;
    }

    if (m_LocalMode)
    {
        QStringList remainingDevices;
        for (auto &drv : managedDrivers)
        {
            if (drv->getDevices().count() == 0)
                remainingDevices << QString("+ %1").arg(
                                     drv->getUniqueLabel().isEmpty() == false ? drv->getUniqueLabel() : drv->getName());
        }

        if (remainingDevices.count() == 1)
        {
            QString message = i18n("Unable to establish:\n%1\nPlease ensure the device is connected and powered on.",
                                   remainingDevices.at(0));
            appendLogText(message);
            KSNotification::event(QLatin1String("IndiServerMessage"), message, KSNotification::EVENT_WARN);
            KNotification::beep(i18n("Ekos startup error"));
        }
        else
        {
            QString message = i18n("Unable to establish the following devices:\n%1\nPlease ensure each device is connected "
                                   "and powered on.", remainingDevices.join("\n"));
            appendLogText(message);
            KSNotification::event(QLatin1String("IndiServerMessage"), message, KSNotification::EVENT_WARN);
            KNotification::beep(i18n("Ekos startup error"));
        }
    }
    else
    {
        QStringList remainingDevices;

        for (auto &driver : currentProfile->drivers.values())
        {
            bool driverFound = false;

            for (auto &device : m_GenericDevices)
            {
                if (device->getBaseDevice()->getDriverName() == driver)
                {
                    driverFound = true;
                    break;
                }
            }

            if (driverFound == false)
                remainingDevices << QString("+ %1").arg(driver);
        }

        if (remainingDevices.count() == 1)
        {
            QString message = i18n("Unable to remotely establish:\n%1\nPlease ensure the device is connected and powered on.",
                                   remainingDevices.at(0));
            appendLogText(message);
            KSNotification::event(QLatin1String("IndiServerMessage"), message, KSNotification::EVENT_WARN);
            KNotification::beep(i18n("Ekos startup error"));
        }
        else
        {
            QString message = i18n("Unable to remotely establish the following devices:\n%1\nPlease ensure each device is connected "
                                   "and powered on.", remainingDevices.join("\n"));
            appendLogText(message);
            KSNotification::event(QLatin1String("IndiServerMessage"), message, KSNotification::EVENT_WARN);
            KNotification::beep(i18n("Ekos startup error"));
        }
    }

    m_ekosStatus = Ekos::Error;
}

void Manager::connectDevices()
{
    // Check if already connected
    int nConnected = 0;

    Ekos::CommunicationStatus previousStatus = m_indiStatus;

    for (auto &device : m_GenericDevices)
    {
        if (device->isConnected())
            nConnected++;
    }
    if (m_GenericDevices.count() == nConnected)
    {
        m_indiStatus = Ekos::Success;
        emit indiStatusChanged(m_indiStatus);
        return;
    }

    m_indiStatus = Ekos::Pending;
    if (previousStatus != m_indiStatus)
        emit indiStatusChanged(m_indiStatus);

    for (auto &device : m_GenericDevices)
    {
        qCDebug(KSTARS_EKOS) << "Connecting " << device->getDeviceName();
        device->Connect();
    }

    connectB->setEnabled(false);
    disconnectB->setEnabled(true);

    appendLogText(i18n("Connecting INDI devices..."));
}

void Manager::disconnectDevices()
{
    for (auto &device : m_GenericDevices)
    {
        qCDebug(KSTARS_EKOS) << "Disconnecting " << device->getDeviceName();
        device->Disconnect();
    }

    appendLogText(i18n("Disconnecting INDI devices..."));
}

void Manager::cleanDevices(bool stopDrivers)
{
    if (m_ekosStatus == Ekos::Idle)
        return;

    if (mountProcess.get())
        mountProcess->stopTimers();

    INDIListener::Instance()->disconnect(this);
    DriverManager::Instance()->disconnect(this);

    if (managedDrivers.isEmpty() == false)
    {
        if (m_LocalMode)
        {
            if (stopDrivers)
                DriverManager::Instance()->stopDevices(managedDrivers);
        }
        else
        {
            if (stopDrivers)
            {
                DriverManager::Instance()->disconnectRemoteHost(managedDrivers.first());

                if (m_RemoteManagerStart && currentProfile->INDIWebManagerPort != -1)
                    INDI::WebManager::stopProfile(currentProfile);
            }
            m_RemoteManagerStart = false;
        }
    }

    reset();

    profileGroup->setEnabled(true);

    appendLogText(i18n("INDI services stopped."));
}

void Manager::processNewDevice(ISD::GenericDevice * device)
{
    qCInfo(KSTARS_EKOS) << "Ekos received a new device: " << device->getDeviceName();

    Ekos::CommunicationStatus previousStatus = m_indiStatus;

    for(auto &oneDevice : m_GenericDevices)
    {
        if (oneDevice->getDeviceName() == device->getDeviceName())
        {
            qCWarning(KSTARS_EKOS) << "Found duplicate device, ignoring...";
            return;
        }
    }

    // Always reset INDI Connection status if we receive a new device
    m_indiStatus = Ekos::Idle;
    if (previousStatus != m_indiStatus)
        emit indiStatusChanged(m_indiStatus);

    m_GenericDevices.append(device);

    nDevices--;

    connect(device, &ISD::GenericDevice::ready, this, &Ekos::Manager::setDeviceReady, Qt::UniqueConnection);
    connect(device, &ISD::GenericDevice::newMount, this, &Ekos::Manager::addMount, Qt::UniqueConnection);
    connect(device, &ISD::GenericDevice::newCamera, this, &Ekos::Manager::addCamera, Qt::UniqueConnection);
    connect(device, &ISD::GenericDevice::newGuider, this, &Ekos::Manager::addGuider, Qt::UniqueConnection);
    connect(device, &ISD::GenericDevice::newFilterWheel, this, &Ekos::Manager::addFilterWheel, Qt::UniqueConnection);
    connect(device, &ISD::GenericDevice::newFocuser, this, &Ekos::Manager::addFocuser, Qt::UniqueConnection);
    connect(device, &ISD::GenericDevice::newDome, this, &Ekos::Manager::addDome, Qt::UniqueConnection);
    connect(device, &ISD::GenericDevice::newRotator, this, &Ekos::Manager::addRotator, Qt::UniqueConnection);
    connect(device, &ISD::GenericDevice::newWeather, this, &Ekos::Manager::addWeather, Qt::UniqueConnection);
    connect(device, &ISD::GenericDevice::newDustCap, this, &Ekos::Manager::addDustCap, Qt::UniqueConnection);
    connect(device, &ISD::GenericDevice::newLightBox, this, &Ekos::Manager::addLightBox, Qt::UniqueConnection);
    connect(device, &ISD::GenericDevice::newGPS, this, &Ekos::Manager::addGPS, Qt::UniqueConnection);

    connect(device, &ISD::GenericDevice::Connected, this, &Ekos::Manager::deviceConnected, Qt::UniqueConnection);
    connect(device, &ISD::GenericDevice::Disconnected, this, &Ekos::Manager::deviceDisconnected, Qt::UniqueConnection);
    connect(device, &ISD::GenericDevice::propertyDefined, this, &Ekos::Manager::processNewProperty, Qt::UniqueConnection);
    connect(device, &ISD::GenericDevice::propertyDeleted, this, &Ekos::Manager::processDeleteProperty, Qt::UniqueConnection);
    connect(device, &ISD::GenericDevice::interfaceDefined, this, &Ekos::Manager::syncActiveDevices, Qt::UniqueConnection);

    connect(device, &ISD::GenericDevice::numberUpdated, this, &Ekos::Manager::processNewNumber, Qt::UniqueConnection);
    connect(device, &ISD::GenericDevice::switchUpdated, this, &Ekos::Manager::processNewSwitch, Qt::UniqueConnection);
    connect(device, &ISD::GenericDevice::textUpdated, this, &Ekos::Manager::processNewText, Qt::UniqueConnection);
    connect(device, &ISD::GenericDevice::lightUpdated, this, &Ekos::Manager::processNewLight, Qt::UniqueConnection);
    connect(device, &ISD::GenericDevice::BLOBUpdated, this, &Ekos::Manager::processNewBLOB, Qt::UniqueConnection);

    // Only look for primary & guider CCDs if we can tell a difference between them
    // otherwise rely on saved options
    if (currentProfile->ccd() != currentProfile->guider())
    {
        for (auto &oneCamera : m_GenericDevices)
        {
            if (oneCamera->getDeviceName().startsWith(currentProfile->ccd(), Qt::CaseInsensitive))
                m_PrimaryCamera = QString(oneCamera->getDeviceName());
            else if (oneCamera->getDeviceName().startsWith(currentProfile->guider(), Qt::CaseInsensitive))
                m_GuideCamera = QString(oneCamera->getDeviceName());
        }
    }

    if (nDevices <= 0)
    {
        m_ekosStatus = Ekos::Success;
        emit ekosStatusChanged(m_ekosStatus);

        connectB->setEnabled(true);
        disconnectB->setEnabled(false);

        if (m_LocalMode == false && nDevices == 0)
        {
            if (currentProfile->autoConnect)
                appendLogText(i18n("Remote devices established."));
            else
                appendLogText(i18n("Remote devices established. Please connect devices."));
        }

        if (!m_PortSelector)
        {
            portSelectorB->setEnabled(false);
            m_PortSelector.reset(new Selector::Dialog(KStars::Instance()));
            connect(m_PortSelector.get(), &Selector::Dialog::accepted, this, &Manager::setPortSelectionComplete);
        }
    }
}

void Manager::deviceConnected()
{
    connectB->setEnabled(false);
    disconnectB->setEnabled(true);
    processINDIB->setEnabled(false);

    Ekos::CommunicationStatus previousStatus = m_indiStatus;

    auto device = qobject_cast<ISD::GenericDevice *>(sender());

    if (Options::verboseLogging())
    {
        qCInfo(KSTARS_EKOS) << device->getDeviceName()
                            << "Version:" << device->getDriverVersion()
                            << "Interface:" << device->getDriverInterface()
                            << "is connected.";
    }

    int nConnectedDevices = 0;

    for (auto &device : m_GenericDevices)
    {
        if (device->isConnected())
            nConnectedDevices++;
    }

    qCDebug(KSTARS_EKOS) << nConnectedDevices << " devices connected out of " << m_GenericDevices.count();

    if (nConnectedDevices >= currentProfile->drivers.count())
        //if (nConnectedDevices >= genericDevices.count())
    {
        m_indiStatus = Ekos::Success;
        qCInfo(KSTARS_EKOS) << "All INDI devices are now connected.";
    }
    else
        m_indiStatus = Ekos::Pending;

    if (previousStatus != m_indiStatus)
        emit indiStatusChanged(m_indiStatus);

    if (device->getDriverInterface() & INDI::BaseDevice::TELESCOPE_INTERFACE)
    {
        if (mountProcess.get() != nullptr)
        {
            mountProcess->setEnabled(true);
            if (alignProcess.get() != nullptr)
                alignProcess->setEnabled(true);
        }
    }
    else if (device->getDriverInterface() & INDI::BaseDevice::CCD_INTERFACE)
    {
        if (captureProcess.get() != nullptr)
        {
            captureProcess->setEnabled(true);
            capturePreview->setEnabled(true);
        }
        if (focusProcess.get() != nullptr)
            focusProcess->setEnabled(true);
        if (alignProcess.get() != nullptr)
        {
            if (mountProcess.get() && mountProcess->isEnabled())
                alignProcess->setEnabled(true);
            else
                alignProcess->setEnabled(false);
        }
        if (guideProcess.get() != nullptr)
            guideProcess->setEnabled(true);
    }
    else if (device->getDriverInterface() & INDI::BaseDevice::FOCUSER_INTERFACE)
    {
        if (focusProcess.get() != nullptr)
            focusProcess->setEnabled(true);
    }

    if (Options::neverLoadConfig())
        return;

    INDIConfig tConfig = Options::loadConfigOnConnection() ? LOAD_LAST_CONFIG : LOAD_DEFAULT_CONFIG;

    for (auto &oneDevice : m_GenericDevices)
    {
        if (oneDevice == device)
        {
            connect(device, &ISD::GenericDevice::switchUpdated, this, &Ekos::Manager::watchDebugProperty, Qt::UniqueConnection);

            auto configProp = device->getBaseDevice()->getSwitch("CONFIG_PROCESS");
            if (configProp && configProp->getState() == IPS_IDLE)
                device->setConfig(tConfig);
            break;
        }
    }
}

void Manager::deviceDisconnected()
{
    ISD::GenericDevice * dev = static_cast<ISD::GenericDevice *>(sender());

    Ekos::CommunicationStatus previousStatus = m_indiStatus;

    if (dev != nullptr)
    {
        if (dev->getState("CONNECTION") == IPS_ALERT)
            m_indiStatus = Ekos::Error;
        else if (dev->getState("CONNECTION") == IPS_BUSY)
            m_indiStatus = Ekos::Pending;
        else
            m_indiStatus = Ekos::Idle;

        if (Options::verboseLogging())
            qCDebug(KSTARS_EKOS) << dev->getDeviceName() << " is disconnected.";

        // In case a device fails to connect, display and log a useful message for the user.
        if (m_indiStatus == Ekos::Error)
        {
            QString message = i18n("%1 failed to connect.\nPlease ensure the device is connected and powered on.",
                                   dev->getDeviceName());
            appendLogText(message);
            KSNotification::event(QLatin1String("IndiServerMessage"), message, KSNotification::EVENT_WARN);
        }
        else if (m_indiStatus == Ekos::Idle)
        {
            QString message = i18n("%1 is disconnected.", dev->getDeviceName());
            appendLogText(message);
        }
    }
    else
        m_indiStatus = Ekos::Idle;

    if (previousStatus != m_indiStatus)
        emit indiStatusChanged(m_indiStatus);

    connectB->setEnabled(true);
    disconnectB->setEnabled(false);
    processINDIB->setEnabled(true);

    if (dev != nullptr && dev->getBaseDevice() &&
            (dev->getDriverInterface() & INDI::BaseDevice::TELESCOPE_INTERFACE))
    {
        if (mountProcess.get() != nullptr)
            mountProcess->setEnabled(false);
    }
}

void Manager::addMount(ISD::Mount * device)
{
    syncGenericDevice(device->genericDevice());

    ekosLiveClient->message()->sendMounts();
    ekosLiveClient->message()->sendScopes();

    appendLogText(i18n("%1 is online.", device->getDeviceName()));
}

void Manager::addCamera(ISD::Camera * device)
{
    syncGenericDevice(device->genericDevice());

    captureProcess->setEnabled(true);
    capturePreview->setEnabled(true);

    ekosLiveClient.get()->message()->sendCameras();
    ekosLiveClient.get()->media()->registerCameras();

    appendLogText(i18n("%1 is online.", device->getDeviceName()));
}

void Manager::addFilterWheel(ISD::FilterWheel * device)
{
    syncGenericDevice(device->genericDevice());

    ekosLiveClient.get()->message()->sendFilterWheels();

    appendLogText(i18n("%1 filter is online.", device->getDeviceName()));
}

void Manager::addFocuser(ISD::Focuser *device)
{
    syncGenericDevice(device->genericDevice());

    appendLogText(i18n("%1 focuser is online.", device->getDeviceName()));
}

void Manager::addRotator(ISD::Rotator *device)
{
    syncGenericDevice(device->genericDevice());

    appendLogText(i18n("Rotator %1 is online.", device->getDeviceName()));
}

void Manager::addDome(ISD::Dome * device)
{
    syncGenericDevice(device->genericDevice());

    ekosLiveClient.get()->message()->sendDomes();

    appendLogText(i18n("%1 is online.", device->getDeviceName()));
}

void Manager::addWeather(ISD::Weather * device)
{
    syncGenericDevice(device->genericDevice());

    appendLogText(i18n("%1 Weather is online.", device->getDeviceName()));
}

void Manager::addGPS(ISD::GPS * device)
{
    syncGenericDevice(device->genericDevice());

    appendLogText(i18n("%1 GPS is online.", device->getDeviceName()));
}

void Manager::addDustCap(ISD::DustCap * device)
{
    syncGenericDevice(device->genericDevice());

    ekosLiveClient.get()->message()->sendCaps();

    appendLogText(i18n("%1 Dust cap is online.", device->getDeviceName()));
}

void Manager::addLightBox(ISD::LightBox * device)
{
    syncGenericDevice(device->genericDevice());

    appendLogText(i18n("%1 Light box is online.", device->getDeviceName()));
}

void Manager::syncGenericDevice(ISD::GenericDevice *device)
{
    createModules(device);

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    /// Cameras
    ////////////////////////////////////////////////////////////////////////////////////////////////////
    auto camera = dynamic_cast<ISD::Camera*>(device->getConcreteDevice(INDI::BaseDevice::CCD_INTERFACE));
    if (camera)
    {
        // Capture Module
        if (captureProcess && captureProcess->addCamera(camera))
        {
            bool rc = false;
            if (Options::defaultCaptureCCD().isEmpty() == false)
                rc = captureProcess->setCamera(Options::defaultCaptureCCD());
            if (rc == false && m_PrimaryCamera.isEmpty() == false)
                captureProcess->setCamera(m_PrimaryCamera);
        }

        // Focus Module
        if (focusProcess && focusProcess->addCamera(camera))
        {
            if (camera->hasCooler())
                focusProcess->addTemperatureSource(camera->genericDevice());

            bool rc = false;
            if (Options::defaultFocusCCD().isEmpty() == false)
                rc = focusProcess->setCamera(Options::defaultFocusCCD());
            if (rc == false && m_PrimaryCamera.isEmpty() == false)
                focusProcess->setCamera(m_PrimaryCamera);
        }

        // Align Module
        if (alignProcess && alignProcess->addCamera(camera))
        {
            bool rc = false;
            if (Options::defaultAlignCCD().isEmpty() == false)
                rc = alignProcess->setCamera(Options::defaultAlignCCD());
            if (rc == false && m_PrimaryCamera.isEmpty() == false)
                alignProcess->setCamera(m_PrimaryCamera);

        }

        // Guide Module
        if (guideProcess && guideProcess->addCamera(camera))
        {
            bool rc = false;
            if (Options::defaultGuideCCD().isEmpty() == false)
                rc = guideProcess->setCamera(Options::defaultGuideCCD());
            if (rc == false && m_GuideCamera.isEmpty() == false)
                guideProcess->setCamera(m_GuideCamera);
        }
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    /// Mount
    ////////////////////////////////////////////////////////////////////////////////////////////////////
    auto mount = dynamic_cast<ISD::Mount*>(device->getConcreteDevice(INDI::BaseDevice::TELESCOPE_INTERFACE));
    if (mount)
    {
        double primaryScopeFL = 0, primaryScopeAperture = 0, guideScopeFL = 0, guideScopeAperture = 0;
        getCurrentProfileTelescopeInfo(primaryScopeFL, primaryScopeAperture, guideScopeFL, guideScopeAperture);
        if (mountProcess && mountProcess->addMount(mount))
        {
            // Save telescope info in mount driver
            mountProcess->setTelescopeInfo(QList<double>() << primaryScopeFL << primaryScopeAperture << guideScopeFL <<
                                           guideScopeAperture);
        }

        if (captureProcess)
            captureProcess->addMount(mount);

        if (guideProcess && guideProcess->addMount(mount))
            guideProcess->setTelescopeInfo(primaryScopeFL, primaryScopeAperture, guideScopeFL, guideScopeAperture);

        if (alignProcess && alignProcess->addMount(mount))
            alignProcess->setTelescopeInfo(primaryScopeFL, primaryScopeAperture, guideScopeFL, guideScopeAperture);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    /// Focuser
    ////////////////////////////////////////////////////////////////////////////////////////////////////
    auto focuser = dynamic_cast<ISD::Focuser*>(device->getConcreteDevice(INDI::BaseDevice::FOCUSER_INTERFACE));
    if (focuser)
    {
        if (focusProcess && focusProcess->addFocuser(focuser))
        {
            if (Options::defaultFocusFocuser().isEmpty() == false)
                focusProcess->setFocuser(Options::defaultFocusFocuser());

            // Temperature sources.
            focusProcess->addTemperatureSource(focuser->genericDevice());
        }
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    /// Filter Wheel
    ////////////////////////////////////////////////////////////////////////////////////////////////////
    auto filterWheel = dynamic_cast<ISD::FilterWheel*>(device->getConcreteDevice(INDI::BaseDevice::FILTER_INTERFACE));
    if (filterWheel)
    {
        if (captureProcess)
            captureProcess->addFilterWheel(filterWheel);

        if (focusProcess)
            focusProcess->addFilterWheel(filterWheel);

        if (alignProcess)
            alignProcess->addFilterWheel(filterWheel);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    /// Rotators
    ////////////////////////////////////////////////////////////////////////////////////////////////////
    auto rotator = dynamic_cast<ISD::Rotator*>(device->getConcreteDevice(INDI::BaseDevice::ROTATOR_INTERFACE));
    if (rotator)
    {
        if (captureProcess)
            captureProcess->addRotator(rotator);

        if (alignProcess)
            alignProcess->addRotator(rotator);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    /// Domes
    ////////////////////////////////////////////////////////////////////////////////////////////////////
    auto dome = dynamic_cast<ISD::Dome*>(device->getConcreteDevice(INDI::BaseDevice::DOME_INTERFACE));
    if (dome)
    {
        //        if (domeProcess)
        //        {
        //            domeProcess->addDome(dome);
        //            if (observatoryProcess && observatoryProcess->getDomeModel())
        //                observatoryProcess->getDomeModel()->initModel(domeProcess.get());
        //        }

        if (captureProcess)
            captureProcess->addDome(dome);

        if (alignProcess)
            alignProcess->addDome(dome);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    /// Weather
    ////////////////////////////////////////////////////////////////////////////////////////////////////
    auto weather = dynamic_cast<ISD::Weather*>(device->getConcreteDevice(INDI::BaseDevice::WEATHER_INTERFACE));
    if (weather)
    {
        //        if (weatherProcess)
        //        {
        //            weatherProcess->addWeather(weather);
        //            if (observatoryProcess && observatoryProcess->getWeatherModel())
        //                observatoryProcess->getWeatherModel()->initModel(weatherProcess.get());
        //        }

        if (focusProcess)
            focusProcess->addTemperatureSource(weather->genericDevice());
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    /// GPS
    ////////////////////////////////////////////////////////////////////////////////////////////////////
    auto gps = dynamic_cast<ISD::GPS*>(device->getConcreteDevice(INDI::BaseDevice::GPS_INTERFACE));
    if (gps)
    {
        if (mountProcess)
            mountProcess->addGPS(gps);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    /// Dust Cap
    ////////////////////////////////////////////////////////////////////////////////////////////////////
    auto dustCap = dynamic_cast<ISD::DustCap*>(device->getConcreteDevice(INDI::BaseDevice::DUSTCAP_INTERFACE));
    if (dustCap)
    {
        if (captureProcess)
            captureProcess->addDustCap(dustCap);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////
    /// Light Box
    ////////////////////////////////////////////////////////////////////////////////////////////////////
    auto lightBox = dynamic_cast<ISD::LightBox*>(device->getConcreteDevice(INDI::BaseDevice::LIGHTBOX_INTERFACE));
    if (lightBox)
    {
        if (captureProcess)
            captureProcess->addLightBox(lightBox);
    }
}

void Manager::removeDevice(ISD::GenericDevice * device)
{
    if (alignProcess)
        alignProcess->removeDevice(device);
    if (captureProcess)
        captureProcess->removeDevice(device);
    if (focusProcess)
        focusProcess->removeDevice(device);
    if (mountProcess)
        mountProcess->removeDevice(device);
    if (guideProcess)
        guideProcess->removeDevice(device);
    // TODO add Observatory
    if (m_PortSelector)
        m_PortSelector->removeDevice(device->getDeviceName());

    DarkLibrary::Instance()->removeDevice(device);

    appendLogText(i18n("%1 is offline.", device->getDeviceName()));

    // #1 Remove from Generic Devices
    // Generic devices are ALL the devices we receive from INDI server
    // Whether Ekos cares about them (i.e. selected equipment) or extra devices we
    // do not care about
    for (auto &oneDevice : m_GenericDevices)
    {
        if (oneDevice->getDeviceName() == device->getDeviceName())
        {
            m_GenericDevices.removeOne(device);
        }
    }

    if (m_GenericDevices.isEmpty())
    {
        cleanDevices();
        removeTabs();
    }
}

void Manager::processNewText(ITextVectorProperty * tvp)
{
    ekosLiveClient.get()->message()->processNewText(tvp);

    if (!strcmp(tvp->name, "FILTER_NAME"))
    {
        ekosLiveClient.get()->message()->sendFilterWheels();
    }

    if (!strcmp(tvp->name, "ACTIVE_DEVICES"))
    {
        syncActiveDevices();
    }
}

void Manager::processNewSwitch(ISwitchVectorProperty * svp)
{
    ekosLiveClient.get()->message()->processNewSwitch(svp);
}

void Manager::processNewLight(ILightVectorProperty * lvp)
{
    ekosLiveClient.get()->message()->processNewLight(lvp);
}

void Manager::processNewBLOB(IBLOB * bp)
{
    ekosLiveClient.get()->media()->processNewBLOB(bp);
}

void Manager::processNewNumber(INumberVectorProperty * nvp)
{
    ekosLiveClient.get()->message()->processNewNumber(nvp);

    if (!strcmp(nvp->name, "CCD_INFO") || !strcmp(nvp->name, "GUIDER_INFO") || !strcmp(nvp->name, "CCD_FRAME") ||
            !strcmp(nvp->name, "GUIDER_FRAME"))
    {
        if (focusProcess.get() != nullptr)
            focusProcess->syncCameraInfo();

        if (guideProcess.get() != nullptr)
            guideProcess->syncCameraInfo();

        if (alignProcess.get() != nullptr)
            alignProcess->syncCameraInfo();

        return;
    }
}

void Manager::processDeleteProperty(const QString &name)
{
    ISD::GenericDevice * deviceInterface = qobject_cast<ISD::GenericDevice *>(sender());
    ekosLiveClient.get()->message()->processDeleteProperty(deviceInterface->getDeviceName(), name);
}

void Manager::processNewProperty(INDI::Property prop)
{
    auto device = qobject_cast<ISD::GenericDevice *>(sender());

    settleTimer.start();

    ekosLiveClient.get()->message()->processNewProperty(prop);

    if (prop->isNameMatch("DEVICE_PORT_SCAN") || prop->isNameMatch("CONNECTION_TYPE"))
    {
        if (!m_PortSelector)
        {
            m_PortSelector.reset(new Selector::Dialog(KStars::Instance()));
            connect(m_PortSelector.get(), &Selector::Dialog::accepted, this, &Manager::setPortSelectionComplete);
        }
        m_PortSelectorTimer.start();
        portSelectorB->setEnabled(true);
        m_PortSelector->addDevice(device);
        return;
    }

    // Check if we need to turn on DEBUG for logging purposes
    if (prop->isNameMatch("DEBUG"))
    {
        uint16_t interface = device->getDriverInterface();
        if ( opsLogs->getINDIDebugInterface() & interface )
        {
            // Check if we need to enable debug logging for the INDI drivers.
            auto debugSP = prop->getSwitch();
            debugSP->at(0)->setState(ISS_ON);
            debugSP->at(1)->setState(ISS_OFF);
            device->getDriverInfo()->getClientManager()->sendNewSwitch(debugSP);
        }
        return;
    }

    // Handle debug levels for logging purposes
    if (prop->isNameMatch("DEBUG_LEVEL"))
    {
        uint16_t interface = device->getDriverInterface();
        // Check if the logging option for the specific device class is on and if the device interface matches it.
        if ( opsLogs->getINDIDebugInterface() & interface )
        {
            // Turn on everything
            auto debugLevel = prop->getSwitch();
            for (auto &it : *debugLevel)
                it.setState(ISS_ON);

            device->getDriverInfo()->getClientManager()->sendNewSwitch(debugLevel);
        }
        return;
    }

    if (prop->isNameMatch("ACTIVE_DEVICES"))
    {
        if (device->getDriverInterface() > 0)
            syncActiveDevices();

        return;
    }

    if (prop->isNameMatch("ASTROMETRY_SOLVER"))
    {
        for (auto &oneDevice : m_GenericDevices)
        {
            if (oneDevice->getDeviceName() == prop->getDeviceName())
            {
                initAlign();
                alignProcess->setAstrometryDevice(oneDevice);
                break;
            }
        }

        return;
    }

    if (focusProcess.get() != nullptr && strstr(prop->getName(), "FOCUS_"))
    {
        focusProcess->checkFocuser();
        return;
    }
}

void Manager::processTabChange()
{
    auto currentWidget = toolsWidget->currentWidget();

    if (alignProcess && alignProcess.get() == currentWidget)
    {
        auto alignReady = alignProcess->isEnabled() == false && alignProcess->isParserOK();
        auto captureReady = captureProcess && captureProcess->isEnabled();
        auto mountReady = mountProcess && mountProcess->isEnabled();
        if (alignReady && captureReady && mountReady)
            alignProcess->setEnabled(true);

        alignProcess->checkCamera();
    }
    else if (captureProcess && currentWidget == captureProcess.get())
    {
        captureProcess->checkCamera();
    }
    else if (focusProcess && currentWidget == focusProcess.get())
    {
        focusProcess->checkCamera();
    }
    else if (guideProcess && currentWidget == guideProcess.get())
    {
        guideProcess->checkCamera();
    }

    updateLog();
}

void Manager::updateLog()
{
    QWidget * currentWidget = toolsWidget->currentWidget();

    if (currentWidget == setupTab)
        ekosLogOut->setPlainText(m_LogText.join("\n"));
    else if (currentWidget == alignProcess.get())
        ekosLogOut->setPlainText(alignProcess->getLogText());
    else if (currentWidget == captureProcess.get())
        ekosLogOut->setPlainText(captureProcess->getLogText());
    else if (currentWidget == focusProcess.get())
        ekosLogOut->setPlainText(focusProcess->getLogText());
    else if (currentWidget == guideProcess.get())
        ekosLogOut->setPlainText(guideProcess->getLogText());
    else if (currentWidget == mountProcess.get())
        ekosLogOut->setPlainText(mountProcess->getLogText());
    else if (currentWidget == schedulerProcess.get())
        ekosLogOut->setPlainText(schedulerProcess->getLogText());
    //    else if (currentWidget == observatoryProcess.get())
    //        ekosLogOut->setPlainText(observatoryProcess->getLogText());

#ifdef Q_OS_OSX
    repaint(); //This is a band-aid for a bug in QT 5.10.0
#endif
}

void Manager::appendLogText(const QString &text)
{
    m_LogText.insert(0, i18nc("log entry; %1 is the date, %2 is the text", "%1 %2",
                              KStarsData::Instance()->lt().toString("yyyy-MM-ddThh:mm:ss"), text));

    qCInfo(KSTARS_EKOS) << text;

    emit newLog(text);

    updateLog();
}

void Manager::clearLog()
{
    QWidget * currentWidget = toolsWidget->currentWidget();

    if (currentWidget == setupTab)
    {
        m_LogText.clear();
        updateLog();
    }
    else if (currentWidget == alignProcess.get())
        alignProcess->clearLog();
    else if (currentWidget == captureProcess.get())
        captureProcess->clearLog();
    else if (currentWidget == focusProcess.get())
        focusProcess->clearLog();
    else if (currentWidget == guideProcess.get())
        guideProcess->clearLog();
    else if (currentWidget == mountProcess.get())
        mountProcess->clearLog();
    else if (currentWidget == schedulerProcess.get())
        schedulerProcess->clearLog();
    //    else if (currentWidget == observatoryProcess.get())
    //        observatoryProcess->clearLog();
}

void Manager::initCapture()
{
    if (captureProcess.get() != nullptr)
        return;

    captureProcess.reset(new Capture());
    emit newModule("Capture");

    captureProcess->setEnabled(false);
    capturePreview->shareCaptureProcess(captureProcess.get());
    int index = addModuleTab(EkosModule::Capture, captureProcess.get(), QIcon(":/icons/ekos_ccd.png"));
    toolsWidget->tabBar()->setTabToolTip(index, i18nc("Charge-Coupled Device", "CCD"));
    if (Options::ekosLeftIcons())
    {
        QTransform trans;
        trans.rotate(90);
        QIcon icon  = toolsWidget->tabIcon(index);
        QPixmap pix = icon.pixmap(QSize(48, 48));
        icon        = QIcon(pix.transformed(trans));
        toolsWidget->setTabIcon(index, icon);
    }
    connect(captureProcess.get(), &Ekos::Capture::newLog, this, &Ekos::Manager::updateLog);
    connect(captureProcess.get(), &Ekos::Capture::newStatus, this, &Ekos::Manager::updateCaptureStatus);
    connect(captureProcess.get(), &Ekos::Capture::newImage, this, &Ekos::Manager::updateCaptureProgress);
    connect(captureProcess.get(), &Ekos::Capture::driverTimedout, this, &Ekos::Manager::restartDriver);
    connect(captureProcess.get(), &Ekos::Capture::newExposureProgress, this, &Ekos::Manager::updateExposureProgress);
    capturePreview->setEnabled(true);

    captureProcess->setFilterManager(filterManager);
    // display capture status changes
    connect(filterManager.data(), &FilterManager::newStatus, capturePreview->captureStatusWidget,
            &CaptureStatusWidget::setFilterState);
    // display target drift
    connect(schedulerProcess.get(), &Ekos::Scheduler::targetDistance,
            captureProcess.get(), &Ekos::Capture::updateTargetDistance,  Qt::UniqueConnection);
    connect(schedulerProcess.get(), &Ekos::Scheduler::targetDistance, this, [this](double distance)
    {
        capturePreview->updateTargetDistance(distance);
    });


    connectModules();
}

void Manager::initAlign()
{
    if (alignProcess.get() != nullptr)
        return;

    alignProcess.reset(new Ekos::Align(currentProfile));

    emit newModule("Align");

    double primaryScopeFL = 0, primaryScopeAperture = 0, guideScopeFL = 0, guideScopeAperture = 0;
    getCurrentProfileTelescopeInfo(primaryScopeFL, primaryScopeAperture, guideScopeFL, guideScopeAperture);
    alignProcess->setTelescopeInfo(primaryScopeFL, primaryScopeAperture, guideScopeFL, guideScopeAperture);

    alignProcess->setEnabled(false);
    int index = addModuleTab(EkosModule::Align, alignProcess.get(), QIcon(":/icons/ekos_align.png"));
    toolsWidget->tabBar()->setTabToolTip(index, i18n("Align"));
    connect(alignProcess.get(), &Ekos::Align::newLog, this, &Ekos::Manager::updateLog);
    if (Options::ekosLeftIcons())
    {
        QTransform trans;
        trans.rotate(90);
        QIcon icon  = toolsWidget->tabIcon(index);
        QPixmap pix = icon.pixmap(QSize(48, 48));
        icon        = QIcon(pix.transformed(trans));
        toolsWidget->setTabIcon(index, icon);
    }

    alignProcess->setFilterManager(filterManager);

    connectModules();
}

void Manager::initFocus()
{
    if (focusProcess.get() != nullptr)
        return;

    focusProcess.reset(new Ekos::Focus());

    emit newModule("Focus");

    int index    = addModuleTab(EkosModule::Focus, focusProcess.get(), QIcon(":/icons/ekos_focus.png"));

    toolsWidget->tabBar()->setTabToolTip(index, i18n("Focus"));

    // Focus <---> Manager connections
    connect(focusProcess.get(), &Ekos::Focus::newLog, this, &Ekos::Manager::updateLog);
    connect(focusProcess.get(), &Ekos::Focus::newStatus, this, &Ekos::Manager::updateFocusStatus);
    connect(focusProcess.get(), &Ekos::Focus::newStarPixmap, focusManager, &Ekos::FocusManager::updateFocusStarPixmap);
    connect(focusProcess.get(), &Ekos::Focus::newHFR, this, &Ekos::Manager::updateCurrentHFR);

    // connect HFR plot widget
    connect(focusProcess.get(), &Ekos::Focus::initHFRPlot, focusManager->hfrVPlot, &FocusHFRVPlot::init);
    connect(focusProcess.get(), &Ekos::Focus::redrawHFRPlot, focusManager->hfrVPlot, &FocusHFRVPlot::redraw);
    connect(focusProcess.get(), &Ekos::Focus::newHFRPlotPosition, focusManager->hfrVPlot, &FocusHFRVPlot::addPosition);
    // connect signal/slot for adding a new position with errors to be shown as error bars
    connect(focusProcess.get(), &Ekos::Focus::newHFRPlotPositionWithSigma, focusManager->hfrVPlot,
            &FocusHFRVPlot::addPositionWithSigma);
    connect(focusProcess.get(), &Ekos::Focus::drawPolynomial, focusManager->hfrVPlot, &FocusHFRVPlot::drawPolynomial);
    connect(focusProcess.get(), &Ekos::Focus::setTitle, focusManager->hfrVPlot, &FocusHFRVPlot::setTitle);
    connect(focusProcess.get(), &Ekos::Focus::updateTitle, focusManager->hfrVPlot, &FocusHFRVPlot::updateTitle);
    connect(focusProcess.get(), &Ekos::Focus::minimumFound, focusManager->hfrVPlot, &FocusHFRVPlot::drawMinimum);
    // setup signal/slots for Linear 1 Pass focus algo
    connect(focusProcess.get(), &Ekos::Focus::drawCurve, focusManager->hfrVPlot, &FocusHFRVPlot::drawCurve);

    // Focus <---> Filter Manager connections
    focusProcess->setFilterManager(filterManager);
    connect(filterManager.data(), &Ekos::FilterManager::checkFocus, focusProcess.get(), &Ekos::Focus::checkFocus,
            Qt::UniqueConnection);
    connect(focusProcess.get(), &Ekos::Focus::newStatus, filterManager.data(), &Ekos::FilterManager::setFocusStatus,
            Qt::UniqueConnection);
    connect(filterManager.data(), &Ekos::FilterManager::newFocusOffset, focusProcess.get(), &Ekos::Focus::adjustFocusOffset,
            Qt::UniqueConnection);
    connect(focusProcess.get(), &Ekos::Focus::focusPositionAdjusted, filterManager.data(),
            &Ekos::FilterManager::setFocusOffsetComplete, Qt::UniqueConnection);
    connect(focusProcess.get(), &Ekos::Focus::absolutePositionChanged, filterManager.data(),
            &Ekos::FilterManager::setFocusAbsolutePosition, Qt::UniqueConnection);

    if (Options::ekosLeftIcons())
    {
        QTransform trans;
        trans.rotate(90);
        QIcon icon  = toolsWidget->tabIcon(index);
        QPixmap pix = icon.pixmap(QSize(48, 48));
        icon        = QIcon(pix.transformed(trans));
        toolsWidget->setTabIcon(index, icon);
    }

    focusManager->init(focusProcess.get());
    focusManager->setEnabled(true);

    for (auto &oneDevice : m_GenericDevices)
    {
        auto prop1  = oneDevice->getProperty("CCD_TEMPERATURE");
        auto prop2  = oneDevice->getProperty("FOCUSER_TEMPERATURE");
        auto prop3  = oneDevice->getProperty("WEATHER_PARAMETERS");
        if (prop1 || prop2 || prop3)
            focusProcess->addTemperatureSource(oneDevice);
    }

    connectModules();
}

void Manager::updateCurrentHFR(double newHFR, int position)
{
    focusManager->updateCurrentHFR(newHFR);

    QJsonObject cStatus =
    {
        {"hfr", newHFR},
        {"pos", position}
    };

    ekosLiveClient.get()->message()->updateFocusStatus(cStatus);
}

void Manager::updateSigmas(double ra, double de)
{
    guideManager->updateSigmas(ra, de);

    QJsonObject cStatus = { {"rarms", ra}, {"derms", de} };

    ekosLiveClient.get()->message()->updateGuideStatus(cStatus);
}

void Manager::initMount()
{
    if (mountProcess.get() != nullptr)
        return;

    mountProcess.reset(new Ekos::Mount());

    emit newModule("Mount");

    int index    = addModuleTab(EkosModule::Mount, mountProcess.get(), QIcon(":/icons/ekos_mount.png"));

    toolsWidget->tabBar()->setTabToolTip(index, i18n("Mount"));
    connect(mountProcess.get(), &Ekos::Mount::newLog, this, &Ekos::Manager::updateLog);
    connect(mountProcess.get(), &Ekos::Mount::newCoords, this, &Ekos::Manager::updateMountCoords);
    connect(mountProcess.get(), &Ekos::Mount::newStatus, this, &Ekos::Manager::updateMountStatus);
    connect(mountProcess.get(), &Ekos::Mount::newTargetName, this, [this](const QString & name)
    {
        mountTarget->setText(name);
        ekosLiveClient.get()->message()->updateMountStatus(QJsonObject({{"target", name}}));
    });
    connect(mountProcess.get(), &Ekos::Mount::pierSideChanged, [&](ISD::Mount::PierSide side)
    {
        ekosLiveClient.get()->message()->updateMountStatus(QJsonObject({{"pierSide", side}}));
    });
    connect(mountProcess.get(), &Ekos::Mount::newMeridianFlipStatus, [&](Mount::MeridianFlipStatus status)
    {
        ekosLiveClient.get()->message()->updateMountStatus(QJsonObject(
        {
            {"meridianFlipStatus", status},
        }));
    });
    connect(mountProcess.get(), &Ekos::Mount::newMeridianFlipText, [&](const QString & text)
    {
        // Throttle this down
        ekosLiveClient.get()->message()->updateMountStatus(QJsonObject(
        {
            {"meridianFlipText", text},
        }), mountProcess->meridianFlipStatus() == Mount::FLIP_NONE);
    });


    connect(mountProcess.get(), &Ekos::Mount::slewRateChanged, [&](int slewRate)
    {
        QJsonObject status = { { "slewRate", slewRate} };
        ekosLiveClient.get()->message()->updateMountStatus(status);
    });

    if (Options::ekosLeftIcons())
    {
        QTransform trans;
        trans.rotate(90);
        QIcon icon  = toolsWidget->tabIcon(index);
        QPixmap pix = icon.pixmap(QSize(48, 48));
        icon        = QIcon(pix.transformed(trans));
        toolsWidget->setTabIcon(index, icon);
    }

    mountGroup->setEnabled(true);
    capturePreview->shareMountProcess(mountProcess.get());

    connectModules();
}

void Manager::initGuide()
{
    if (guideProcess.get() == nullptr)
    {
        guideProcess.reset(new Ekos::Guide());

        emit newModule("Guide");

        double primaryScopeFL = 0, primaryScopeAperture = 0, guideScopeFL = 0, guideScopeAperture = 0;
        getCurrentProfileTelescopeInfo(primaryScopeFL, primaryScopeAperture, guideScopeFL, guideScopeAperture);
        // Save telescope info in mount driver
        guideProcess->setTelescopeInfo(primaryScopeFL, primaryScopeAperture, guideScopeFL, guideScopeAperture);
    }

    if (toolsWidget->indexOf(guideProcess.get()) == -1)
    {
        // if (managedDevices.contains(KSTARS_TELESCOPE) && managedDevices.value(KSTARS_TELESCOPE)->isConnected())
        // guideProcess->addMount(managedDevices.value(KSTARS_TELESCOPE));

        int index = addModuleTab(EkosModule::Guide, guideProcess.get(), QIcon(":/icons/ekos_guide.png"));
        toolsWidget->tabBar()->setTabToolTip(index, i18n("Guide"));
        connect(guideProcess.get(), &Ekos::Guide::newLog, this, &Ekos::Manager::updateLog);
        connect(guideProcess.get(), &Ekos::Guide::driverTimedout, this, &Ekos::Manager::restartDriver);

        guideManager->setEnabled(true);

        connect(guideProcess.get(), &Ekos::Guide::newStatus, this, &Ekos::Manager::updateGuideStatus);
        connect(guideProcess.get(), &Ekos::Guide::newStarPixmap, guideManager, &Ekos::GuideManager::updateGuideStarPixmap);
        connect(guideProcess.get(), &Ekos::Guide::newAxisSigma, this, &Ekos::Manager::updateSigmas);
        connect(guideProcess.get(), &Ekos::Guide::newAxisDelta, [&](double ra, double de)
        {
            QJsonObject status = { { "drift_ra", ra}, {"drift_de", de} };
            ekosLiveClient.get()->message()->updateGuideStatus(status);
        });

        if (Options::ekosLeftIcons())
        {
            QTransform trans;
            trans.rotate(90);
            QIcon icon  = toolsWidget->tabIcon(index);
            QPixmap pix = icon.pixmap(QSize(48, 48));
            icon        = QIcon(pix.transformed(trans));
            toolsWidget->setTabIcon(index, icon);
        }
        guideManager->init(guideProcess.get());
    }

    connectModules();
}

//void Manager::initObservatory()
//{
//    if (observatoryProcess.get() == nullptr)
//    {
//        // Initialize the Observatory Module
//        observatoryProcess.reset(new Ekos::Observatory());

//        emit newModule("Observatory");

//        int index = addModuleTab(EkosModule::Observatory, observatoryProcess.get(), QIcon(":/icons/ekos_observatory.png"));
//        toolsWidget->tabBar()->setTabToolTip(index, i18n("Observatory"));
//        connect(observatoryProcess.get(), &Ekos::Observatory::newLog, this, &Ekos::Manager::updateLog);
//    }
//}

void Manager::addGuider(ISD::Guider * device)
{
    appendLogText(i18n("Guider port from %1 is ready.", device->getDeviceName()));

    initGuide();

    guideProcess->addGuider(device);

    if (Options::defaultGuideGuider().isEmpty() == false)
        guideProcess->setGuider(Options::defaultGuideGuider());
}

void Manager::removeTabs()
{
    disconnect(toolsWidget, &QTabWidget::currentChanged, this, &Ekos::Manager::processTabChange);

    for (int i = numPermanentTabs; i < toolsWidget->count(); i++)
        toolsWidget->removeTab(i);

    alignProcess.reset();
    captureProcess.reset();
    focusProcess.reset();
    guideProcess.reset();
    mountProcess.reset();
    //observatoryProcess.reset();

    connect(toolsWidget, &QTabWidget::currentChanged, this, &Ekos::Manager::processTabChange, Qt::UniqueConnection);
}

bool Manager::isRunning(const QString &process)
{
    QProcess ps;
#ifdef Q_OS_OSX
    ps.start("pgrep", QStringList() << process);
    ps.waitForFinished();
    QString output = ps.readAllStandardOutput();
    return output.length() > 0;
#else
    ps.start("ps", QStringList() << "-o"
             << "comm"
             << "--no-headers"
             << "-C" << process);
    ps.waitForFinished();
    QString output = ps.readAllStandardOutput();
    return output.contains(process);
#endif
}

void Manager::addObjectToScheduler(SkyObject * object)
{
    if (schedulerProcess.get() != nullptr)
        schedulerProcess->addObject(object);
}

QString Manager::getCurrentJobName()
{
    return schedulerProcess->getCurrentJobName();
}

bool Manager::setProfile(const QString &profileName)
{
    int index = profileCombo->findText(profileName);

    if (index < 0)
        return false;

    profileCombo->setCurrentIndex(index);

    return true;
}

void Manager::editNamedProfile(const QJsonObject &profileInfo)
{
    ProfileEditor editor(this);
    setProfile(profileInfo["name"].toString());
    currentProfile = getCurrentProfile();
    editor.setPi(currentProfile);
    editor.setSettings(profileInfo);
    editor.saveProfile();
}

void Manager::addNamedProfile(const QJsonObject &profileInfo)
{
    ProfileEditor editor(this);

    editor.setSettings(profileInfo);
    editor.saveProfile();
    profiles.clear();
    loadProfiles();
    profileCombo->setCurrentIndex(profileCombo->count() - 1);
    currentProfile = getCurrentProfile();
}

void Manager::deleteNamedProfile(const QString &name)
{
    currentProfile = getCurrentProfile();

    for (auto &pi : profiles)
    {
        // Do not delete an actively running profile
        // Do not delete simulator profile
        if (pi->name == "Simulators" || pi->name != name || (pi.get() == currentProfile && ekosStatus() != Idle))
            continue;

        KStarsData::Instance()->userdb()->DeleteProfile(pi.get());

        profiles.clear();
        loadProfiles();
        currentProfile = getCurrentProfile();
        return;
    }
}

QJsonObject Manager::getNamedProfile(const QString &name)
{
    QJsonObject profileInfo;

    // Get current profile
    for (auto &pi : profiles)
    {
        if (name == pi->name)
            return pi->toJson();
    }

    return QJsonObject();
}

QStringList Manager::getProfiles()
{
    QStringList profiles;

    for (int i = 0; i < profileCombo->count(); i++)
        profiles << profileCombo->itemText(i);

    return profiles;
}

void Manager::addProfile()
{
    ProfileEditor editor(this);

    if (editor.exec() == QDialog::Accepted)
    {
        profiles.clear();
        loadProfiles();
        profileCombo->setCurrentIndex(profileCombo->count() - 1);
    }

    currentProfile = getCurrentProfile();
}

void Manager::editProfile()
{
    ProfileEditor editor(this);

    currentProfile = getCurrentProfile();

    editor.setPi(currentProfile);

    if (editor.exec() == QDialog::Accepted)
    {
        int currentIndex = profileCombo->currentIndex();

        profiles.clear();
        loadProfiles();
        profileCombo->setCurrentIndex(currentIndex);
    }

    currentProfile = getCurrentProfile();
}

void Manager::deleteProfile()
{
    currentProfile = getCurrentProfile();

    if (currentProfile->name == "Simulators")
        return;

    auto executeDeleteProfile = [&]()
    {
        KStarsData::Instance()->userdb()->DeleteProfile(currentProfile);
        profiles.clear();
        loadProfiles();
        currentProfile = getCurrentProfile();
    };

    connect(KSMessageBox::Instance(), &KSMessageBox::accepted, this, [this, executeDeleteProfile]()
    {
        //QObject::disconnect(KSMessageBox::Instance(), &KSMessageBox::accepted, this, nullptr);
        KSMessageBox::Instance()->disconnect(this);
        executeDeleteProfile();
    });

    KSMessageBox::Instance()->questionYesNo(i18n("Are you sure you want to delete the profile?"),
                                            i18n("Confirm Delete"));

}

void Manager::wizardProfile()
{
    ProfileWizard wz;
    if (wz.exec() != QDialog::Accepted)
        return;

    ProfileEditor editor(this);

    editor.setProfileName(wz.profileName);
    editor.setAuxDrivers(wz.selectedAuxDrivers());
    if (wz.useInternalServer == false)
        editor.setHostPort(wz.host, wz.port);
    editor.setWebManager(wz.useWebManager);
    editor.setGuiderType(wz.selectedExternalGuider());
    // Disable connection options
    editor.setConnectionOptionsEnabled(false);

    if (editor.exec() == QDialog::Accepted)
    {
        profiles.clear();
        loadProfiles();
        profileCombo->setCurrentIndex(profileCombo->count() - 1);
    }

    currentProfile = getCurrentProfile();
}

ProfileInfo * Manager::getCurrentProfile()
{
    ProfileInfo * currProfile = nullptr;

    // Get current profile
    for (auto &pi : profiles)
    {
        if (profileCombo->currentText() == pi->name)
        {
            currProfile = pi.get();
            break;
        }
    }

    return currProfile;
}

void Manager::updateProfileLocation(ProfileInfo * pi)
{
    if (pi->city.isEmpty() == false)
    {
        bool cityFound = KStars::Instance()->setGeoLocation(pi->city, pi->province, pi->country);
        if (cityFound)
            appendLogText(i18n("Site location updated to %1.", KStarsData::Instance()->geo()->fullName()));
        else
            appendLogText(i18n("Failed to update site location to %1. City not found.",
                               KStarsData::Instance()->geo()->fullName()));
    }
}

void Manager::updateMountStatus(ISD::Mount::Status status)
{
    static ISD::Mount::Status lastStatus = ISD::Mount::MOUNT_IDLE;

    if (status == lastStatus)
        return;

    lastStatus = status;

    mountStatus->setText(mountProcess.get()->statusString());
    mountStatus->setStyleSheet(QString());

    switch (status)
    {
        case ISD::Mount::MOUNT_PARKING:
        case ISD::Mount::MOUNT_SLEWING:
        case ISD::Mount::MOUNT_MOVING:
            mountPI->setColor(QColor(KStarsData::Instance()->colorScheme()->colorNamed("TargetColor")));
            if (mountPI->isAnimated() == false)
                mountPI->startAnimation();
            break;

        case ISD::Mount::MOUNT_TRACKING:
            mountPI->setColor(Qt::darkGreen);
            if (mountPI->isAnimated() == false)
                mountPI->startAnimation();

            break;

        case ISD::Mount::MOUNT_PARKED:
            mountStatus->setStyleSheet("font-weight:bold;background-color:red;border:2px solid black;");
            if (mountPI->isAnimated())
                mountPI->stopAnimation();
            break;

        default:
            if (mountPI->isAnimated())
                mountPI->stopAnimation();
    }

    QJsonObject cStatus =
    {
        {"status", mountProcess.get()->statusString(false)}
    };

    ekosLiveClient.get()->message()->updateMountStatus(cStatus);
}

void Manager::updateMountCoords(const SkyPoint position, ISD::Mount::PierSide pierSide, const dms &ha)
{
    Q_UNUSED(pierSide)
    raOUT->setText(position.ra().toHMSString());
    decOUT->setText(position.dec().toDMSString());
    azOUT->setText(position.az().toDMSString());
    altOUT->setText(position.alt().toDMSString());

    QJsonObject cStatus =
    {
        {"ra", dms::fromString(raOUT->text(), false).Degrees()},
        {"de", dms::fromString(decOUT->text(), true).Degrees()},
        {"ra0", position.ra0().Degrees()},
        {"de0", position.dec0().Degrees()},
        {"az", dms::fromString(azOUT->text(), true).Degrees()},
        {"at", dms::fromString(altOUT->text(), true).Degrees()},
        {"ha", ha.Degrees()},
    };

    ekosLiveClient.get()->message()->updateMountStatus(cStatus, true);
}

void Manager::updateCaptureStatus(Ekos::CaptureState status)
{
    capturePreview->updateCaptureStatus(status);

    switch (status)
    {
        case Ekos::CAPTURE_IDLE:
        /* Fall through */
        case Ekos::CAPTURE_ABORTED:
        /* Fall through */
        case Ekos::CAPTURE_COMPLETE:
            if (focusModule() && focusModule()->status() == Ekos::FOCUS_COMPLETE)
                focusManager->stopAnimation();
            m_CountdownTimer.stop();
            break;
        case Ekos::CAPTURE_CAPTURING:
            m_CountdownTimer.start();
            break;
        default:
            break;
    }

    QJsonObject cStatus =
    {
        {"status", captureStates[status]},
        {"seqt", capturePreview->captureCountsWidget->sequenceRemainingTime->text()},
        {"ovt", capturePreview->captureCountsWidget->overallRemainingTime->text()}
    };

    ekosLiveClient.get()->message()->updateCaptureStatus(cStatus);
}

void Manager::updateCaptureProgress(Ekos::SequenceJob * job, const QSharedPointer<FITSData> &data)
{
    capturePreview->updateJobProgress(job, data);

    QJsonObject status =
    {
        {"seqv", job->getCompleted()},
        {"seqr", job->getCoreProperty(SequenceJob::SJ_Count).toInt()},
        {"seql", capturePreview->captureCountsWidget->sequenceRemainingTime->text()}
    };

    ekosLiveClient.get()->message()->updateCaptureStatus(status);

    //const QString filename = ;
    //if (!filename.isEmpty() && job->getStatus() == SequenceJob::JOB_BUSY)
    if (data && job->getStatus() == JOB_BUSY)
    {
        QString uuid = QUuid::createUuid().toString();
        uuid = uuid.remove(QRegularExpression("[-{}]"));

        if (Options::useSummaryPreview())
            ekosLiveClient.get()->media()->sendView(m_SummaryView, uuid);
        else
            ekosLiveClient.get()->media()->sendData(data, uuid);

        if (job->getCoreProperty(SequenceJob::SJ_Preview).toBool() == false)
            ekosLiveClient.get()->cloud()->upload(data, uuid);

    }
}

void Manager::updateExposureProgress(Ekos::SequenceJob * job)
{
    QJsonObject status
    {
        {"expv", job->getExposeLeft()},
        {"expr", job->getCoreProperty(SequenceJob::SJ_Exposure).toDouble()}
    };

    ekosLiveClient.get()->message()->updateCaptureStatus(status);
}

void Manager::updateCaptureCountDown()
{
    capturePreview->updateCaptureCountDown(-1);

    QJsonObject status =
    {
        {"seqt", capturePreview->captureCountsWidget->sequenceRemainingTime->text()},
        {"ovt", capturePreview->captureCountsWidget->overallRemainingTime->text()}
    };

    ekosLiveClient.get()->message()->updateCaptureStatus(status);
}


void Manager::updateFocusStatus(Ekos::FocusState status)
{
    focusManager->updateFocusStatus(status);

    QJsonObject cStatus =
    {
        {"status", getFocusStatusString(status, false)}
    };

    ekosLiveClient.get()->message()->updateFocusStatus(cStatus);
}

void Manager::updateGuideStatus(Ekos::GuideState status)
{
    guideManager->updateGuideStatus(status);
    QJsonObject cStatus =
    {
        {"status", getGuideStatusString(status, false)}
    };

    ekosLiveClient.get()->message()->updateGuideStatus(cStatus);
}

void Manager::setTarget(SkyObject * o)
{
    mountTarget->setText(o->name());
    ekosLiveClient.get()->message()->updateMountStatus(QJsonObject({{"target", o->name()}}));
}

void Manager::showEkosOptions()
{
    QWidget * currentWidget = toolsWidget->currentWidget();

    if (alignProcess.get() && alignProcess.get() == currentWidget)
    {
        KConfigDialog * alignSettings = KConfigDialog::exists("alignsettings");
        if (alignSettings)
        {
            alignSettings->setEnabled(true);
            alignSettings->show();
        }
        return;
    }

    if (guideProcess.get() && guideProcess.get() == currentWidget)
    {
        KConfigDialog::showDialog("guidesettings");
        return;
    }

    if (ekosOptionsWidget == nullptr)
    {
        optionsB->click();
    }
    else if (KConfigDialog::showDialog("settings"))
    {
        KConfigDialog * cDialog = KConfigDialog::exists("settings");
        cDialog->setCurrentPage(ekosOptionsWidget);
    }
}

void Manager::getCurrentProfileTelescopeInfo(double &primaryFocalLength, double &primaryAperture, double &guideFocalLength,
        double &guideAperture)
{
    ProfileInfo * pi = getCurrentProfile();
    if (pi)
    {
        int primaryScopeID = 0, guideScopeID = 0;
        primaryScopeID = pi->primaryscope;
        guideScopeID = pi->guidescope;
        if (primaryScopeID > 0 || guideScopeID > 0)
        {
            // Get all OAL equipment filter list
            QList<OAL::Scope *> m_scopeList;
            KStarsData::Instance()->userdb()->GetAllScopes(m_scopeList);
            for(auto oneScope : m_scopeList)
            {
                if (oneScope->id().toInt() == primaryScopeID)
                {
                    primaryFocalLength = oneScope->focalLength();
                    primaryAperture = oneScope->aperture();
                }

                if (oneScope->id().toInt() == guideScopeID)
                {
                    guideFocalLength = oneScope->focalLength();
                    guideAperture = oneScope->aperture();
                }
            }

            qDeleteAll(m_scopeList);
        }
    }
}

void Manager::updateDebugInterfaces()
{
    KSUtils::Logging::SyncFilterRules();

    for (ISD::GenericDevice * device : m_GenericDevices)
    {
        auto debugProp = device->getProperty("DEBUG");
        if (!debugProp)
            continue;

        auto debugSP = debugProp->getSwitch();

        // Check if the debug interface matches the driver device class
        if ( ( opsLogs->getINDIDebugInterface() & device->getDriverInterface() ) &&
                debugSP->sp[0].s != ISS_ON)
        {
            debugSP->at(0)->setState(ISS_ON);
            debugSP->at(1)->setState(ISS_OFF);
            device->getDriverInfo()->getClientManager()->sendNewSwitch(debugSP);
            appendLogText(i18n("Enabling debug logging for %1...", device->getDeviceName()));
        }
        else if ( !( opsLogs->getINDIDebugInterface() & device->getDriverInterface() ) &&
                  debugSP->sp[0].s != ISS_OFF)
        {
            debugSP->at(0)->setState(ISS_OFF);
            debugSP->at(1)->setState(ISS_ON);
            device->getDriverInfo()->getClientManager()->sendNewSwitch(debugSP);
            appendLogText(i18n("Disabling debug logging for %1...", device->getDeviceName()));
        }

        if (opsLogs->isINDISettingsChanged())
            device->setConfig(SAVE_CONFIG);
    }
}

void Manager::watchDebugProperty(ISwitchVectorProperty * svp)
{
    if (!strcmp(svp->name, "DEBUG"))
    {
        ISD::GenericDevice * deviceInterface = qobject_cast<ISD::GenericDevice *>(sender());

        // We don't process pure general interfaces
        if (deviceInterface->getDriverInterface() == INDI::BaseDevice::GENERAL_INTERFACE)
            return;

        // If debug was turned off, but our logging policy requires it then turn it back on.
        // We turn on debug logging if AT LEAST one driver interface is selected by the logging settings
        if (svp->s == IPS_OK && svp->sp[0].s == ISS_OFF &&
                (opsLogs->getINDIDebugInterface() & deviceInterface->getDriverInterface()))
        {
            svp->sp[0].s = ISS_ON;
            svp->sp[1].s = ISS_OFF;
            deviceInterface->getDriverInfo()->getClientManager()->sendNewSwitch(svp);
            appendLogText(i18n("Re-enabling debug logging for %1...", deviceInterface->getDeviceName()));
        }
        // To turn off debug logging, NONE of the driver interfaces should be enabled in logging settings.
        // For example, if we have CCD+FilterWheel device and CCD + Filter Wheel logging was turned on in
        // the log settings, then if the user turns off only CCD logging, the debug logging is NOT
        // turned off until he turns off Filter Wheel logging as well.
        else if (svp->s == IPS_OK && svp->sp[0].s == ISS_ON
                 && !(opsLogs->getINDIDebugInterface() & deviceInterface->getDriverInterface()))
        {
            svp->sp[0].s = ISS_OFF;
            svp->sp[1].s = ISS_ON;
            deviceInterface->getDriverInfo()->getClientManager()->sendNewSwitch(svp);
            appendLogText(i18n("Re-disabling debug logging for %1...", deviceInterface->getDeviceName()));
        }
    }
}

void Manager::announceEvent(const QString &message, KSNotification::EventType event)
{
    ekosLiveClient.get()->message()->sendEvent(message, event);
}

void Manager::connectModules()
{
    DarkLibrary::Instance()->disconnect(this);
    connect(DarkLibrary::Instance(), &DarkLibrary::newImage, this, [this](const QSharedPointer<FITSData> &data)
    {
        ekosLiveClient.get()->media()->sendData(data, "+D");
    });
    connect(DarkLibrary::Instance(), &DarkLibrary::newFrame, ekosLiveClient.get()->media(), &EkosLive::Media::sendModuleFrame);

    // Guide <---> Capture connections
    if (captureProcess.get() && guideProcess.get())
    {
        //        captureProcess.get()->disconnect(guideProcess.get());
        //        guideProcess.get()->disconnect(captureProcess.get());

        // Guide Limits
        connect(guideProcess.get(), &Ekos::Guide::newStatus, captureProcess.get(), &Ekos::Capture::setGuideStatus,
                Qt::UniqueConnection);
        connect(guideProcess.get(), &Ekos::Guide::newAxisDelta, captureProcess.get(), &Ekos::Capture::setGuideDeviation,
                Qt::UniqueConnection);

        // Dithering
        connect(captureProcess.get(), &Ekos::Capture::newStatus, guideProcess.get(), &Ekos::Guide::setCaptureStatus,
                Qt::UniqueConnection);

        // Guide Head
        connect(captureProcess.get(), &Ekos::Capture::suspendGuiding, guideProcess.get(), &Ekos::Guide::suspend,
                Qt::UniqueConnection);
        connect(captureProcess.get(), &Ekos::Capture::resumeGuiding, guideProcess.get(), &Ekos::Guide::resume,
                Qt::UniqueConnection);
        connect(guideProcess.get(), &Ekos::Guide::guideChipUpdated, captureProcess.get(), &Ekos::Capture::setGuideChip,
                Qt::UniqueConnection);

        // Meridian Flip
        connect(captureProcess.get(), &Ekos::Capture::meridianFlipStarted, guideProcess.get(), &Ekos::Guide::abort,
                Qt::UniqueConnection);
        connect(captureProcess.get(), &Ekos::Capture::meridianFlipCompleted, guideProcess.get(),
                &Ekos::Guide::guideAfterMeridianFlip, Qt::UniqueConnection);
    }

    // Guide <---> Mount connections
    if (guideProcess.get() && mountProcess.get())
    {
        // Parking
        connect(mountProcess.get(), &Ekos::Mount::newStatus, guideProcess.get(), &Ekos::Guide::setMountStatus,
                Qt::UniqueConnection);
        connect(mountProcess.get(), &Ekos::Mount::newCoords, guideProcess.get(), &Ekos::Guide::setMountCoords,
                Qt::UniqueConnection);

    }

    // Focus <---> Guide connections
    if (guideProcess.get() && focusProcess.get())
    {
        // Suspend
        connect(focusProcess.get(), &Ekos::Focus::suspendGuiding, guideProcess.get(), &Ekos::Guide::suspend, Qt::UniqueConnection);
        connect(focusProcess.get(), &Ekos::Focus::resumeGuiding, guideProcess.get(), &Ekos::Guide::resume, Qt::UniqueConnection);
    }

    // Capture <---> Focus connections
    if (captureProcess.get() && focusProcess.get())
    {
        // Check focus HFR value
        connect(captureProcess.get(), &Ekos::Capture::checkFocus, focusProcess.get(), &Ekos::Focus::checkFocus,
                Qt::UniqueConnection);

        // Reset Focus
        connect(captureProcess.get(), &Ekos::Capture::resetFocus, focusProcess.get(), &Ekos::Focus::resetFrame,
                Qt::UniqueConnection);

        // Abort Focus
        connect(captureProcess.get(), &Ekos::Capture::abortFocus, focusProcess.get(), &Ekos::Focus::abort,
                Qt::UniqueConnection);

        // New Focus Status
        connect(focusProcess.get(), &Ekos::Focus::newStatus, captureProcess.get(), &Ekos::Capture::setFocusStatus,
                Qt::UniqueConnection);

        // New Focus HFR
        connect(focusProcess.get(), &Ekos::Focus::newHFR, captureProcess.get(), &Ekos::Capture::setHFR, Qt::UniqueConnection);

        // New Focus temperature delta
        connect(focusProcess.get(), &Ekos::Focus::newFocusTemperatureDelta, captureProcess.get(),
                &Ekos::Capture::setFocusTemperatureDelta, Qt::UniqueConnection);

        // Meridian Flip
        connect(captureProcess.get(), &Ekos::Capture::meridianFlipStarted, focusProcess.get(), &Ekos::Focus::meridianFlipStarted,
                Qt::UniqueConnection);
    }

    // Capture <---> Align connections
    if (captureProcess.get() && alignProcess.get())
    {
        // Alignment flag
        connect(alignProcess.get(), &Ekos::Align::newStatus, captureProcess.get(), &Ekos::Capture::setAlignStatus,
                Qt::UniqueConnection);
        // Solver data
        connect(alignProcess.get(), &Ekos::Align::newSolverResults, captureProcess.get(),  &Ekos::Capture::setAlignResults,
                Qt::UniqueConnection);
        // Capture Status
        connect(captureProcess.get(), &Ekos::Capture::newStatus, alignProcess.get(), &Ekos::Align::setCaptureStatus,
                Qt::UniqueConnection);
    }

    // Capture <---> Mount connections
    if (captureProcess.get() && mountProcess.get())
    {
        // Register both modules since both are now created and ready
        // In case one module misses the DBus signal, then it will be correctly initialized.
        captureProcess->registerNewModule("Mount");
        mountProcess->registerNewModule("Capture");

        // Meridian Flip states
        connect(captureProcess.get(), &Ekos::Capture::meridianFlipStarted, mountProcess.get(), &Ekos::Mount::disableAltLimits,
                Qt::UniqueConnection);
        connect(captureProcess.get(), &Ekos::Capture::meridianFlipCompleted, mountProcess.get(), &Ekos::Mount::enableAltLimits,
                Qt::UniqueConnection);
        connect(captureProcess.get(), &Ekos::Capture::newMeridianFlipStatus, mountProcess.get(),
                &Ekos::Mount::meridianFlipStatusChanged, Qt::UniqueConnection);
        connect(mountProcess.get(), &Ekos::Mount::newMeridianFlipStatus, captureProcess.get(),
                &Ekos::Capture::meridianFlipStatusChanged, Qt::UniqueConnection);

        // Mount Status
        connect(mountProcess.get(), &Ekos::Mount::newStatus, captureProcess.get(), &Ekos::Capture::setMountStatus,
                Qt::UniqueConnection);
    }

    // Capture <---> EkosLive connections
    if (captureProcess.get() && ekosLiveClient.get())
    {
        //captureProcess.get()->disconnect(ekosLiveClient.get()->message());

        connect(captureProcess.get(), &Ekos::Capture::dslrInfoRequested, ekosLiveClient.get()->message(),
                &EkosLive::Message::requestDSLRInfo, Qt::UniqueConnection);
        connect(captureProcess.get(), &Ekos::Capture::sequenceChanged, ekosLiveClient.get()->message(),
                &EkosLive::Message::sendCaptureSequence, Qt::UniqueConnection);
        connect(captureProcess.get(), &Ekos::Capture::settingsUpdated, ekosLiveClient.get()->message(),
                &EkosLive::Message::sendCaptureSettings, Qt::UniqueConnection);
    }

    // Scheduler <---> EkosLive connections
    if(schedulerProcess.get() && ekosLiveClient.get())
    {
        connect(schedulerProcess.get(), &Ekos::Scheduler::jobsUpdated, ekosLiveClient.get()->message(),
                &EkosLive::Message::sendSchedulerJobs, Qt::UniqueConnection);
    }

    // Focus <---> Align connections
    if (focusProcess.get() && alignProcess.get())
    {
        connect(focusProcess.get(), &Ekos::Focus::newStatus, alignProcess.get(), &Ekos::Align::setFocusStatus,
                Qt::UniqueConnection);
    }

    // Focus <---> Mount connections
    if (focusProcess.get() && mountProcess.get())
    {
        connect(mountProcess.get(), &Ekos::Mount::newStatus, focusProcess.get(), &Ekos::Focus::setMountStatus,
                Qt::UniqueConnection);
        connect(mountProcess.get(), &Ekos::Mount::newCoords, focusProcess.get(), &Ekos::Focus::setMountCoords,
                Qt::UniqueConnection);
    }

    // Mount <---> Align connections
    if (mountProcess.get() && alignProcess.get())
    {
        connect(mountProcess.get(), &Ekos::Mount::newStatus, alignProcess.get(), &Ekos::Align::setMountStatus,
                Qt::UniqueConnection);
        connect(mountProcess.get(), &Ekos::Mount::newTarget,  alignProcess.get(), &Ekos::Align::setTarget,
                Qt::UniqueConnection);
        connect(mountProcess.get(), &Ekos::Mount::newCoords,  alignProcess.get(), &Ekos::Align::setTelescopeCoordinates,
                Qt::UniqueConnection);
        connect(alignProcess.get(), &Ekos::Align::newPAAStage, mountProcess.get(), &Ekos::Mount::paaStageChanged,
                Qt::UniqueConnection);
    }

    // Mount <---> Guide connections
    if (mountProcess.get() && guideProcess.get())
    {
        connect(mountProcess.get(), &Ekos::Mount::pierSideChanged, guideProcess.get(), &Ekos::Guide::setPierSide,
                Qt::UniqueConnection);
    }

    // Align <--> EkosLive connections
    if (alignProcess.get() && ekosLiveClient.get())
    {
        //        alignProcess.get()->disconnect(ekosLiveClient.get()->message());
        //        alignProcess.get()->disconnect(ekosLiveClient.get()->media());

        connect(alignProcess.get(), &Ekos::Align::newStatus, ekosLiveClient.get()->message(), &EkosLive::Message::setAlignStatus,
                Qt::UniqueConnection);
        connect(alignProcess.get(), &Ekos::Align::newSolution, ekosLiveClient.get()->message(),
                &EkosLive::Message::setAlignSolution, Qt::UniqueConnection);
        connect(alignProcess.get()->polarAlignmentAssistant(), &Ekos::PolarAlignmentAssistant::newPAHStage,
                ekosLiveClient.get()->message(), &EkosLive::Message::setPAHStage,
                Qt::UniqueConnection);
        connect(alignProcess.get()->polarAlignmentAssistant(), &Ekos::PolarAlignmentAssistant::newPAHMessage,
                ekosLiveClient.get()->message(),
                &EkosLive::Message::setPAHMessage, Qt::UniqueConnection);
        connect(alignProcess.get()->polarAlignmentAssistant(), &Ekos::PolarAlignmentAssistant::PAHEnabled,
                ekosLiveClient.get()->message(), &EkosLive::Message::setPAHEnabled,
                Qt::UniqueConnection);
        connect(alignProcess.get()->polarAlignmentAssistant(), &Ekos::PolarAlignmentAssistant::polarResultUpdated,
                ekosLiveClient.get()->message(),
                &EkosLive::Message::setPolarResults, Qt::UniqueConnection);
        connect(alignProcess.get()->polarAlignmentAssistant(), &Ekos::PolarAlignmentAssistant::updatedErrorsChanged,
                ekosLiveClient.get()->message(),
                &EkosLive::Message::setUpdatedErrors, Qt::UniqueConnection);
        connect(alignProcess.get()->polarAlignmentAssistant(), &Ekos::PolarAlignmentAssistant::newCorrectionVector,
                ekosLiveClient.get()->media(),
                &EkosLive::Media::setCorrectionVector, Qt::UniqueConnection);

        connect(alignProcess.get(), &Ekos::Align::newImage, ekosLiveClient.get()->media(), &EkosLive::Media::sendModuleFrame,
                Qt::UniqueConnection);
        connect(alignProcess.get(), &Ekos::Align::newFrame, ekosLiveClient.get()->media(), &EkosLive::Media::sendUpdatedFrame,
                Qt::UniqueConnection);

        connect(alignProcess.get(), &Ekos::Align::settingsUpdated, ekosLiveClient.get()->message(),
                &EkosLive::Message::sendAlignSettings, Qt::UniqueConnection);

        connect(alignProcess.get(), &Ekos::Align::manualRotatorChanged, ekosLiveClient.get()->message(),
                &EkosLive::Message::sendManualRotatorStatus, Qt::UniqueConnection);
    }

    // Focus <--> EkosLive Connections
    if (focusProcess.get() && ekosLiveClient.get())
    {
        connect(focusProcess.get(), &Ekos::Focus::settingsUpdated, ekosLiveClient.get()->message(),
                &EkosLive::Message::sendFocusSettings, Qt::UniqueConnection);
        connect(focusProcess.get(), &Ekos::Focus::newImage, ekosLiveClient.get()->media(), &EkosLive::Media::sendModuleFrame,
                Qt::UniqueConnection);
    }

    // Guide <--> EkosLive Connections
    if (guideProcess.get() && ekosLiveClient.get())
    {
        connect(guideProcess.get(), &Ekos::Guide::settingsUpdated, ekosLiveClient.get()->message(),
                &EkosLive::Message::sendGuideSettings, Qt::UniqueConnection);
        connect(guideProcess.get(), &Ekos::Guide::newImage, ekosLiveClient.get()->media(), &EkosLive::Media::sendModuleFrame,
                Qt::UniqueConnection);
    }

    // Analyze connections.
    if (analyzeProcess.get())
    {
        connect(schedulerProcess.get(), &Ekos::Scheduler::jobStarted,
                analyzeProcess.get(), &Ekos::Analyze::schedulerJobStarted, Qt::UniqueConnection);
        connect(schedulerProcess.get(), &Ekos::Scheduler::jobEnded,
                analyzeProcess.get(), &Ekos::Analyze::schedulerJobEnded, Qt::UniqueConnection);
        connect(schedulerProcess.get(), &Ekos::Scheduler::targetDistance,
                analyzeProcess.get(), &Ekos::Analyze::newTargetDistance,  Qt::UniqueConnection);

        if (captureProcess.get())
        {
            connect(captureProcess.get(), &Ekos::Capture::captureComplete,
                    analyzeProcess.get(), &Ekos::Analyze::captureComplete, Qt::UniqueConnection);
            connect(captureProcess.get(), &Ekos::Capture::captureStarting,
                    analyzeProcess.get(), &Ekos::Analyze::captureStarting, Qt::UniqueConnection);
            connect(captureProcess.get(), &Ekos::Capture::captureAborted,
                    analyzeProcess.get(), &Ekos::Analyze::captureAborted, Qt::UniqueConnection);
#if 0
            // Meridian Flip
            connect(captureProcess.get(), &Ekos::Capture::meridianFlipStarted,
                    analyzeProcess.get(), &Ekos::Analyze::meridianFlipStarted, Qt::UniqueConnection);
            connect(captureProcess.get(), &Ekos::Capture::meridianFlipCompleted,
                    analyzeProcess.get(), &Ekos::Analyze::meridianFlipComplete, Qt::UniqueConnection);
#endif
        }
        if (guideProcess.get())
        {
            connect(guideProcess.get(), &Ekos::Guide::newStatus,
                    analyzeProcess.get(), &Ekos::Analyze::guideState, Qt::UniqueConnection);

            connect(guideProcess.get(), &Ekos::Guide::guideStats,
                    analyzeProcess.get(), &Ekos::Analyze::guideStats, Qt::UniqueConnection);
        }
    }
    if (focusProcess.get())
    {
        connect(focusProcess.get(), &Ekos::Focus::autofocusComplete,
                analyzeProcess.get(), &Ekos::Analyze::autofocusComplete, Qt::UniqueConnection);
        connect(focusProcess.get(), &Ekos::Focus::autofocusStarting,
                analyzeProcess.get(), &Ekos::Analyze::autofocusStarting, Qt::UniqueConnection);
        connect(focusProcess.get(), &Ekos::Focus::autofocusAborted,
                analyzeProcess.get(), &Ekos::Analyze::autofocusAborted, Qt::UniqueConnection);
        connect(focusProcess.get(), &Ekos::Focus::newFocusTemperatureDelta,
                analyzeProcess.get(), &Ekos::Analyze::newTemperature, Qt::UniqueConnection);
    }
    if (alignProcess.get())
    {
        connect(alignProcess.get(), &Ekos::Align::newStatus,
                analyzeProcess.get(), &Ekos::Analyze::alignState, Qt::UniqueConnection);

    }
    if (mountProcess.get())
    {
        connect(mountProcess.get(), &Ekos::Mount::newStatus,
                analyzeProcess.get(), &Ekos::Analyze::mountState, Qt::UniqueConnection);
        connect(mountProcess.get(), &Ekos::Mount::newCoords,
                analyzeProcess.get(), &Ekos::Analyze::mountCoords, Qt::UniqueConnection);
        connect(mountProcess.get(), &Ekos::Mount::newMeridianFlipStatus,
                analyzeProcess.get(), &Ekos::Analyze::mountFlipStatus, Qt::UniqueConnection);
    }
}

void Manager::setEkosLiveConnected(bool enabled)
{
    ekosLiveClient.get()->setConnected(enabled);
}

void Manager::setEkosLiveConfig(bool onlineService, bool rememberCredentials, bool autoConnect)
{
    ekosLiveClient.get()->setConfig(onlineService, rememberCredentials, autoConnect);
}

void Manager::setEkosLiveUser(const QString &username, const QString &password)
{
    ekosLiveClient.get()->setUser(username, password);
}

bool Manager::ekosLiveStatus()
{
    return ekosLiveClient.get()->isConnected();
}

QList<ISD::GenericDevice *> Manager::findDevicesByInterface(uint32_t interface)
{
    QList<ISD::GenericDevice *> devices;
    for (auto &oneDevice : m_GenericDevices)
    {
        if (oneDevice->getDriverInterface() & interface)
            devices.append(oneDevice);
    }

    return devices;
}

void Manager::syncActiveDevices()
{
    for (auto oneDevice : m_GenericDevices)
    {
        // Find out what ACTIVE_DEVICES properties this driver needs
        // and update it from the existing drivers.
        auto tvp = oneDevice->getBaseDevice()->getText("ACTIVE_DEVICES");
        if (!tvp)
            continue;

        //bool propertyUpdated = false;

        for (auto &it : *tvp)
        {
            QList<ISD::GenericDevice *> devs;
            if (it.isNameMatch("ACTIVE_TELESCOPE"))
            {
                devs = findDevicesByInterface(INDI::BaseDevice::TELESCOPE_INTERFACE);
            }
            else if (it.isNameMatch("ACTIVE_DOME"))
            {
                devs = findDevicesByInterface(INDI::BaseDevice::DOME_INTERFACE);
            }
            else if (it.isNameMatch("ACTIVE_GPS"))
            {
                devs = findDevicesByInterface(INDI::BaseDevice::GPS_INTERFACE);
            }
            else if (it.isNameMatch("ACTIVE_ROTATOR"))
            {
                devs = findDevicesByInterface(INDI::BaseDevice::ROTATOR_INTERFACE);
            }
            else if (it.isNameMatch("ACTIVE_FILTER"))
            {
                devs = findDevicesByInterface(INDI::BaseDevice::FILTER_INTERFACE);
                // Active filter wheel should be set to whatever the user selects in capture module
                const QString defaultFilterWheel = Options::defaultCaptureFilterWheel();
                // Does defaultFilterWheel exist in devices?
                if (defaultFilterWheel == "--")
                {
                    // If already empty, do not update it.
                    if (!QString(it.getText()).isEmpty())
                    {
                        it.setText("");
                        oneDevice->getDriverInfo()->getClientManager()->sendNewText(tvp);
                    }
                    continue;
                }
                else
                {
                    for (auto &oneDev : devs)
                    {
                        if (oneDev->getDeviceName() == defaultFilterWheel)
                        {
                            // TODO this should be profile specific
                            if (QString(it.getText()) != defaultFilterWheel)
                            {
                                it.setText(defaultFilterWheel.toLatin1().constData());
                                oneDevice->getDriverInfo()->getClientManager()->sendNewText(tvp);
                                break;
                            }
                        }
                    }
                    continue;
                }
                // If it does not exist, then continue and pick from available devs below.

            }
            // 2021.04.21 JM: There could be more than active weather device
            //                else if (it.isNameMatch("ACTIVE_WEATHER"))
            //                {
            //                    devs = findDevicesByInterface(INDI::BaseDevice::WEATHER_INTERFACE);
            //                }

            if (!devs.empty())
            {
                if (it.getText() != devs.first()->getDeviceName())
                {
                    it.setText(devs.first()->getDeviceName().toLatin1().constData());
                    oneDevice->getDriverInfo()->getClientManager()->sendNewText(tvp);
                }
            }
        }
    }
}

bool Manager::checkUniqueBinaryDriver(DriverInfo * primaryDriver, DriverInfo * secondaryDriver)
{
    if (!primaryDriver || !secondaryDriver)
        return false;

    return (primaryDriver->getExecutable() == secondaryDriver->getExecutable() &&
            primaryDriver->getAuxInfo().value("mdpd", false).toBool() == true);
}

void Manager::restartDriver(const QString &deviceName)
{
    qCInfo(KSTARS_EKOS) << "Restarting driver" << deviceName;
    if (m_LocalMode)
    {
        for (auto &oneDevice : m_GenericDevices)
        {
            if (oneDevice->getDeviceName() == deviceName)
            {
                DriverManager::Instance()->restartDriver(oneDevice->getDriverInfo());
                break;
            }
        }
    }
    else
        INDI::WebManager::restartDriver(currentProfile, deviceName);
}

void Manager::setEkosLoggingEnabled(const QString &name, bool enabled)
{
    // LOGGING, FILE, DEFAULT are exclusive, so one of them must be SET to TRUE
    if (name == "LOGGING")
    {
        Options::setDisableLogging(!enabled);
        if (!enabled)
            KSUtils::Logging::Disable();
    }
    else if (name == "FILE")
    {
        Options::setLogToFile(enabled);
        if (enabled)
            KSUtils::Logging::UseFile();
    }
    else if (name == "DEFAULT")
    {
        Options::setLogToDefault(enabled);
        if (enabled)
            KSUtils::Logging::UseDefault();
    }
    // VERBOSE should be set to TRUE if INDI or Ekos logging is selected.
    else if (name == "VERBOSE")
    {
        Options::setVerboseLogging(enabled);
        KSUtils::Logging::SyncFilterRules();
    }
    // Toggle INDI Logging
    else if (name == "INDI")
    {
        Options::setINDILogging(enabled);
        KSUtils::Logging::SyncFilterRules();
    }
    else if (name == "FITS")
    {
        Options::setFITSLogging(enabled);
        KSUtils::Logging::SyncFilterRules();
    }
    else if (name == "CAPTURE")
    {
        Options::setCaptureLogging(enabled);
        Options::setINDICCDLogging(enabled);
        Options::setINDIFilterWheelLogging(enabled);
        KSUtils::Logging::SyncFilterRules();
    }
    else if (name == "FOCUS")
    {
        Options::setFocusLogging(enabled);
        Options::setINDIFocuserLogging(enabled);
        KSUtils::Logging::SyncFilterRules();
    }
    else if (name == "GUIDE")
    {
        Options::setGuideLogging(enabled);
        Options::setINDICCDLogging(enabled);
        KSUtils::Logging::SyncFilterRules();
    }
    else if (name == "ALIGNMENT")
    {
        Options::setAlignmentLogging(enabled);
        KSUtils::Logging::SyncFilterRules();
    }
    else if (name == "MOUNT")
    {
        Options::setMountLogging(enabled);
        Options::setINDIMountLogging(enabled);
        KSUtils::Logging::SyncFilterRules();
    }
    else if (name == "SCHEDULER")
    {
        Options::setSchedulerLogging(enabled);
        KSUtils::Logging::SyncFilterRules();
    }
    else if (name == "OBSERVATORY")
    {
        Options::setObservatoryLogging(enabled);
        KSUtils::Logging::SyncFilterRules();
    }
}

void Manager::acceptPortSelection()
{
    if (m_PortSelector)
        m_PortSelector->accept();
}

void Manager::setPortSelectionComplete()
{
    if (currentProfile->portSelector)
    {
        // Turn off port selector
        currentProfile->portSelector = false;
        KStarsData::Instance()->userdb()->SaveProfile(currentProfile);
    }

    if (currentProfile->autoConnect)
        connectDevices();
}

void Manager::activateModule(const QString &name, bool popup)
{
    auto child = toolsWidget->findChild<QWidget *>(name);
    if (child)
    {
        toolsWidget->setCurrentWidget(child);
        if (popup)
        {
            raise();
            activateWindow();
            showNormal();
        }
    }
}

void Manager::createModules(ISD::GenericDevice *device)
{
    if (device->isConnected())
    {
        if (device->getDriverInterface() & INDI::BaseDevice::CCD_INTERFACE)
        {
            initCapture();
            initFocus();
            initAlign();
            initGuide();
        }
        if (device->getDriverInterface() & INDI::BaseDevice::FILTER_INTERFACE)
        {
            initCapture();
            initFocus();
            initAlign();
        }
        if (device->getDriverInterface() & INDI::BaseDevice::FOCUSER_INTERFACE)
            initFocus();
        if (device->getDriverInterface() & INDI::BaseDevice::TELESCOPE_INTERFACE)
        {
            initCapture();
            initAlign();
            initGuide();
            initMount();
        }
        if (device->getDriverInterface() & INDI::BaseDevice::DOME_INTERFACE)
        {
            initCapture();
            initAlign();
            //initObservatory();
        }
        if (device->getDriverInterface() & INDI::BaseDevice::WEATHER_INTERFACE)
        {
            initAlign();
            //initObservatory();
        }
        if (device->getDriverInterface() & INDI::BaseDevice::DUSTCAP_INTERFACE)
        {
            initCapture();
        }
        if (device->getDriverInterface() & INDI::BaseDevice::LIGHTBOX_INTERFACE)
        {
            initCapture();
        }
    }
}

void Manager::setDeviceReady()
{
    auto device = static_cast<ISD::GenericDevice*>(sender());

    if (device->isConnected())
        createModules(device);
    // Check if we need to perform autoconnect
    // After a device is declared ready. i.e. when all properties arrived.
    else if (currentProfile->autoConnect && currentProfile->portSelector == false)
    {
        if (device)
            device->Connect();
    }

    emit newDevice(device->getDeviceName(), device->getDriverInterface());
}

}
