/*
 * This file is part of Krita
 *
 * Copyright (c) 2006 Cyrille Berger <cberger@cberger.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef WAVEFILTER_H
#define WAVEFILTER_H

#include <QObject>
#include <QVariant>
#include "filter/kis_filter.h"

class KisConfigWidget;

class KritaWaveFilter : public QObject
{
    Q_OBJECT
public:
    KritaWaveFilter(QObject *parent, const QVariantList &);
    ~KritaWaveFilter() override;
};

class KisFilterWave : public KisFilter
{
public:

    KisFilterWave();

public:

    void processImpl(KisPaintDeviceSP device,
                     const QRect& applyRect,
                     const KisFilterConfigurationSP config,
                     KoUpdater* progressUpdater) const override;
    static inline KoID id() {
        return KoID("wave", i18n("Wave"));
    }

    KisFilterConfigurationSP defaultConfiguration() const override;
public:
    QRect neededRect(const QRect& rect, const KisFilterConfigurationSP config = 0, int lod = 0) const override;

    KisConfigWidget * createConfigurationWidget(QWidget* parent, const KisPaintDeviceSP dev, bool useForMasks) const override;
};

#endif
