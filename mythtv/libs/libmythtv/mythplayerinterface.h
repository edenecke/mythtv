﻿#ifndef MYTHPLAYERINTERFACE_H
#define MYTHPLAYERINTERFACE_H

// MythTV
#include "DetectLetterbox.h"
#include "jitterometer.h"
#include "mythplayer.h"

class MythPlayerInterface : public MythPlayer,
                            public MythPlayerVisualiser,
                            public MythVideoScanTracker,
                            public MythPlayerAudioInterface
{
    Q_OBJECT

  public slots:
    void EmbedPlayback(bool Embed, const QRect& Rect = {});

  public:
    MythPlayerInterface(MythMainWindow* MainWindow, TV* Tv, PlayerContext* Context, PlayerFlags Flags);

    virtual void EventLoop();
    bool InitVideo() override;
    void ReinitVideo(bool ForceUpdate) override;
    void VideoStart() override;
    virtual bool VideoLoop();
    void ChangeSpeed() override;
    void ReleaseNextVideoFrame(VideoFrame* Frame, int64_t Timecode, bool Wrap = true) override;
    void SetVideoParams(int Width, int Height, double FrameRate, float Aspect,
                        bool ForceUpdate, int ReferenceFrames,
                        FrameScanType Scan = kScan_Ignore,
                        const QString& CodecName = QString()) override;
    void WindowResized(const QSize& Size);
    void GetPlaybackData(InfoMap& Map);
    void GetCodecDescription(InfoMap& Map);
    void ToggleAdjustFill(AdjustFillMode Mode = kAdjustFill_Toggle);
    void EnableFrameRateMonitor(bool Enable = false);
    bool CanSupportDoubleRate();

    // FIXME - should be private
    DetectLetterbox m_detectLetterBox { this };

    // N.B. Editor - keep ringfenced and move into subclass
    bool EnableEdit();
    void DisableEdit(int HowToSave);
    bool HandleProgramEditorActions(QStringList& Actions);
    uint64_t GetNearestMark(uint64_t Frame, bool Right);
    bool IsTemporaryMark(uint64_t Frame);
    bool HasTemporaryMark();
    bool IsCutListSaved();
    bool DeleteMapHasUndo();
    bool DeleteMapHasRedo();
    QString DeleteMapGetUndoMessage();
    QString DeleteMapGetRedoMessage();
    // End editor stuff

  protected:
    void InitFrameInterval() override;
    void DisplayPauseFrame() override;
    virtual void DisplayNormalFrame(bool CheckPrebuffer = true);

    void RefreshPauseFrame();
    void RenderVideoFrame(VideoFrame* Frame, FrameScanType Scan, bool Prepare, int64_t Wait);
    void DoDisplayVideoFrame(VideoFrame* Frame, int64_t Due);

    MythMainWindow* m_mainWindow   { nullptr };
    TV*             m_tv           { nullptr };
    MythDisplay*    m_display      { nullptr };
    Jitterometer    m_outputJmeter { "Player" };

    // N.B. Editor - keep ringfenced and move into subclass
    float           m_speedBeforeEdit  { 1.0   };
    bool            m_pausedBeforeEdit { false };
    QElapsedTimer   m_editUpdateTimer;
};

#endif
