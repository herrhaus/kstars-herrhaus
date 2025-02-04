/*
    SPDX-FileCopyrightText: 2012 Jasem Mutlaq <mutlaqja@ikarustech.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "indiconcretedevice.h"
#include "indicamerachip.h"

#include "wsmedia.h"
#include "auxiliary/imageviewer.h"
#include "fitsviewer/fitscommon.h"
#include "fitsviewer/fitsview.h"
#include "fitsviewer/fitsviewer.h"
#include "ekos/capture/placeholderpath.h"

#include <QStringList>
#include <QPointer>
#include <QtConcurrent>

#include <memory>

class FITSData;
class FITSView;
class QTimer;
class StreamWG;

/**
 * \namespace ISD
 *
 *  ISD is a collection of INDI Standard Devices. It encapsulates common types of INDI devices such as telescopes and CCDs.
 *
 */
namespace ISD
{
class CameraChip;

/**
 * @class Camera
 * Camera class controls an INDI Camera device. It can be used to issue and abort capture commands, receive and process BLOBs,
 * and return information on the capabilities of the camera.
 *
 * @author Jasem Mutlaq
 */
class Camera : public ConcreteDevice
{
        Q_OBJECT

    public:
        explicit Camera(GenericDevice *parent);
        virtual ~Camera() override;

        typedef enum { UPLOAD_CLIENT, UPLOAD_LOCAL, UPLOAD_BOTH } UploadMode;
        enum BlobType
        {
            BLOB_IMAGE,
            BLOB_FITS,
            BLOB_RAW,
            BLOB_OTHER
        } BType;
        typedef enum { TELESCOPE_PRIMARY, TELESCOPE_GUIDE, TELESCOPE_UNKNOWN } TelescopeType;
        typedef enum
        {
            ERROR_CAPTURE,              /** INDI Camera error */
            ERROR_SAVE,                 /** Saving to disk error */
            ERROR_LOAD,                 /** Loading image buffer error */
            ERROR_VIEWER                /** Loading in FITS Viewer Error */
        } ErrorType;

        void registerProperty(INDI::Property prop) override;
        void removeProperty(const QString &name) override;
        void processSwitch(ISwitchVectorProperty *svp) override;
        void processText(ITextVectorProperty *tvp) override;
        void processNumber(INumberVectorProperty *nvp) override;
        bool processBLOB(IBLOB *bp) override;

        // Does it an on-chip dedicated guide head?
        bool hasGuideHead();
        // Does it report temperature?
        bool hasCooler();
        // Can temperature be controlled?
        bool canCool()
        {
            return CanCool;
        }
        // Does it have active cooler on/off controls?
        bool hasCoolerControl();
        bool setCoolerControl(bool enable);
        bool isCoolerOn();
        // Does it have a video stream?
        bool hasVideoStream()
        {
            return HasVideoStream;
        }

        // Temperature controls
        bool getTemperature(double *value);
        bool setTemperature(double value);

        // Temperature Regulation
        bool getTemperatureRegulation(double &ramp, double &threshold);
        bool setTemperatureRegulation(double ramp, double threshold);

        // Utility functions
        void setISOMode(bool enable)
        {
            ISOMode = enable;
        }
        void setSeqPrefix(const QString &preFix)
        {
            seqPrefix = preFix;
        }
        void setPlaceholderPath(Ekos::PlaceholderPath php)
        {
            placeholderPath = php;
        }
        void setNextSequenceID(int count)
        {
            nextSequenceID = count;
        }

        // Gain controls
        bool hasGain()
        {
            return gainN != nullptr;
        }
        bool getGain(double *value);
        IPerm getGainPermission() const
        {
            return gainPerm;
        }
        bool setGain(double value);
        bool getGainMinMaxStep(double *min, double *max, double *step);

        // offset controls
        bool hasOffset()
        {
            return offsetN != nullptr;
        }
        bool getOffset(double *value);
        IPerm getOffsetPermission() const
        {
            return offsetPerm;
        }
        bool setOffset(double value);
        bool getOffsetMinMaxStep(double *min, double *max, double *step);

        // Rapid Guide
        bool configureRapidGuide(CameraChip *targetChip, bool autoLoop, bool sendImage = false, bool showMarker = false);
        bool setRapidGuide(CameraChip *targetChip, bool enable);

        // Upload Settings
        void updateUploadSettings(const QString &remoteDir);
        UploadMode getUploadMode();
        bool setUploadMode(UploadMode mode);

        // Encoding Format
        const QString &getEncodingFormat() const
        {
            return m_EncodingFormat;
        }
        bool setEncodingFormat(const QString &value);
        const QStringList &getEncodingFormats() const
        {
            return m_EncodingFormats;
        }

        // Capture Format
        const QStringList &getCaptureFormats() const
        {
            return m_CaptureFormats;
        }
        QString getCaptureFormat() const;
        bool setCaptureFormat(const QString &format);

        // BLOB control
        bool isBLOBEnabled();
        bool setBLOBEnabled(bool enable, const QString &prop = QString());

        // Video Stream
        bool setVideoStreamEnabled(bool enable);
        bool resetStreamingFrame();
        bool setStreamingFrame(int x, int y, int w, int h);
        bool isStreamingEnabled();
        bool setStreamExposure(double duration);
        bool getStreamExposure(double *duration);
        bool setStreamLimits(uint16_t maxBufferSize, uint16_t maxPreviewFPS);

        // Video Recording
        bool setSERNameDirectory(const QString &filename, const QString &directory);
        bool getSERNameDirectory(QString &filename, QString &directory);
        bool startRecording();
        bool startDurationRecording(double duration);
        bool startFramesRecording(uint32_t frames);
        bool stopRecording();

        // Telescope type
        TelescopeType getTelescopeType()
        {
            return telescopeType;
        }
        bool setTelescopeType(TelescopeType type);

        // Update FITS Header
        bool setFITSHeader(const QMap<QString, QString> &values);

        CameraChip *getChip(CameraChip::ChipType cType);

        bool setFastExposureEnabled(bool enable);
        bool isFastExposureEnabled() const
        {
            return m_FastExposureEnabled;
        }
        bool setFastCount(uint32_t count);

        const QMap<QString, double> &getExposurePresets() const
        {
            return m_ExposurePresets;
        }
        const QPair<double, double> getExposurePresetsMinMax() const
        {
            return m_ExposurePresetsMinMax;
        }

    public slots:
        //void FITSViewerDestroyed();
        void StreamWindowHidden();
        // Blob manager
        void setBLOBManager(const char *device, INDI::Property prop);

    protected slots:
        void setWSBLOB(const QByteArray &message, const QString &extension);

    signals:
        void newTemperatureValue(double value);
        void newExposureValue(ISD::CameraChip *chip, double value, IPState state);
        void newGuideStarData(ISD::CameraChip *chip, double dx, double dy, double fit);
        void newBLOBManager(INDI::Property prop);
        void newRemoteFile(QString);
        void videoStreamToggled(bool enabled);
        void videoRecordToggled(bool enabled);
        void newFPS(double instantFPS, double averageFPS);
        void newVideoFrame(const QSharedPointer<QImage> &frame);
        void coolerToggled(bool enabled);
        void error(ErrorType type);
        void newImage(const QSharedPointer<FITSData> &data);

    private:
        void processStream(IBLOB *bp);
        bool generateFilename(bool batch_mode, const QString &extension, QString *filename);
        // Saves an image to disk on a separate thread.
        bool writeImageFile(const QString &filename, IBLOB *bp, bool is_fits);
        bool WriteImageFileInternal(const QString &filename, char *buffer, const size_t size);
        // Creates or finds the FITSViewer.
        QPointer<FITSViewer> getFITSViewer();
        void handleImage(CameraChip *targetChip, const QString &filename, IBLOB *bp, QSharedPointer<FITSData> data);

        bool ISOMode { true };
        bool HasGuideHead { false };
        bool HasCooler { false };
        bool CanCool { false };
        bool HasCoolerControl { false };
        bool HasVideoStream { false };
        bool m_FastExposureEnabled { false };
        QString seqPrefix;
        Ekos::PlaceholderPath placeholderPath;

        int nextSequenceID { 0 };
        std::unique_ptr<StreamWG> streamWindow;
        int streamW { 0 };
        int streamH { 0 };
        int normalTabID { -1 };
        int calibrationTabID { -1 };
        int focusTabID { -1 };
        int guideTabID { -1 };
        int alignTabID { -1 };

        //char BLOBFilename[MAXINDIFILENAME + 1];
        IBLOB *primaryCCDBLOB { nullptr };

        std::unique_ptr<CameraChip> primaryChip;
        std::unique_ptr<CameraChip> guideChip;
        std::unique_ptr<WSMedia> m_Media;
        QString m_EncodingFormat {"FITS"};
        QStringList m_EncodingFormats;
        QStringList m_CaptureFormats;
        int m_CaptureFormatIndex;
        TelescopeType telescopeType { TELESCOPE_UNKNOWN };

        // Gain, since it is spread among different vector properties, let's try to find the property itself.
        INumber *gainN { nullptr };
        IPerm gainPerm { IP_RO };

        INumber *offsetN { nullptr };
        IPerm offsetPerm { IP_RO };

        QPointer<FITSViewer> m_FITSViewerWindow;
        QPointer<ImageViewer> m_ImageViewerWindow;

        QDateTime m_LastNotificationTS;

        // Typically for DSLRs
        QMap<QString, double> m_ExposurePresets;
        QPair<double, double> m_ExposurePresetsMinMax;

        // Used when writing the image fits file to disk in a separate thread.
        char *fileWriteBuffer { nullptr };
        int fileWriteBufferSize { 0 };
        QString fileWriteFilename;
        QFuture<void> fileWriteThread;
};
}
