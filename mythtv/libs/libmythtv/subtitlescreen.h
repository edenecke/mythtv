// -*- Mode: c++ -*-

#ifndef SUBTITLESCREEN_H
#define SUBTITLESCREEN_H

#include <QStringList>
#include <QRegExp>
#include <QVector>
#include <QFont>
#include <QHash>
#include <QRect>
#include <QSize>

#ifdef USING_LIBASS
extern "C" {
#include <ass/ass.h>
}
#endif

#include "mythscreentype.h"
#include "subtitlereader.h"
#include "mythplayer.h"

class SubtitleScreen : public MythScreenType
{
    friend class FormattedTextSubtitle;

    static bool Initialise708Fonts(int fontStretch = QFont::Unstretched);

  public:
    SubtitleScreen(MythPlayer *player, const char * name, int fontStretch);
    virtual ~SubtitleScreen();

    void EnableSubtitles(int type, bool forced_only = false);
    void DisableForcedSubtitles(void);
    int  EnabledSubtitleType(void) { return m_subtitleType; }

    void ClearAllSubtitles(void);
    void ClearNonDisplayedSubtitles(void);
    void ClearDisplayedSubtitles(void);
    void ExpireSubtitles(void);
    void DisplayDVDButton(AVSubtitle* dvdButton, QRect &buttonPos);

    QSize CalcTextSize(const QString &text,
                       const CC708CharacterAttribute &format) const;

    void RegisterExpiration(MythUIType *shape, long long endTime)
    {
        m_expireTimes.insert(shape, endTime);
    }

    bool GetUseBackground(void) { return m_useBackground; }

    // MythScreenType methods
    virtual bool Create(void);
    virtual void Pulse(void);

  private:
    void OptimiseDisplayedArea(void);
    void DisplayAVSubtitles(void);
    void DisplayTextSubtitles(void);
    void DisplayRawTextSubtitles(void);
    void DrawTextSubtitles(QStringList &wrappedsubs, uint64_t start,
                           uint64_t duration);
    void DisplayCC608Subtitles(void);
    void DisplayCC708Subtitles(void);
    void AddScaledImage(QImage &img, QRect &pos);
    void Clear708Cache(int num);
    MythFontProperties* Get708Font(CC708CharacterAttribute attr) const;
    void SetFontSizes(int small, int medium, int large);

    MythPlayer        *m_player;
    SubtitleReader    *m_subreader;
    CC608Reader       *m_608reader;
    CC708Reader       *m_708reader;
    QRect              m_safeArea;
    bool               m_useBackground;
    QRegExp            m_removeHTML;
    int                m_subtitleType;
    QHash<MythUIType*, long long> m_expireTimes;
    int                m_708fontSizes[4];
    int                m_textFontZoom; // valid for 708 & text subs
    bool               m_refreshArea;
    QHash<int,QList<MythUIType*> > m_708imageCache;
    int                m_fontStretch;

#ifdef USING_LIBASS
    bool InitialiseAssLibrary(void);
    void LoadAssFonts(void);
    void CleanupAssLibrary(void);
    void InitialiseAssTrack(int tracknum);
    void CleanupAssTrack(void);
    void AddAssEvent(char *event);
    void ResizeAssRenderer(void);
    void RenderAssTrack(uint64_t timecode);

    ASS_Library       *m_assLibrary;
    ASS_Renderer      *m_assRenderer;
    int                m_assTrackNum;
    ASS_Track         *m_assTrack;
    uint               m_assFontCount;
#endif // USING_LIBASS
};

class FormattedTextChunk
{
  public:
    FormattedTextChunk(const QString &t, CC708CharacterAttribute formatting,
                       SubtitleScreen *p)
        : text(t), format(formatting), parent(p)
    {
    }
    FormattedTextChunk(void) {}

    QSize CalcSize(void) const
    {
        return parent->CalcTextSize(text, format);
    }
    bool Split(FormattedTextChunk &newChunk);
    QString ToLogString(void) const;

    QString text;
    CC708CharacterAttribute format;
    SubtitleScreen *parent; // where fonts and sizes are kept
};

class FormattedTextLine
{
  public:
    FormattedTextLine(int x = -1, int y = -1, int o_x = -1, int o_y = -1)
        : x_indent(x), y_indent(y), orig_x(o_x), orig_y(o_y) {}

    QSize CalcSize(void) const
    {
        int height = 0, width = 0;
        QList<FormattedTextChunk>::const_iterator it;
        for (it = chunks.constBegin(); it != chunks.constEnd(); ++it)
        {
            QSize tmp = (*it).CalcSize();
            height = max(height, tmp.height());
            width += tmp.width();
        }
        return QSize(width, height);
    }

    QList<FormattedTextChunk> chunks;
    int x_indent; // -1 means TBD i.e. centered
    int y_indent; // -1 means TBD i.e. relative to bottom
    int orig_x, orig_y; // original, unscaled coordinates
};

class FormattedTextSubtitle
{
  public:
    FormattedTextSubtitle(const QRect &safearea, bool useBackground,
                          SubtitleScreen *p)
        : m_safeArea(safearea), m_useBackground(useBackground),
          parent(p) {}
    FormattedTextSubtitle(void)
        : m_safeArea(QRect()), m_useBackground(false), parent(0) {}
    void InitFromCC608(vector<CC608Text*> &buffers);
    void InitFromCC708(const CC708Window &win, int num,
                       const vector<CC708String*> &list,
                       float aspect = 1.77777f,
                       int textFontZoom = 100);
    void InitFromSRT(QStringList &subs, int textFontZoom);
    void WrapLongLines(void);
    void Layout(void);
    bool Draw(QList<MythUIType*> *imageCache = 0,
              uint64_t start = 0, uint64_t duration = 0) const;
    QStringList ToSRT(void) const;
    QRect m_bounds;

  private:
    QVector<FormattedTextLine> m_lines;
    const QRect m_safeArea;
    const bool m_useBackground;
    SubtitleScreen *parent; // where fonts and sizes are kept
    int m_xAnchorPoint; // 0=left, 1=center, 2=right
    int m_yAnchorPoint; // 0=top,  1=center, 2=bottom
    int m_xAnchor; // pixels from left
    int m_yAnchor; // pixels from top
};

#endif // SUBTITLESCREEN_H
