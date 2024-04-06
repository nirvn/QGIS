/***************************************************************************
                         qgssinglecolorrendererwidget.h
                         ---------------------------------
    begin                : March 2012
    copyright            : (C) 2012 by Marco Hugentobler
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

#ifndef QGSSINGLECOLORRENDERERWIDGET_H
#define QGSSINGLECOLORRENDERERWIDGET_H

#include "qgsrasterrendererwidget.h"
#include "qgis_sip.h"
#include "qgis_gui.h"
#include "ui_qgssinglecolorrendererwidgetbase.h"

/**
 * \ingroup gui
 * \class QgsSingleBandGrayRendererWidget
 */
class GUI_EXPORT QgsSingleColorRendererWidget: public QgsRasterRendererWidget, private Ui::QgsSingleColorRendererWidgetBase
{
    Q_OBJECT
  public:
    QgsSingleColorRendererWidget( QgsRasterLayer *layer, const QgsRectangle &extent = QgsRectangle() );

    static QgsRasterRendererWidget *create( QgsRasterLayer *layer, const QgsRectangle &extent ) SIP_FACTORY { return new QgsSingleColorRendererWidget( layer, extent ); }

    QgsRasterRenderer *renderer() SIP_FACTORY override;

    /**
     * Sets the widget state from the specified renderer.
     */
    void setFromRenderer( const QgsRasterRenderer *r );

  private slots:
    void colorChanged( const QColor &color );

};

#endif // QGSSINGLECOLORRENDERERWIDGET_H
