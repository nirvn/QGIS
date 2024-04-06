/***************************************************************************
                         qgssinglecolorrenderer.h
                         ---------------------------
    begin                : December 2011
    copyright            : (C) 2011 by Marco Hugentobler
    email                : marco at sourcepole dot ch
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef QGSSINGLECOLORRENDERER_H
#define QGSSINGLECOLORRENDERER_H

#include "qgis_core.h"
#include "qgis_sip.h"
#include "qgsrasterrenderer.h"

#include <QImage>

#include <memory>

class QDomElement;

/**
 * \ingroup core
  * \brief Raster renderer pipe for single band gray.
  */
class CORE_EXPORT QgsSingleColorRenderer: public QgsRasterRenderer
{
  public:

    QgsSingleColorRenderer( QgsRasterInterface *input, QColor color );

    //! QgsSingleColorRenderer cannot be copied. Use clone() instead.
    QgsSingleColorRenderer( const QgsSingleColorRenderer & ) = delete;
    //! QgsSingleColorRenderer cannot be copied. Use clone() instead.
    const QgsSingleColorRenderer &operator=( const QgsSingleColorRenderer & ) = delete;

    QgsSingleColorRenderer *clone() const override SIP_FACTORY;
    Qgis::RasterRendererFlags flags() const override;

    static QgsRasterRenderer *create( const QDomElement &elem, QgsRasterInterface *input ) SIP_FACTORY;

    QgsRasterBlock *block( int bandNo, const QgsRectangle &extent, int width, int height, QgsRasterBlockFeedback *feedback = nullptr ) override SIP_FACTORY;

    QColor color() const;
    void setColor( QColor &color );

    void writeXml( QDomDocument &doc, QDomElement &parentElem ) const override;

    QList<int> usesBands() const override;

  private:
#ifdef SIP_RUN
    QgsSingleColorRenderer( const QgsSingleColorRenderer & );
    const QgsSingleColorRenderer &operator=( const QgsSingleColorRenderer & );
#endif

    QColor mColor;
};

#endif // QGSSINGLECOLORRENDERER_H
