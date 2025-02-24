//////////////////////////////////////////////////////////////////////////////
// Program Name: envInfo.h
// Created     : Dec. 15, 2015
//
// Copyright (c) 2015 Bill Meek, from: 2010 David Blain <dblain@mythtv.org>
//
// Licensed under the GPL v2 or later, see COPYING for details
//
//////////////////////////////////////////////////////////////////////////////

#ifndef V2ENVINFO_H_
#define V2ENVINFO_H_

#include "libmythbase/http/mythhttpservice.h"

class V2EnvInfo : public QObject
{
    Q_OBJECT

    Q_CLASSINFO( "version"    , "1.0" );

    SERVICE_PROPERTY2( QString, LANG        )
    SERVICE_PROPERTY2( QString, LCALL       )
    SERVICE_PROPERTY2( QString, LCCTYPE     )
    SERVICE_PROPERTY2( QString, HOME        )
    SERVICE_PROPERTY2( QString, MYTHCONFDIR );

    public:

        Q_INVOKABLE V2EnvInfo(QObject *parent = nullptr)
            : QObject       ( parent ),
              m_LANG        ( ""     ),
              m_LCALL       ( ""     ),
              m_LCCTYPE     ( ""     ),
              m_HOME        ( ""     ),
              m_MYTHCONFDIR ( ""     )
        {
        }

        void Copy( const V2EnvInfo *src )
        {
            m_LANG        = src->m_LANG;
            m_LCALL       = src->m_LCALL;
            m_LCCTYPE     = src->m_LCCTYPE;
            m_HOME        = src->m_HOME;
            m_MYTHCONFDIR = src->m_MYTHCONFDIR;
        }

    private:
        Q_DISABLE_COPY(V2EnvInfo);
};

using EnvInfoPtr = V2EnvInfo*;

Q_DECLARE_METATYPE(V2EnvInfo*)

#endif
