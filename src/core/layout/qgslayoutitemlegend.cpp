/***************************************************************************
                         qgslayoutitemlegend.cpp
                         -----------------------
    begin                : October 2017
    copyright            : (C) 2017 by Nyall Dawson
    email                : nyall dot dawson at gmail dot com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#include <limits>

#include "qgslayoutitemlegend.h"
#include "qgslayoutitemregistry.h"
#include "qgslayoutitemmap.h"
#include "qgslayout.h"
#include "qgslayoutmodel.h"
#include "qgslayertree.h"
#include "qgslayertreemodel.h"
#include "qgslegendrenderer.h"
#include "qgslegendstyle.h"
#include "qgslogger.h"
#include "qgsmapsettings.h"
#include "qgsproject.h"
#include "qgssymbollayerutils.h"
#include "qgslayertreeutils.h"
#include <QDomDocument>
#include <QDomElement>
#include <QPainter>

QgsLayoutItemLegend::QgsLayoutItemLegend( QgsLayout *layout )
  : QgsLayoutItem( layout )
  , mLegendModel( new QgsLegendModel( layout->project()->layerTreeRoot() ) )
{
#if 0 //TODO
  connect( &layout->atlasComposition(), &QgsAtlasComposition::renderEnded, this, &QgsLayoutItemLegend::onAtlasEnded );
  connect( &layout->atlasComposition(), &QgsAtlasComposition::featureChanged, this, &QgsLayoutItemLegend::onAtlasFeature );
#endif

  // Connect to the main layertreeroot.
  // It serves in "auto update mode" as a medium between the main app legend and this one
  connect( mLayout->project()->layerTreeRoot(), &QgsLayerTreeNode::customPropertyChanged, this, &QgsLayoutItemLegend::nodeCustomPropertyChanged );
}

QgsLayoutItemLegend *QgsLayoutItemLegend::create( QgsLayout *layout )
{
  return new QgsLayoutItemLegend( layout );
}

int QgsLayoutItemLegend::type() const
{
  return QgsLayoutItemRegistry::LayoutLegend;
}

QString QgsLayoutItemLegend::stringType() const
{
  return QStringLiteral( "ItemLegend" );
}


void QgsLayoutItemLegend::paint( QPainter *painter, const QStyleOptionGraphicsItem *itemStyle, QWidget *pWidget )
{
  if ( !painter )
    return;

  if ( mFilterAskedForUpdate )
  {
    mFilterAskedForUpdate = false;
    doUpdateFilterByMap();
  }

  int dpi = painter->device()->logicalDpiX();
  double dotsPerMM = dpi / 25.4;

  if ( mLayout )
  {
    mSettings.setUseAdvancedEffects( mLayout->context().flags() & QgsLayoutContext::FlagUseAdvancedEffects );
    mSettings.setDpi( dpi );
  }
  if ( mMap && mLayout )
  {
    mSettings.setMmPerMapUnit( mLayout->convertFromLayoutUnits( mMap->mapUnitsToLayoutUnits(), QgsUnitTypes::LayoutMillimeters ).length() );

    // use a temporary QgsMapSettings to find out real map scale
    QSizeF mapSizePixels = QSizeF( mMap->rect().width() * dotsPerMM, mMap->rect().height() * dotsPerMM );
    QgsRectangle mapExtent = mMap->extent();

    QgsMapSettings ms = mMap->mapSettings( mapExtent, mapSizePixels, dpi );
    mSettings.setMapScale( ms.scale() );
  }
  mInitialMapScaleCalculated = true;

  QgsLegendRenderer legendRenderer( mLegendModel.get(), mSettings );
  legendRenderer.setLegendSize( mForceResize && mSizeToContents ? QSize() : rect().size() );

  //adjust box if width or height is too small
  if ( mSizeToContents )
  {
    QSizeF size = legendRenderer.minimumSize();
    if ( mForceResize )
    {
      mForceResize = false;
      //set new rect, respecting position mode and data defined size/position
      QRectF targetRect = QRectF( pos().x(), pos().y(), size.width(), size.height() );
      attemptSetSceneRect( targetRect );
    }
    else if ( size.height() > rect().height() || size.width() > rect().width() )
    {
      //need to resize box
      QRectF targetRect = QRectF( pos().x(), pos().y(), rect().width(), rect().height() );
      if ( size.height() > targetRect.height() )
        targetRect.setHeight( size.height() );
      if ( size.width() > rect().width() )
        targetRect.setWidth( size.width() );

      //set new rect, respecting position mode and data defined size/position
      attemptSetSceneRect( targetRect );
    }
  }
  QgsLayoutItem::paint( painter, itemStyle, pWidget );
}

void QgsLayoutItemLegend::draw( QgsRenderContext &context, const QStyleOptionGraphicsItem * )
{
  QPainter *painter = context.painter();
  painter->save();

  // painter is scaled to dots, so scale back to layout units
  painter->scale( context.scaleFactor(), context.scaleFactor() );

  painter->setPen( QPen( QColor( 0, 0, 0 ) ) );

  if ( !mSizeToContents )
  {
    // set a clip region to crop out parts of legend which don't fit
    QRectF thisPaintRect = QRectF( 0, 0, rect().width(), rect().height() );
    painter->setClipRect( thisPaintRect );
  }

  QgsLegendRenderer legendRenderer( mLegendModel.get(), mSettings );
  legendRenderer.setLegendSize( mForceResize && mSizeToContents ? QSize() : rect().size() );

  legendRenderer.drawLegend( painter );

  painter->restore();
}

void QgsLayoutItemLegend::adjustBoxSize()
{
  if ( !mSizeToContents )
    return;

  if ( !mInitialMapScaleCalculated )
  {
    // this is messy - but until we have painted the item we have no knowledge of the current DPI
    // and so cannot correctly calculate the map scale. This results in incorrect size calculations
    // for marker symbols with size in map units, causing the legends to initially expand to huge
    // sizes if we attempt to calculate the box size first.
    return;
  }

  QgsLegendRenderer legendRenderer( mLegendModel.get(), mSettings );
  QSizeF size = legendRenderer.minimumSize();
  QgsDebugMsg( QString( "width = %1 height = %2" ).arg( size.width() ).arg( size.height() ) );
  if ( size.isValid() )
  {
    QRectF targetRect = QRectF( pos().x(), pos().y(), size.width(), size.height() );
    //set new rect, respecting position mode and data defined size/position
    attemptSetSceneRect( targetRect );
  }
}

void QgsLayoutItemLegend::setResizeToContents( bool enabled )
{
  mSizeToContents = enabled;
}

bool QgsLayoutItemLegend::resizeToContents() const
{
  return mSizeToContents;
}

void QgsLayoutItemLegend::setCustomLayerTree( QgsLayerTree *rootGroup )
{
  mLegendModel->setRootGroup( rootGroup ? rootGroup : ( mLayout ? mLayout->project()->layerTreeRoot() : nullptr ) );

  mCustomLayerTree.reset( rootGroup );
}


void QgsLayoutItemLegend::setAutoUpdateModel( bool autoUpdate )
{
  if ( autoUpdate == autoUpdateModel() )
    return;

  setCustomLayerTree( autoUpdate ? nullptr : mLayout->project()->layerTreeRoot()->clone() );
  adjustBoxSize();
  updateFilterByMap( false );
}

void QgsLayoutItemLegend::nodeCustomPropertyChanged( QgsLayerTreeNode *, const QString & )
{
  if ( autoUpdateModel() )
  {
    // in "auto update" mode, some parameters on the main app legend may have been changed (expression filtering)
    // we must then call updateItem to reflect the changes
    updateFilterByMap( false );
  }
}

bool QgsLayoutItemLegend::autoUpdateModel() const
{
  return !mCustomLayerTree;
}

void QgsLayoutItemLegend::setLegendFilterByMapEnabled( bool enabled )
{
  mLegendFilterByMap = enabled;
  updateFilterByMap( false );
}

void QgsLayoutItemLegend::setTitle( const QString &t )
{
  mTitle = t;
  mSettings.setTitle( t );

  if ( mLayout && id().isEmpty() )
  {
    //notify the model that the display name has changed
    mLayout->itemsModel()->updateItemDisplayName( this );
  }
}
QString QgsLayoutItemLegend::title() const
{
  return mTitle;
}

Qt::AlignmentFlag QgsLayoutItemLegend::titleAlignment() const
{
  return mSettings.titleAlignment();
}

void QgsLayoutItemLegend::setTitleAlignment( Qt::AlignmentFlag alignment )
{
  mSettings.setTitleAlignment( alignment );
}

QgsLegendStyle &QgsLayoutItemLegend::rstyle( QgsLegendStyle::Style s )
{
  return mSettings.rstyle( s );
}

QgsLegendStyle QgsLayoutItemLegend::style( QgsLegendStyle::Style s ) const
{
  return mSettings.style( s );
}

void QgsLayoutItemLegend::setStyle( QgsLegendStyle::Style s, const QgsLegendStyle &style )
{
  mSettings.setStyle( s, style );
}

QFont QgsLayoutItemLegend::styleFont( QgsLegendStyle::Style s ) const
{
  return mSettings.style( s ).font();
}

void QgsLayoutItemLegend::setStyleFont( QgsLegendStyle::Style s, const QFont &f )
{
  rstyle( s ).setFont( f );
}

void QgsLayoutItemLegend::setStyleMargin( QgsLegendStyle::Style s, double margin )
{
  rstyle( s ).setMargin( margin );
}

void QgsLayoutItemLegend::setStyleMargin( QgsLegendStyle::Style s, QgsLegendStyle::Side side, double margin )
{
  rstyle( s ).setMargin( side, margin );
}

double QgsLayoutItemLegend::lineSpacing() const
{
  return mSettings.lineSpacing();
}

void QgsLayoutItemLegend::setLineSpacing( double spacing )
{
  mSettings.setLineSpacing( spacing );
}

double QgsLayoutItemLegend::boxSpace() const
{
  return mSettings.boxSpace();
}

void QgsLayoutItemLegend::setBoxSpace( double s )
{
  mSettings.setBoxSpace( s );
}

double QgsLayoutItemLegend::columnSpace() const
{
  return mSettings.columnSpace();
}

void QgsLayoutItemLegend::setColumnSpace( double s )
{
  mSettings.setColumnSpace( s );
}

QColor QgsLayoutItemLegend::fontColor() const
{
  return mSettings.fontColor();
}

void QgsLayoutItemLegend::setFontColor( const QColor &c )
{
  mSettings.setFontColor( c );
}

double QgsLayoutItemLegend::symbolWidth() const
{
  return mSettings.symbolSize().width();
}

void QgsLayoutItemLegend::setSymbolWidth( double w )
{
  mSettings.setSymbolSize( QSizeF( w, mSettings.symbolSize().height() ) );
}

double QgsLayoutItemLegend::symbolHeight() const
{
  return mSettings.symbolSize().height();
}

void QgsLayoutItemLegend::setSymbolHeight( double h )
{
  mSettings.setSymbolSize( QSizeF( mSettings.symbolSize().width(), h ) );
}

double QgsLayoutItemLegend::wmsLegendWidth() const
{
  return mSettings.wmsLegendSize().width();
}

void QgsLayoutItemLegend::setWmsLegendWidth( double w )
{
  mSettings.setWmsLegendSize( QSizeF( w, mSettings.wmsLegendSize().height() ) );
}

double QgsLayoutItemLegend::wmsLegendHeight() const
{
  return mSettings.wmsLegendSize().height();
}
void QgsLayoutItemLegend::setWmsLegendHeight( double h )
{
  mSettings.setWmsLegendSize( QSizeF( mSettings.wmsLegendSize().width(), h ) );
}

void QgsLayoutItemLegend::setWrapString( const QString &t )
{
  mSettings.setWrapChar( t );
}

QString QgsLayoutItemLegend::wrapString() const
{
  return mSettings.wrapChar();
}

int QgsLayoutItemLegend::columnCount() const
{
  return mColumnCount;
}

void QgsLayoutItemLegend::setColumnCount( int c )
{
  mColumnCount = c;
  mSettings.setColumnCount( c );
}

bool QgsLayoutItemLegend::splitLayer() const
{
  return mSettings.splitLayer();
}

void QgsLayoutItemLegend::setSplitLayer( bool s )
{
  mSettings.setSplitLayer( s );
}

bool QgsLayoutItemLegend::equalColumnWidth() const
{
  return mSettings.equalColumnWidth();
}

void QgsLayoutItemLegend::setEqualColumnWidth( bool s )
{
  mSettings.setEqualColumnWidth( s );
}

bool QgsLayoutItemLegend::drawRasterStroke() const
{
  return mSettings.drawRasterStroke();
}

void QgsLayoutItemLegend::setDrawRasterStroke( bool enabled )
{
  mSettings.setDrawRasterStroke( enabled );
}

QColor QgsLayoutItemLegend::rasterStrokeColor() const
{
  return mSettings.rasterStrokeColor();
}

void QgsLayoutItemLegend::setRasterStrokeColor( const QColor &color )
{
  mSettings.setRasterStrokeColor( color );
}

double QgsLayoutItemLegend::rasterStrokeWidth() const
{
  return mSettings.rasterStrokeWidth();
}

void QgsLayoutItemLegend::setRasterStrokeWidth( double width )
{
  mSettings.setRasterStrokeWidth( width );
}

void QgsLayoutItemLegend::synchronizeWithModel()
{
  adjustBoxSize();
  updateFilterByMap( false );
}

void QgsLayoutItemLegend::updateLegend()
{
  adjustBoxSize();
  updateFilterByMap( false );
}

#if 0//TODO
bool QgsLayoutItemLegend::writeXml( QDomElement &elem, QDomDocument &doc ) const
{
  if ( elem.isNull() )
  {
    return false;
  }

  QDomElement composerLegendElem = doc.createElement( QStringLiteral( "ComposerLegend" ) );
  elem.appendChild( composerLegendElem );

  //write general properties
  composerLegendElem.setAttribute( QStringLiteral( "title" ), mTitle );
  composerLegendElem.setAttribute( QStringLiteral( "titleAlignment" ), QString::number( static_cast< int >( mSettings.titleAlignment() ) ) );
  composerLegendElem.setAttribute( QStringLiteral( "columnCount" ), QString::number( mColumnCount ) );
  composerLegendElem.setAttribute( QStringLiteral( "splitLayer" ), QString::number( mSettings.splitLayer() ) );
  composerLegendElem.setAttribute( QStringLiteral( "equalColumnWidth" ), QString::number( mSettings.equalColumnWidth() ) );

  composerLegendElem.setAttribute( QStringLiteral( "boxSpace" ), QString::number( mSettings.boxSpace() ) );
  composerLegendElem.setAttribute( QStringLiteral( "columnSpace" ), QString::number( mSettings.columnSpace() ) );

  composerLegendElem.setAttribute( QStringLiteral( "symbolWidth" ), QString::number( mSettings.symbolSize().width() ) );
  composerLegendElem.setAttribute( QStringLiteral( "symbolHeight" ), QString::number( mSettings.symbolSize().height() ) );
  composerLegendElem.setAttribute( QStringLiteral( "lineSpacing" ), QString::number( mSettings.lineSpacing() ) );

  composerLegendElem.setAttribute( QStringLiteral( "rasterBorder" ), mSettings.drawRasterStroke() );
  composerLegendElem.setAttribute( QStringLiteral( "rasterBorderColor" ), QgsSymbolLayerUtils::encodeColor( mSettings.rasterStrokeColor() ) );
  composerLegendElem.setAttribute( QStringLiteral( "rasterBorderWidth" ), QString::number( mSettings.rasterStrokeWidth() ) );

  composerLegendElem.setAttribute( QStringLiteral( "wmsLegendWidth" ), QString::number( mSettings.wmsLegendSize().width() ) );
  composerLegendElem.setAttribute( QStringLiteral( "wmsLegendHeight" ), QString::number( mSettings.wmsLegendSize().height() ) );
  composerLegendElem.setAttribute( QStringLiteral( "wrapChar" ), mSettings.wrapChar() );
  composerLegendElem.setAttribute( QStringLiteral( "fontColor" ), mSettings.fontColor().name() );

  composerLegendElem.setAttribute( QStringLiteral( "resizeToContents" ), mSizeToContents );

  if ( mMap )
  {
    composerLegendElem.setAttribute( QStringLiteral( "map" ), mMap->id() );
  }

  QDomElement composerLegendStyles = doc.createElement( QStringLiteral( "styles" ) );
  composerLegendElem.appendChild( composerLegendStyles );

  style( QgsLegendStyle::Title ).writeXml( QStringLiteral( "title" ), composerLegendStyles, doc );
  style( QgsLegendStyle::Group ).writeXml( QStringLiteral( "group" ), composerLegendStyles, doc );
  style( QgsLegendStyle::Subgroup ).writeXml( QStringLiteral( "subgroup" ), composerLegendStyles, doc );
  style( QgsLegendStyle::Symbol ).writeXml( QStringLiteral( "symbol" ), composerLegendStyles, doc );
  style( QgsLegendStyle::SymbolLabel ).writeXml( QStringLiteral( "symbolLabel" ), composerLegendStyles, doc );

  if ( mCustomLayerTree )
  {
    // if not using auto-update - store the custom layer tree
    mCustomLayerTree->writeXml( composerLegendElem );
  }

  if ( mLegendFilterByMap )
  {
    composerLegendElem.setAttribute( QStringLiteral( "legendFilterByMap" ), QStringLiteral( "1" ) );
  }
  composerLegendElem.setAttribute( QStringLiteral( "legendFilterByAtlas" ), mFilterOutAtlas ? QStringLiteral( "1" ) : QStringLiteral( "0" ) );

  return _writeXml( composerLegendElem, doc );
}

bool QgsLayoutItemLegend::readXml( const QDomElement &itemElem, const QDomDocument &doc )
{
  if ( itemElem.isNull() )
  {
    return false;
  }

  //read general properties
  mTitle = itemElem.attribute( QStringLiteral( "title" ) );
  mSettings.setTitle( mTitle );
  if ( !itemElem.attribute( QStringLiteral( "titleAlignment" ) ).isEmpty() )
  {
    mSettings.setTitleAlignment( static_cast< Qt::AlignmentFlag >( itemElem.attribute( QStringLiteral( "titleAlignment" ) ).toInt() ) );
  }
  int colCount = itemElem.attribute( QStringLiteral( "columnCount" ), QStringLiteral( "1" ) ).toInt();
  if ( colCount < 1 ) colCount = 1;
  mColumnCount = colCount;
  mSettings.setColumnCount( mColumnCount );
  mSettings.setSplitLayer( itemElem.attribute( QStringLiteral( "splitLayer" ), QStringLiteral( "0" ) ).toInt() == 1 );
  mSettings.setEqualColumnWidth( itemElem.attribute( QStringLiteral( "equalColumnWidth" ), QStringLiteral( "0" ) ).toInt() == 1 );

  QDomNodeList stylesNodeList = itemElem.elementsByTagName( QStringLiteral( "styles" ) );
  if ( !stylesNodeList.isEmpty() )
  {
    QDomNode stylesNode = stylesNodeList.at( 0 );
    for ( int i = 0; i < stylesNode.childNodes().size(); i++ )
    {
      QDomElement styleElem = stylesNode.childNodes().at( i ).toElement();
      QgsLegendStyle style;
      style.readXml( styleElem, doc );
      QString name = styleElem.attribute( QStringLiteral( "name" ) );
      QgsLegendStyle::Style s;
      if ( name == QLatin1String( "title" ) ) s = QgsLegendStyle::Title;
      else if ( name == QLatin1String( "group" ) ) s = QgsLegendStyle::Group;
      else if ( name == QLatin1String( "subgroup" ) ) s = QgsLegendStyle::Subgroup;
      else if ( name == QLatin1String( "symbol" ) ) s = QgsLegendStyle::Symbol;
      else if ( name == QLatin1String( "symbolLabel" ) ) s = QgsLegendStyle::SymbolLabel;
      else continue;
      setStyle( s, style );
    }
  }

  //font color
  QColor fontClr;
  fontClr.setNamedColor( itemElem.attribute( QStringLiteral( "fontColor" ), QStringLiteral( "#000000" ) ) );
  mSettings.setFontColor( fontClr );

  //spaces
  mSettings.setBoxSpace( itemElem.attribute( QStringLiteral( "boxSpace" ), QStringLiteral( "2.0" ) ).toDouble() );
  mSettings.setColumnSpace( itemElem.attribute( QStringLiteral( "columnSpace" ), QStringLiteral( "2.0" ) ).toDouble() );

  mSettings.setSymbolSize( QSizeF( itemElem.attribute( QStringLiteral( "symbolWidth" ), QStringLiteral( "7.0" ) ).toDouble(), itemElem.attribute( QStringLiteral( "symbolHeight" ), QStringLiteral( "14.0" ) ).toDouble() ) );
  mSettings.setWmsLegendSize( QSizeF( itemElem.attribute( QStringLiteral( "wmsLegendWidth" ), QStringLiteral( "50" ) ).toDouble(), itemElem.attribute( QStringLiteral( "wmsLegendHeight" ), QStringLiteral( "25" ) ).toDouble() ) );
  mSettings.setLineSpacing( itemElem.attribute( QStringLiteral( "lineSpacing" ), QStringLiteral( "1.0" ) ).toDouble() );

  mSettings.setDrawRasterStroke( itemElem.attribute( QStringLiteral( "rasterBorder" ), QStringLiteral( "1" ) ) != QLatin1String( "0" ) );
  mSettings.setRasterStrokeColor( QgsSymbolLayerUtils::decodeColor( itemElem.attribute( QStringLiteral( "rasterBorderColor" ), QStringLiteral( "0,0,0" ) ) ) );
  mSettings.setRasterStrokeWidth( itemElem.attribute( QStringLiteral( "rasterBorderWidth" ), QStringLiteral( "0" ) ).toDouble() );

  mSettings.setWrapChar( itemElem.attribute( QStringLiteral( "wrapChar" ) ) );

  mSizeToContents = itemElem.attribute( QStringLiteral( "resizeToContents" ), QStringLiteral( "1" ) ) != QLatin1String( "0" );

  //composer map
  mLegendFilterByMap = itemElem.attribute( QStringLiteral( "legendFilterByMap" ), QStringLiteral( "0" ) ).toInt();
  if ( !itemElem.attribute( QStringLiteral( "map" ) ).isEmpty() )
  {
    setMap( mComposition->getComposerMapById( itemElem.attribute( QStringLiteral( "map" ) ).toInt() ) );
  }
  mFilterOutAtlas = itemElem.attribute( QStringLiteral( "legendFilterByAtlas" ), QStringLiteral( "0" ) ).toInt();

  // QGIS >= 2.6
  QDomElement layerTreeElem = itemElem.firstChildElement( QStringLiteral( "layer-tree" ) );
  if ( layerTreeElem.isNull() )
    layerTreeElem = itemElem.firstChildElement( QStringLiteral( "layer-tree-group" ) );

  if ( !layerTreeElem.isNull() )
  {
    std::unique_ptr< QgsLayerTree > tree( QgsLayerTree::readXml( layerTreeElem ) );
    if ( mComposition )
      tree->resolveReferences( mComposition->project(), true );
    setCustomLayerTree( tree.release() );
  }
  else
    setCustomLayerTree( nullptr );

  //restore general composer item properties
  QDomNodeList composerItemList = itemElem.elementsByTagName( QStringLiteral( "ComposerItem" ) );
  if ( !composerItemList.isEmpty() )
  {
    QDomElement composerItemElem = composerItemList.at( 0 ).toElement();
    _readXml( composerItemElem, doc );
  }

  // < 2.0 projects backward compatibility >>>>>
  //title font
  QString titleFontString = itemElem.attribute( QStringLiteral( "titleFont" ) );
  if ( !titleFontString.isEmpty() )
  {
    rstyle( QgsLegendStyle::Title ).rfont().fromString( titleFontString );
  }
  //group font
  QString groupFontString = itemElem.attribute( QStringLiteral( "groupFont" ) );
  if ( !groupFontString.isEmpty() )
  {
    rstyle( QgsLegendStyle::Group ).rfont().fromString( groupFontString );
  }

  //layer font
  QString layerFontString = itemElem.attribute( QStringLiteral( "layerFont" ) );
  if ( !layerFontString.isEmpty() )
  {
    rstyle( QgsLegendStyle::Subgroup ).rfont().fromString( layerFontString );
  }
  //item font
  QString itemFontString = itemElem.attribute( QStringLiteral( "itemFont" ) );
  if ( !itemFontString.isEmpty() )
  {
    rstyle( QgsLegendStyle::SymbolLabel ).rfont().fromString( itemFontString );
  }

  if ( !itemElem.attribute( QStringLiteral( "groupSpace" ) ).isEmpty() )
  {
    rstyle( QgsLegendStyle::Group ).setMargin( QgsLegendStyle::Top, itemElem.attribute( QStringLiteral( "groupSpace" ), QStringLiteral( "3.0" ) ).toDouble() );
  }
  if ( !itemElem.attribute( QStringLiteral( "layerSpace" ) ).isEmpty() )
  {
    rstyle( QgsLegendStyle::Subgroup ).setMargin( QgsLegendStyle::Top, itemElem.attribute( QStringLiteral( "layerSpace" ), QStringLiteral( "3.0" ) ).toDouble() );
  }
  if ( !itemElem.attribute( QStringLiteral( "symbolSpace" ) ).isEmpty() )
  {
    rstyle( QgsLegendStyle::Symbol ).setMargin( QgsLegendStyle::Top, itemElem.attribute( QStringLiteral( "symbolSpace" ), QStringLiteral( "2.0" ) ).toDouble() );
    rstyle( QgsLegendStyle::SymbolLabel ).setMargin( QgsLegendStyle::Top, itemElem.attribute( QStringLiteral( "symbolSpace" ), QStringLiteral( "2.0" ) ).toDouble() );
  }
  // <<<<<<< < 2.0 projects backward compatibility

  emit itemChanged();
  return true;
}
#endif

QString QgsLayoutItemLegend::displayName() const
{
  if ( !id().isEmpty() )
  {
    return id();
  }

  //if no id, default to portion of title text
  QString text = mSettings.title();
  if ( text.isEmpty() )
  {
    return tr( "<legend>" );
  }
  if ( text.length() > 25 )
  {
    return QString( tr( "%1..." ) ).arg( text.left( 25 ) );
  }
  else
  {
    return text;
  }
}

void QgsLayoutItemLegend::setMap( QgsLayoutItemMap *map )
{
  if ( mMap )
  {
    disconnect( mMap, &QObject::destroyed, this, &QgsLayoutItemLegend::invalidateCurrentMap );
    disconnect( mMap, &QgsLayoutObject::changed, this, &QgsLayoutItemLegend::updateFilterByMapAndRedraw );
    disconnect( mMap, &QgsLayoutItemMap::extentChanged, this, &QgsLayoutItemLegend::updateFilterByMapAndRedraw );
    disconnect( mMap, &QgsLayoutItemMap::layerStyleOverridesChanged, this, &QgsLayoutItemLegend::mapLayerStyleOverridesChanged );
  }

  mMap = map;

  if ( map )
  {
    connect( map, &QObject::destroyed, this, &QgsLayoutItemLegend::invalidateCurrentMap );
    connect( map, &QgsLayoutObject::changed, this, &QgsLayoutItemLegend::updateFilterByMapAndRedraw );
    connect( map, &QgsLayoutItemMap::extentChanged, this, &QgsLayoutItemLegend::updateFilterByMapAndRedraw );
    connect( map, &QgsLayoutItemMap::layerStyleOverridesChanged, this, &QgsLayoutItemLegend::mapLayerStyleOverridesChanged );
  }

  updateFilterByMap();
}

void QgsLayoutItemLegend::invalidateCurrentMap()
{
  setMap( nullptr );
}

void QgsLayoutItemLegend::refreshDataDefinedProperty( const QgsLayoutObject::DataDefinedProperty property )
{
  QgsExpressionContext context = createExpressionContext();

  bool forceUpdate = false;
  //updates data defined properties and redraws item to match
  if ( property == QgsLayoutObject::LegendTitle || property == QgsLayoutObject::AllProperties )
  {
    bool ok = false;
    QString t = mDataDefinedProperties.valueAsString( QgsLayoutObject::LegendTitle, context, mTitle, &ok );
    if ( ok )
    {
      mSettings.setTitle( t );
      forceUpdate = true;
    }
  }
  if ( property == QgsLayoutObject::LegendColumnCount || property == QgsLayoutObject::AllProperties )
  {
    bool ok = false;
    int cols = mDataDefinedProperties.valueAsInt( QgsLayoutObject::LegendColumnCount, context, mColumnCount, &ok );
    if ( ok && cols >= 0 )
    {
      mSettings.setColumnCount( cols );
      forceUpdate = true;
    }
  }
  if ( forceUpdate )
  {
    adjustBoxSize();
    update();
  }

  QgsLayoutItem::refreshDataDefinedProperty( property );
}


void QgsLayoutItemLegend::updateFilterByMapAndRedraw()
{
  updateFilterByMap( true );
}

void QgsLayoutItemLegend::mapLayerStyleOverridesChanged()
{
  if ( !mMap )
    return;

  // map's style has been changed, so make sure to update the legend here
  if ( mLegendFilterByMap )
  {
    // legend is being filtered by map, so we need to re run the hit test too
    // as the style overrides may also have affected the visible symbols
    updateFilterByMap( false );
  }
  else
  {
    mLegendModel->setLayerStyleOverrides( mMap->layerStyleOverrides() );

    Q_FOREACH ( QgsLayerTreeLayer *nodeLayer, mLegendModel->rootGroup()->findLayers() )
      mLegendModel->refreshLayerLegend( nodeLayer );
  }

  adjustBoxSize();
  updateFilterByMap( false );
}

void QgsLayoutItemLegend::updateFilterByMap( bool redraw )
{
  // ask for update
  // the actual update will take place before the redraw.
  // This is to avoid multiple calls to the filter
  mFilterAskedForUpdate = true;

  if ( redraw )
    update();
}

void QgsLayoutItemLegend::doUpdateFilterByMap()
{
  if ( mMap )
    mLegendModel->setLayerStyleOverrides( mMap->layerStyleOverrides() );
  else
    mLegendModel->setLayerStyleOverrides( QMap<QString, QString>() );


  bool filterByExpression = QgsLayerTreeUtils::hasLegendFilterExpression( *( mCustomLayerTree ? mCustomLayerTree.get() : mLayout->project()->layerTreeRoot() ) );

  if ( mMap && ( mLegendFilterByMap || filterByExpression || mInAtlas ) )
  {
    int dpi = mLayout->context().dpi();

    QgsRectangle requestRectangle = mMap->requestedExtent();

    QSizeF size( requestRectangle.width(), requestRectangle.height() );
    size *= mLayout->convertFromLayoutUnits( mMap->mapUnitsToLayoutUnits(), QgsUnitTypes::LayoutMillimeters ).length() * dpi / 25.4;

    QgsMapSettings ms = mMap->mapSettings( requestRectangle, size, dpi );

    QgsGeometry filterPolygon;
    if ( mInAtlas )
    {
#if 0 //TODO
      filterPolygon = composition()->atlasComposition().currentGeometry( mMap->crs() );
#endif
    }
    mLegendModel->setLegendFilter( &ms, /* useExtent */ mInAtlas || mLegendFilterByMap, filterPolygon, /* useExpressions */ true );
  }
  else
    mLegendModel->setLegendFilterByMap( nullptr );

  mForceResize = true;
}

void QgsLayoutItemLegend::setLegendFilterOutAtlas( bool doFilter )
{
  mFilterOutAtlas = doFilter;
}

bool QgsLayoutItemLegend::legendFilterOutAtlas() const
{
  return mFilterOutAtlas;
}

void QgsLayoutItemLegend::onAtlasFeature( QgsFeature *feat )
{
  if ( !feat )
    return;
  mInAtlas = mFilterOutAtlas;
  updateFilterByMap();
}

void QgsLayoutItemLegend::onAtlasEnded()
{
  mInAtlas = false;
  updateFilterByMap();
}

// -------------------------------------------------------------------------
#include "qgslayertreemodellegendnode.h"
#include "qgsvectorlayer.h"

QgsLegendModel::QgsLegendModel( QgsLayerTree *rootNode, QObject *parent )
  : QgsLayerTreeModel( rootNode, parent )
{
  setFlag( QgsLayerTreeModel::AllowLegendChangeState, false );
  setFlag( QgsLayerTreeModel::AllowNodeReorder, true );
}

QVariant QgsLegendModel::data( const QModelIndex &index, int role ) const
{
  // handle custom layer node labels
  if ( QgsLayerTreeNode *node = index2node( index ) )
  {
    if ( QgsLayerTree::isLayer( node ) && ( role == Qt::DisplayRole || role == Qt::EditRole ) && !node->customProperty( QStringLiteral( "legend/title-label" ) ).isNull() )
    {
      QgsLayerTreeLayer *nodeLayer = QgsLayerTree::toLayer( node );
      QString name = node->customProperty( QStringLiteral( "legend/title-label" ) ).toString();
      if ( nodeLayer->customProperty( QStringLiteral( "showFeatureCount" ), 0 ).toInt() && role == Qt::DisplayRole )
      {
        QgsVectorLayer *vlayer = qobject_cast<QgsVectorLayer *>( nodeLayer->layer() );
        if ( vlayer && vlayer->featureCount() >= 0 )
          name += QStringLiteral( " [%1]" ).arg( vlayer->featureCount() );
      }
      return name;
    }
  }

  return QgsLayerTreeModel::data( index, role );
}

Qt::ItemFlags QgsLegendModel::flags( const QModelIndex &index ) const
{
  // make the legend nodes selectable even if they are not by default
  if ( index2legendNode( index ) )
    return QgsLayerTreeModel::flags( index ) | Qt::ItemIsSelectable;

  return QgsLayerTreeModel::flags( index );
}
