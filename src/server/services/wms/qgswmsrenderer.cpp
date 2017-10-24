/***************************************************************************
                              qgswmsrenderer.cpp
                              -------------------
  begin                : May 14, 2006
  copyright            : (C) 2006 by Marco Hugentobler
                         (C) 2017 by David Marteau
  email                : marco dot hugentobler at karto dot baug dot ethz dot ch
                         david dot marteau at 3liz dot com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/


#include "qgswmsutils.h"
#include "qgswmsrenderer.h"
#include "qgsfilterrestorer.h"
#include "qgscapabilitiescache.h"
#include "qgsexception.h"
#include "qgsfields.h"
#include "qgsfieldformatter.h"
#include "qgsfieldformatterregistry.h"
#include "qgsfeatureiterator.h"
#include "qgsgeometry.h"
#include "qgsmapserviceexception.h"
#include "qgslayertree.h"
#include "qgslayertreemodel.h"
#include "qgslayertreemodellegendnode.h"
#include "qgslegendrenderer.h"
#include "qgsmaplayer.h"
#include "qgsmaplayerlegend.h"
#include "qgsmaptopixel.h"
#include "qgsproject.h"
#include "qgsrasteridentifyresult.h"
#include "qgsrasterlayer.h"
#include "qgsrasterrenderer.h"
#include "qgsscalecalculator.h"
#include "qgscoordinatereferencesystem.h"
#include "qgsvectordataprovider.h"
#include "qgsvectorlayer.h"
#include "qgslogger.h"
#include "qgsmessagelog.h"
#include "qgssymbol.h"
#include "qgsrenderer.h"
#include "qgspaintenginehack.h"
#include "qgsogcutils.h"
#include "qgsfeature.h"
#include "qgsaccesscontrol.h"
#include "qgsfeaturerequest.h"
#include "qgsmaprendererjobproxy.h"
#include "qgswmsserviceexception.h"
#include "qgsserverprojectutils.h"
#include "qgsgui.h"
#include "qgsmaplayerstylemanager.h"
#include "qgswkbtypes.h"
#include "qgsannotationmanager.h"
#include "qgsannotation.h"
#include "qgsvectorlayerlabeling.h"
#include "qgsvectorlayerfeaturecounter.h"
#include "qgspallabeling.h"
#include "qgslayerrestorer.h"
#include "qgsdxfexport.h"
#include "qgssymbollayerutils.h"
#include "qgslayoutitemlegend.h"

#include <QImage>
#include <QPainter>
#include <QStringList>
#include <QTemporaryFile>
#include <QTextStream>
#include <QDir>

//for printing
#include "qgslayoutmanager.h"
#include "qgscomposition.h"
#include "qgscomposerarrow.h"
#include "qgscomposerlabel.h"
#include "qgscomposerlegend.h"
#include "qgscomposermap.h"
#include "qgscomposermapgrid.h"
#include "qgscomposerframe.h"
#include "qgscomposerhtml.h"
#include "qgscomposerpicture.h"
#include "qgscomposerscalebar.h"
#include "qgscomposershape.h"
#include <QBuffer>
#include <QPrinter>
#include <QSvgGenerator>
#include <QUrl>
#include <QPaintEngine>

namespace QgsWms
{

  namespace
  {

    QgsLayerTreeModelLegendNode *_findLegendNodeForRule( QgsLayerTreeModel *legendModel, const QString &rule )
    {
      Q_FOREACH ( QgsLayerTreeLayer *nodeLayer, legendModel->rootGroup()->findLayers() )
      {
        Q_FOREACH ( QgsLayerTreeModelLegendNode *legendNode, legendModel->layerLegendNodes( nodeLayer ) )
        {
          if ( legendNode->data( Qt::DisplayRole ).toString() == rule )
            return legendNode;
        }
      }
      return nullptr;
    }

  } // namespace


  QgsRenderer::QgsRenderer( QgsServerInterface *serverIface,
                            const QgsProject *project,
                            const QgsServerRequest::Parameters &parameters )
    : mParameters( parameters )
    , mAccessControl( serverIface->accessControls() )
    , mSettings( *serverIface->serverSettings() )
    , mProject( project )
  {
    mWmsParameters.load( parameters );
    mWmsParameters.dump();

    initRestrictedLayers();
    initNicknameLayers();
  }

  QgsRenderer::~QgsRenderer()
  {
    removeTemporaryLayers();
  }


  QImage *QgsRenderer::getLegendGraphics()
  {
    // check parameters
    if ( mWmsParameters.allLayersNickname().isEmpty() )
      throw QgsBadRequestException( QStringLiteral( "LayerNotSpecified" ),
                                    QStringLiteral( "LAYER is mandatory for GetLegendGraphic operation" ) );

    if ( mWmsParameters.format() == QgsWmsParameters::Format::NONE )
      throw QgsBadRequestException( QStringLiteral( "FormatNotSpecified" ),
                                    QStringLiteral( "FORMAT is mandatory for GetLegendGraphic operation" ) );

    double scaleDenominator = -1;
    if ( ! mWmsParameters.scale().isEmpty() )
      scaleDenominator = mWmsParameters.scaleAsDouble();

    QgsLegendSettings legendSettings = mWmsParameters.legendSettings();

    // get layers
    std::unique_ptr<QgsLayerRestorer> restorer;
    restorer.reset( new QgsLayerRestorer( mNicknameLayers.values() ) );

    QList<QgsMapLayer *> layers;
    QList<QgsWmsParametersLayer> params = mWmsParameters.layersParameters();

    QString sld = mWmsParameters.sld();
    if ( !sld.isEmpty() )
      layers = sldStylizedLayers( sld );
    else
      layers = stylizedLayers( params );

    removeUnwantedLayers( layers, scaleDenominator );
    std::reverse( layers.begin(), layers.end() );

    // check permissions
    Q_FOREACH ( QgsMapLayer *ml, layers )
      checkLayerReadPermissions( ml );

    // build layer tree model for legend
    QgsLayerTree rootGroup;
    std::unique_ptr<QgsLayerTreeModel> legendModel;
    legendModel.reset( buildLegendTreeModel( layers, scaleDenominator, rootGroup ) );

    // rendering step
    qreal dpmm = dotsPerMm();
    std::unique_ptr<QImage> image;
    std::unique_ptr<QPainter> painter;

    if ( !mWmsParameters.rule().isEmpty() )
    {
      QString rule = mWmsParameters.rule();
      int width = mWmsParameters.widthAsInt();
      int height = mWmsParameters.heightAsInt();

      image.reset( createImage( width, height, false ) );
      painter.reset( new QPainter( image.get() ) );
      painter->setRenderHint( QPainter::Antialiasing, true );
      painter->scale( dpmm, dpmm );

      QgsLayerTreeModelLegendNode *legendNode = _findLegendNodeForRule( legendModel.get(), rule );
      if ( legendNode )
      {
        QgsLayerTreeModelLegendNode::ItemContext ctx;
        ctx.painter = painter.get();
        ctx.labelXOffset = 0;
        ctx.point = QPointF();
        double itemHeight = height / dpmm;
        legendNode->drawSymbol( legendSettings, &ctx, itemHeight );
        painter->end();
      }
    }
    else
    {
      QgsLegendRenderer legendRendererNew( legendModel.get(), legendSettings );

      QSizeF minSize = legendRendererNew.minimumSize();
      QSize s( minSize.width() * dpmm, minSize.height() * dpmm );

      image.reset( createImage( s.width(), s.height(), false ) );
      painter.reset( new QPainter( image.get() ) );
      painter->setRenderHint( QPainter::Antialiasing, true );
      painter->scale( dpmm, dpmm );

      legendRendererNew.drawLegend( painter.get() );
      painter->end();
    }

    rootGroup.clear();
    return image.release();
  }

  void QgsRenderer::runHitTest( const QgsMapSettings &mapSettings, HitTest &hitTest ) const
  {
    QgsRenderContext context = QgsRenderContext::fromMapSettings( mapSettings );

    Q_FOREACH ( const QString &id, mapSettings.layerIds() )
    {
      QgsVectorLayer *vl = qobject_cast<QgsVectorLayer *>( mProject->mapLayer( id ) );
      if ( !vl || !vl->renderer() )
        continue;

      if ( vl->hasScaleBasedVisibility() && vl->isInScaleRange( mapSettings.scale() ) )
      {
        hitTest[vl] = SymbolSet(); // no symbols -> will not be shown
        continue;
      }

      QgsCoordinateTransform tr = mapSettings.layerTransform( vl );
      context.setCoordinateTransform( tr );
      context.setExtent( tr.transformBoundingBox( mapSettings.extent(), QgsCoordinateTransform::ReverseTransform ) );

      SymbolSet &usedSymbols = hitTest[vl];
      runHitTestLayer( vl, usedSymbols, context );
    }
  }

  void QgsRenderer::runHitTestLayer( QgsVectorLayer *vl, SymbolSet &usedSymbols, QgsRenderContext &context ) const
  {
    QgsFeatureRenderer *r = vl->renderer();
    bool moreSymbolsPerFeature = r->capabilities() & QgsFeatureRenderer::MoreSymbolsPerFeature;
    r->startRender( context, vl->pendingFields() );
    QgsFeature f;
    QgsFeatureRequest request( context.extent() );
    request.setFlags( QgsFeatureRequest::ExactIntersect );
    QgsFeatureIterator fi = vl->getFeatures( request );
    while ( fi.nextFeature( f ) )
    {
      context.expressionContext().setFeature( f );
      if ( moreSymbolsPerFeature )
      {
        Q_FOREACH ( QgsSymbol *s, r->originalSymbolsForFeature( f, context ) )
          usedSymbols.insert( QgsSymbolLayerUtils::symbolProperties( s ) );
      }
      else
        usedSymbols.insert( QgsSymbolLayerUtils::symbolProperties( r->originalSymbolForFeature( f, context ) ) );
    }
    r->stopRender( context );
  }


  QByteArray QgsRenderer::getPrint( const QString &formatString )
  {
    //GetPrint request needs a template parameter
    QString templateName = mWmsParameters.composerTemplate();
    if ( templateName.isEmpty() )
    {
      throw QgsBadRequestException( QStringLiteral( "ParameterMissing" ),
                                    QStringLiteral( "The TEMPLATE parameter is required for the GetPrint request" ) );
    }

    // get layers parameters
    QList<QgsMapLayer *> layers;
    QList<QgsWmsParametersLayer> params = mWmsParameters.layersParameters();

    // create the output image
    std::unique_ptr<QImage> image( createImage() );

    // configure map settings (background, DPI, ...)
    QgsMapSettings mapSettings;
    configureMapSettings( image.get(), mapSettings );

    // init layer restorer before doing anything
    std::unique_ptr<QgsLayerRestorer> restorer;
    restorer.reset( new QgsLayerRestorer( mNicknameLayers.values() ) );

    // init stylized layers according to LAYERS/STYLES or SLD
    QString sld = mWmsParameters.sld();
    if ( !sld.isEmpty() )
    {
      layers = sldStylizedLayers( sld );
    }
    else
    {
      layers = stylizedLayers( params );
    }

    // remove unwanted layers (restricted layers, ...)
    removeUnwantedLayers( layers );

    // configure each layer with opacity, selection filter, ...
    bool updateMapExtent = mWmsParameters.bbox().isEmpty() ? true : false;
    Q_FOREACH ( QgsMapLayer *layer, layers )
    {
      Q_FOREACH ( QgsWmsParametersLayer param, params )
      {
        if ( param.mNickname == layerNickname( *layer ) )
        {
          checkLayerReadPermissions( layer );

          setLayerOpacity( layer, param.mOpacity );

          setLayerFilter( layer, param.mFilter );

          setLayerAccessControlFilter( layer );

          setLayerSelection( layer, param.mSelection );

          if ( updateMapExtent )
            updateExtent( layer, mapSettings );

          break;
        }
      }
    }

    // add highlight layers above others
    layers = layers << highlightLayers( mWmsParameters.highlightLayersParameters() );

    // add layers to map settings (revert order for the rendering)
    std::reverse( layers.begin(), layers.end() );
    mapSettings.setLayers( layers );

    const QgsLayoutManager *lManager = mProject->layoutManager();
    QDomDocument cDocument;
    if ( !lManager->saveAsTemplate( templateName, cDocument ) )
    {
      throw QgsBadRequestException( QStringLiteral( "InvalidTemplate" ),
                                    QStringLiteral( "Template '%1' is not known" ).arg( templateName ) );
    }

    QgsComposition *c = new QgsComposition( const_cast<QgsProject *>( mProject ) );
    if ( !c->loadFromTemplate( cDocument, nullptr, false, true ) )
    {
      throw QgsBadRequestException( QStringLiteral( "InvalidTemplate" ),
                                    QStringLiteral( "Template '%1' is not loaded correctly" ).arg( templateName ) );
    }
    configureComposition( c, mapSettings );

    c->setPlotStyle( QgsComposition::Print );

    QByteArray ba;

    //SVG export without a running X-Server is a problem. See e.g. http://developer.qt.nokia.com/forums/viewthread/2038
    if ( formatString.compare( QLatin1String( "svg" ), Qt::CaseInsensitive ) == 0 )
    {
      c->setPlotStyle( QgsComposition::Print );

      QSvgGenerator generator;
      QBuffer svgBuffer( &ba );
      generator.setOutputDevice( &svgBuffer );
      int width = ( int )( c->paperWidth() * c->printResolution() / 25.4 ); //width in pixel
      int height = ( int )( c->paperHeight() * c->printResolution() / 25.4 ); //height in pixel
      generator.setSize( QSize( width, height ) );
      generator.setResolution( c->printResolution() ); //because the rendering is done in mm, convert the dpi

      QPainter p( &generator );
      if ( c->printAsRaster() ) //embed one raster into the svg
      {
        QImage img = c->printPageAsRaster( 0 );
        p.drawImage( QRect( 0, 0, width, height ), img, QRectF( 0, 0, img.width(), img.height() ) );
      }
      else
      {
        c->renderPage( &p, 0 );
      }
      p.end();
    }
    else if ( formatString.compare( QLatin1String( "png" ), Qt::CaseInsensitive ) == 0 || formatString.compare( QLatin1String( "jpg" ), Qt::CaseInsensitive ) == 0 )
    {
      QImage image = c->printPageAsRaster( 0 ); //can only return the first page if pixmap is requested

      QBuffer buffer( &ba );
      buffer.open( QIODevice::WriteOnly );
      image.save( &buffer, formatString.toLocal8Bit().data(), -1 );
    }
    else if ( formatString.compare( QLatin1String( "pdf" ), Qt::CaseInsensitive ) == 0 )
    {
      QTemporaryFile tempFile;
      if ( !tempFile.open() )
      {
        delete c;
        return nullptr;
      }

      c->exportAsPDF( tempFile.fileName() );
      ba = tempFile.readAll();
    }
    else //unknown format
    {
      throw QgsBadRequestException( QStringLiteral( "InvalidFormat" ),
                                    QStringLiteral( "Output format '%1' is not supported in the GetPrint request" ).arg( formatString ) );
    }

    delete c;
    return ba;
  }

  bool QgsRenderer::configureComposition( QgsComposition *c, const QgsMapSettings &mapSettings )
  {
    if ( !mWmsParameters.dpi().isEmpty() )
      c->setPrintResolution( mWmsParameters.dpiAsInt() );

    // Composer items
    QList<QgsComposerItem * > itemList;
    c->composerItems( itemList );
    // Composer legends have to be updated after the other items
    QList<QgsComposerLegend *> composerLegends;

    // Update composer items
    QList<QgsComposerItem *>::iterator itemIt = itemList.begin();
    for ( ; itemIt != itemList.end(); ++itemIt )
    {
      // firstly the composer maps
      if ( ( *itemIt )->type() == QgsComposerItem::ComposerMap )
      {
        QgsComposerMap *map = qobject_cast< QgsComposerMap *>( *itemIt );
        if ( !map )
          continue;

        QgsWmsParametersComposerMap cMapParams = mWmsParameters.composerMapParameters( map->id() );

        //map extent is mandatory
        if ( !cMapParams.mHasExtent )
        {
          //remove map from composition if not referenced by the request
          c->removeItem( map );
          delete map;
          continue;
        }

        // Change CRS of map set to "project CRS" to match requested CRS
        // (if map has a valid preset crs then we keep this crs and don't use the
        // requested crs for this map item)
        if ( mapSettings.destinationCrs().isValid() && !map->presetCrs().isValid() )
          map->setCrs( mapSettings.destinationCrs() );

        QgsRectangle r( cMapParams.mExtent );
        if ( mWmsParameters.versionAsNumber() >= QgsProjectVersion( 1, 3, 0 ) && mapSettings.destinationCrs().hasAxisInverted() )
        {
          r.invert();
        }
        map->setNewExtent( r );

        // scale
        if ( cMapParams.mScale > 0 )
        {
          map->setNewScale( cMapParams.mScale );
        }

        // rotation
        if ( cMapParams.mRotation )
        {
          map->setMapRotation( cMapParams.mRotation );
        }

        if ( !map->keepLayerSet() )
        {
          if ( cMapParams.mLayers.isEmpty() )
          {
            map->setLayers( mapSettings.layers() );
          }
          else
          {
            QList<QgsMapLayer *> layerSet;
            QList<QgsWmsParametersLayer> layers = cMapParams.mLayers;
            Q_FOREACH ( QgsWmsParametersLayer layer, layers )
            {
              QString nickname = layer.mNickname;
              QString style = layer.mStyle;
              if ( mNicknameLayers.contains( nickname ) && !mRestrictedLayers.contains( nickname ) )
              {
                if ( !style.isEmpty() )
                {
                  bool rc = mNicknameLayers[nickname]->styleManager()->setCurrentStyle( style );
                  if ( ! rc )
                  {
                    throw QgsMapServiceException( QStringLiteral( "StyleNotDefined" ), QStringLiteral( "Style \"%1\" does not exist for layer \"%2\"" ).arg( style, nickname ) );
                  }
                }
                layerSet << mNicknameLayers[nickname];
              }
              else
              {
                throw QgsBadRequestException( QStringLiteral( "LayerNotDefined" ),
                                              QStringLiteral( "Layer \"%1\" does not exist" ).arg( nickname ) );
              }
            }
            layerSet << highlightLayers( cMapParams.mHighlightLayers );
            std::reverse( layerSet.begin(), layerSet.end() );
            map->setLayers( layerSet );
          }
          map->setKeepLayerSet( true );
        }

        //grid space x / y
        if ( cMapParams.mGridX > 0 && cMapParams.mGridY > 0 )
        {
          map->grid()->setIntervalX( cMapParams.mGridX );
          map->grid()->setIntervalY( cMapParams.mGridY );
        }
        continue;
      }
      // secondly the composer legends
      else if ( ( *itemIt )->type() == QgsComposerItem::ComposerLegend )
      {
        QgsComposerLegend *legend = qobject_cast< QgsComposerLegend *>( *itemIt );
        if ( !legend )
          continue;

        composerLegends.push_back( legend );
        continue;
      }
      // thirdly the composer labels
      else if ( ( *itemIt )->type() == QgsComposerItem::ComposerLabel )
      {
        QgsComposerLabel *label = qobject_cast< QgsComposerLabel *>( *itemIt );
        if ( !label )
          continue;

        QString labelId = label->id().toUpper();
        if ( !mParameters.contains( labelId ) )
          continue;

        QString labelParam = mParameters[ labelId ];
        if ( labelParam.isEmpty() )
        {
          //remove exported labels referenced in the request
          //but with empty string
          c->removeItem( label );
          delete label;
          continue;
        }

        label->setText( labelParam );
        continue;
      }
      // forthly the composer HTMLs
      // an html item will be a composer frame and if it is we can try to get
      // its multiframe parent and then try to cast that to a composer html
      else if ( ( *itemIt )->type() == QgsComposerItem::ComposerFrame )
      {
        QgsComposerFrame *frame = qobject_cast< QgsComposerFrame *>( *itemIt );
        if ( !frame )
          continue;
        const QgsComposerMultiFrame *multiFrame = frame->multiFrame();
        const QgsComposerHtml *composerHtml = qobject_cast<const QgsComposerHtml *>( multiFrame );
        if ( !composerHtml )
          continue;

        QgsComposerHtml *html = const_cast<QgsComposerHtml *>( composerHtml );
        QgsComposerFrame *htmlFrame = html->frame( 0 );
        QString htmlId = htmlFrame->id().toUpper();
        if ( !mParameters.contains( htmlId ) )
        {
          html->update();
          continue;
        }

        QString url = mParameters[ htmlId ];
        //remove exported Htmls referenced in the request
        //but with empty string
        if ( url.isEmpty() )
        {
          c->removeMultiFrame( html );
          delete composerHtml;
          continue;
        }

        QUrl newUrl( url );
        html->setUrl( newUrl );
        html->update();
        continue;
      }
    }

    //update legend
    // if it has an auto-update model
    Q_FOREACH ( QgsComposerLegend *currentLegend, composerLegends )
    {
      if ( !currentLegend )
      {
        continue;
      }

      if ( currentLegend->autoUpdateModel() )
      {
        // the legend has an auto-update model
        // we will update it with map's layers
        const QgsComposerMap *map = currentLegend->composerMap();
        if ( !map )
        {
          continue;
        }

        currentLegend->setAutoUpdateModel( false );

        // get model and layer tree root of the legend
        QgsLegendModel *model = currentLegend->model();
        QStringList layerSet;
        Q_FOREACH ( QgsMapLayer *layer, map->layers() )
          layerSet << layer->id();
        //setLayerIdsToLegendModel( model, layerSet, map->scale() );

        // get model and layer tree root of the legend
        QgsLayerTree *root = model->rootGroup();

        // get layerIds find in the layer tree root
        QStringList layerIds = root->findLayerIds();

        // Q_FOREACH layer find in the layer tree
        // remove it if the layer id is not in map layerIds
        Q_FOREACH ( const QString &layerId, layerIds )
        {
          QgsLayerTreeLayer *nodeLayer = root->findLayer( layerId );
          if ( !nodeLayer )
          {
            continue;
          }
          if ( !layerSet.contains( layerId ) )
          {
            qobject_cast<QgsLayerTreeGroup *>( nodeLayer->parent() )->removeChildNode( nodeLayer );
          }
          else
          {
            QgsMapLayer *layer = nodeLayer->layer();
            if ( !layer->isInScaleRange( map->scale() ) )
            {
              qobject_cast<QgsLayerTreeGroup *>( nodeLayer->parent() )->removeChildNode( nodeLayer );
            }
          }
        }
        root->removeChildrenGroupWithoutLayers();
      }
    }
    return true;
  }

#if 0
  QImage *QgsWMSServer::printCompositionToImage( QgsComposition *c ) const
  {
    int width = ( int )( c->paperWidth() * c->printResolution() / 25.4 ); //width in pixel
    int height = ( int )( c->paperHeight() * c->printResolution() / 25.4 ); //height in pixel
    QImage *image = new QImage( QSize( width, height ), QImage::Format_ARGB32 );
    image->setDotsPerMeterX( c->printResolution() / 25.4 * 1000 );
    image->setDotsPerMeterY( c->printResolution() / 25.4 * 1000 );
    image->fill( 0 );
    QPainter p( image );
    QRectF sourceArea( 0, 0, c->paperWidth(), c->paperHeight() );
    QRectF targetArea( 0, 0, width, height );
    c->render( &p, targetArea, sourceArea );
    p.end();
    return image;
  }
#endif

  QImage *QgsRenderer::getMap( HitTest *hitTest )
  {
    QgsMapSettings mapSettings;
    return getMap( mapSettings, hitTest );
  }

  QImage *QgsRenderer::getMap( QgsMapSettings &mapSettings, HitTest *hitTest )
  {
    // check size
    if ( !checkMaximumWidthHeight() )
    {
      throw QgsBadRequestException( QStringLiteral( "Size error" ),
                                    QStringLiteral( "The requested map size is too large" ) );
    }

    // get layers parameters
    QList<QgsMapLayer *> layers;
    QList<QgsWmsParametersLayer> params = mWmsParameters.layersParameters();

    // init layer restorer before doing anything
    std::unique_ptr<QgsLayerRestorer> restorer;
    restorer.reset( new QgsLayerRestorer( mNicknameLayers.values() ) );

    // init stylized layers according to LAYERS/STYLES or SLD
    QString sld = mWmsParameters.sld();
    if ( !sld.isEmpty() )
    {
      layers = sldStylizedLayers( sld );
    }
    else
    {
      layers = stylizedLayers( params );
    }

    // remove unwanted layers (restricted layers, ...)
    removeUnwantedLayers( layers );

    // configure each layer with opacity, selection filter, ...
    bool updateMapExtent = mWmsParameters.bbox().isEmpty() ? true : false;
    Q_FOREACH ( QgsMapLayer *layer, layers )
    {
      Q_FOREACH ( QgsWmsParametersLayer param, params )
      {
        if ( param.mNickname == layerNickname( *layer ) )
        {
          checkLayerReadPermissions( layer );

          setLayerOpacity( layer, param.mOpacity );

          setLayerFilter( layer, param.mFilter );

          setLayerAccessControlFilter( layer );

          setLayerSelection( layer, param.mSelection );

          if ( updateMapExtent )
            updateExtent( layer, mapSettings );

          break;
        }
      }
    }

    // add highlight layers above others
    layers = layers << highlightLayers( mWmsParameters.highlightLayersParameters() );

    // create the output image and the painter
    std::unique_ptr<QPainter> painter;
    std::unique_ptr<QImage> image( createImage() );

    // configure map settings (background, DPI, ...)
    configureMapSettings( image.get(), mapSettings );

    // add layers to map settings (revert order for the rendering)
    std::reverse( layers.begin(), layers.end() );
    mapSettings.setLayers( layers );

    // rendering step for layers
    painter.reset( layersRendering( mapSettings, *image.get(), hitTest ) );

    // rendering step for annotations
    annotationsRendering( painter.get() );

    // painting is terminated
    painter->end();

    // scale output image if necessary (required by WMS spec)
    QImage *scaledImage = scaleImage( image.get() );
    if ( scaledImage )
      image.reset( scaledImage );

    // return
    return image.release();
  }

  QgsDxfExport QgsRenderer::getDxf( const QMap<QString, QString> &options )
  {
    QgsDxfExport dxf;

    // set extent
    QgsRectangle extent = mWmsParameters.bboxAsRectangle();
    dxf.setExtent( extent );

    // get layers parameters
    QList<QgsMapLayer *> layers;
    QList<QgsWmsParametersLayer> params = mWmsParameters.layersParameters();

    // init layer restorer before doing anything
    std::unique_ptr<QgsLayerRestorer> restorer;
    restorer.reset( new QgsLayerRestorer( mNicknameLayers.values() ) );

    // init stylized layers according to LAYERS/STYLES or SLD
    QString sld = mWmsParameters.sld();
    if ( !sld.isEmpty() )
    {
      layers = sldStylizedLayers( sld );
    }
    else
    {
      layers = stylizedLayers( params );
    }

    // layer attributes options
    QStringList layerAttributes;
    QMap<QString, QString>::const_iterator layerAttributesIt = options.find( QStringLiteral( "LAYERATTRIBUTES" ) );
    if ( layerAttributesIt != options.constEnd() )
    {
      layerAttributes = options.value( QStringLiteral( "LAYERATTRIBUTES" ) ).split( ',' );
    }

    // only wfs layers are allowed to be published
    QStringList wfsLayerIds = QgsServerProjectUtils::wfsLayerIds( *mProject );

    // get dxf layers
    QList< QPair<QgsVectorLayer *, int > > dxfLayers;
    int layerIdx = -1;
    Q_FOREACH ( QgsMapLayer *layer, layers )
    {
      layerIdx++;
      if ( layer->type() != QgsMapLayer::VectorLayer )
        continue;
      if ( !wfsLayerIds.contains( layer->id() ) )
        continue;
      Q_FOREACH ( QgsWmsParametersLayer param, params )
      {
        if ( param.mNickname == layerNickname( *layer ) )
        {
          checkLayerReadPermissions( layer );

          setLayerOpacity( layer, param.mOpacity );

          setLayerFilter( layer, param.mFilter );

          setLayerAccessControlFilter( layer );

          break;
        }
      }
      // cast for dxf layers
      QgsVectorLayer *vlayer = static_cast<QgsVectorLayer *>( layer );

      // get the layer attribute used in dxf
      int layerAttribute = -1;
      if ( layerAttributes.size() > layerIdx )
      {
        layerAttribute = vlayer->pendingFields().indexFromName( layerAttributes.at( layerIdx ) );
      }

      dxfLayers.append( qMakePair( vlayer, layerAttribute ) );
    }

    // add layers to dxf
    dxf.addLayers( dxfLayers );

    dxf.setLayerTitleAsName( options.contains( QStringLiteral( "USE_TITLE_AS_LAYERNAME" ) ) );

    //MODE
    QMap<QString, QString>::const_iterator modeIt = options.find( QStringLiteral( "MODE" ) );

    QgsDxfExport::SymbologyExport se;
    if ( modeIt == options.constEnd() )
    {
      se = QgsDxfExport::NoSymbology;
    }
    else
    {
      if ( modeIt->compare( QStringLiteral( "SymbolLayerSymbology" ), Qt::CaseInsensitive ) == 0 )
      {
        se = QgsDxfExport::SymbolLayerSymbology;
      }
      else if ( modeIt->compare( QStringLiteral( "FeatureSymbology" ), Qt::CaseInsensitive ) == 0 )
      {
        se = QgsDxfExport::FeatureSymbology;
      }
      else
      {
        se = QgsDxfExport::NoSymbology;
      }
    }
    dxf.setSymbologyExport( se );

    //SCALE
    QMap<QString, QString>::const_iterator scaleIt = options.find( QStringLiteral( "SCALE" ) );
    if ( scaleIt != options.constEnd() )
    {
      dxf.setSymbologyScale( scaleIt->toDouble() );
    }

    return dxf;
  }

  static void infoPointToMapCoordinates( int i, int j, QgsPointXY *infoPoint, const QgsMapSettings &mapSettings )
  {
    //check if i, j are in the pixel range of the image
    if ( i < 0 || i > mapSettings.outputSize().width() || j < 0 || j > mapSettings.outputSize().height() )
    {
      throw QgsBadRequestException( "InvalidPoint", "I/J parameters not within the pixel range" );
    }

    double xRes = mapSettings.extent().width() / mapSettings.outputSize().width();
    double yRes = mapSettings.extent().height() / mapSettings.outputSize().height();
    infoPoint->setX( mapSettings.extent().xMinimum() + i * xRes + xRes / 2.0 );
    infoPoint->setY( mapSettings.extent().yMaximum() - j * yRes - yRes / 2.0 );
  }

  QByteArray QgsRenderer::getFeatureInfo( const QString &version )
  {
    // Verifying Mandatory parameters
    // The QUERY_LAYERS parameter is Mandatory
    QStringList queryLayers = mWmsParameters.queryLayersNickname();
    if ( queryLayers.isEmpty() )
    {
      QString msg = QObject::tr( "QUERY_LAYERS parameter is required for GetFeatureInfo" );
      throw QgsBadRequestException( QStringLiteral( "LayerNotQueryable" ), msg );
    }

    // The I/J parameters are Mandatory if they are not replaced by X/Y or FILTER or FILTER_GEOM
    bool ijDefined = false;
    if ( !mWmsParameters.i().isEmpty() && !mWmsParameters.j().isEmpty() )
      ijDefined = true;

    bool xyDefined = false;
    if ( !mWmsParameters.x().isEmpty() && !mWmsParameters.y().isEmpty() )
      xyDefined = true;

    bool filtersDefined = false;
    if ( !mWmsParameters.filters().isEmpty() )
      filtersDefined = true;

    bool filterGeomDefined = false;
    if ( !mWmsParameters.filterGeom().isEmpty() )
      filterGeomDefined = true;

    if ( !ijDefined && !xyDefined && !filtersDefined && !filterGeomDefined )
    {
      throw QgsBadRequestException( QStringLiteral( "ParameterMissing" ),
                                    QStringLiteral( "I/J parameters are required for GetFeatureInfo" ) );
    }

    // get layers parameters
    QList<QgsMapLayer *> layers;
    QList<QgsWmsParametersLayer> params = mWmsParameters.layersParameters();

    // init layer restorer before doing anything
    std::unique_ptr<QgsLayerRestorer> restorer;
    restorer.reset( new QgsLayerRestorer( mNicknameLayers.values() ) );

    // init stylized layers according to LAYERS/STYLES or SLD
    QString sld = mWmsParameters.sld();
    if ( !sld.isEmpty() )
      layers = sldStylizedLayers( sld );
    else
      layers = stylizedLayers( params );

    // create the mapSettings and the output image
    QgsMapSettings mapSettings;
    std::unique_ptr<QImage> outputImage( createImage() );

    // configure map settings (background, DPI, ...)
    configureMapSettings( outputImage.get(), mapSettings );

    QgsMessageLog::logMessage( "mapSettings.destinationCrs(): " +  mapSettings.destinationCrs().authid() );
    QgsMessageLog::logMessage( "mapSettings.extent(): " +  mapSettings.extent().toString() );
    QgsMessageLog::logMessage( QStringLiteral( "mapSettings width = %1 height = %2" ).arg( mapSettings.outputSize().width() ).arg( mapSettings.outputSize().height() ) );
    QgsMessageLog::logMessage( QStringLiteral( "mapSettings.mapUnitsPerPixel() = %1" ).arg( mapSettings.mapUnitsPerPixel() ) );

    QgsScaleCalculator scaleCalc( ( outputImage->logicalDpiX() + outputImage->logicalDpiY() ) / 2, mapSettings.destinationCrs().mapUnits() );
    QgsRectangle mapExtent = mapSettings.extent();
    double scaleDenominator = scaleCalc.calculate( mapExtent, outputImage->width() );

    // remove unwanted layers (restricted layers, ...)
    removeUnwantedLayers( layers, scaleDenominator );
    // remove non identifiable layers
    removeNonIdentifiableLayers( layers );

    Q_FOREACH ( QgsMapLayer *layer, layers )
    {
      Q_FOREACH ( QgsWmsParametersLayer param, params )
      {
        if ( param.mNickname == layerNickname( *layer ) )
        {
          checkLayerReadPermissions( layer );

          setLayerFilter( layer, param.mFilter );

          setLayerAccessControlFilter( layer );

          break;
        }
      }
    }

    // add layers to map settings (revert order for the rendering)
    std::reverse( layers.begin(), layers.end() );
    mapSettings.setLayers( layers );

    QDomDocument result = featureInfoDocument( layers, mapSettings, outputImage.get(), version );

    QByteArray ba;

    QgsWmsParameters::Format infoFormat = mWmsParameters.infoFormat();

    if ( infoFormat == QgsWmsParameters::Format::NONE )
    {
      throw QgsBadRequestException( QStringLiteral( "InvalidFormat" ),
                                    QStringLiteral( "Invalid INFO_FORMAT parameter" ) );
    }
    else if ( infoFormat == QgsWmsParameters::Format::TEXT )
      ba = convertFeatureInfoToText( result );
    else if ( infoFormat == QgsWmsParameters::Format::HTML )
      ba = convertFeatureInfoToHtml( result );
    else
      ba = result.toByteArray();

    return ba;
  }

  QImage *QgsRenderer::createImage( int width, int height, bool useBbox ) const
  {
    if ( width < 0 )
      width = mWmsParameters.widthAsInt();

    if ( height < 0 )
      height = mWmsParameters.heightAsInt();

    //Adapt width / height if the aspect ratio does not correspond with the BBOX.
    //Required by WMS spec. 1.3.
    if ( useBbox && mWmsParameters.versionAsNumber() >= QgsProjectVersion( 1, 3, 0 ) )
    {
      QgsRectangle mapExtent = mWmsParameters.bboxAsRectangle();
      if ( !mWmsParameters.bbox().isEmpty() && mapExtent.isEmpty() )
      {
        throw QgsBadRequestException( QStringLiteral( "InvalidParameterValue" ),
                                      QStringLiteral( "Invalid BBOX parameter" ) );
      }

      QString crs = mWmsParameters.crs();
      if ( crs.compare( "CRS:84", Qt::CaseInsensitive ) == 0 )
      {
        crs = QString( "EPSG:4326" );
        mapExtent.invert();
      }
      QgsCoordinateReferenceSystem outputCRS = QgsCoordinateReferenceSystem::fromOgcWmsCrs( crs );
      if ( outputCRS.hasAxisInverted() )
      {
        mapExtent.invert();
      }
      if ( !mapExtent.isEmpty() && height > 0 && width > 0 )
      {
        double mapWidthHeightRatio = mapExtent.width() / mapExtent.height();
        double imageWidthHeightRatio = ( double )width / ( double )height;
        if ( !qgsDoubleNear( mapWidthHeightRatio, imageWidthHeightRatio, 0.0001 ) )
        {
          // inspired by MapServer, mapdraw.c L115
          double cellsize = ( mapExtent.width() / ( double )width ) * 0.5 + ( mapExtent.height() / ( double )height ) * 0.5;
          width = mapExtent.width() / cellsize;
          height = mapExtent.height() / cellsize;
        }
      }
    }

    if ( width <= 0 || height <= 0 )
    {
      throw QgsException( QStringLiteral( "createImage: Invalid width / height parameters" ) );
    }

    QImage *image = nullptr;

    // use alpha channel only if necessary because it slows down performance
    QgsWmsParameters::Format format = mWmsParameters.format();
    bool transparent = mWmsParameters.transparentAsBool();

    if ( transparent && format != QgsWmsParameters::JPG )
    {
      image = new QImage( width, height, QImage::Format_ARGB32_Premultiplied );
      image->fill( 0 );
    }
    else
    {
      image = new QImage( width, height, QImage::Format_RGB32 );
      image->fill( mWmsParameters.backgroundColorAsColor() );
    }

    //apply DPI parameter if present. This is an extension of Qgis Mapserver compared to WMS 1.3.
    //Because of backwards compatibility, this parameter is optional
    double OGC_PX_M = 0.00028; // OGC reference pixel size in meter, also used by qgis
    int dpm = 1 / OGC_PX_M;
    if ( !mWmsParameters.dpi().isEmpty() )
      dpm = mWmsParameters.dpiAsInt() / 0.0254;

    image->setDotsPerMeterX( dpm );
    image->setDotsPerMeterY( dpm );
    return image;
  }

  void QgsRenderer::configureMapSettings( const QPaintDevice *paintDevice, QgsMapSettings &mapSettings ) const
  {
    if ( !paintDevice )
    {
      throw QgsException( QStringLiteral( "configureMapSettings: no paint device" ) );
    }

    mapSettings.datumTransformStore().clear();
    mapSettings.setOutputSize( QSize( paintDevice->width(), paintDevice->height() ) );
    mapSettings.setOutputDpi( paintDevice->logicalDpiX() );

    //map extent
    QgsRectangle mapExtent = mWmsParameters.bboxAsRectangle();
    if ( !mWmsParameters.bbox().isEmpty() && mapExtent.isEmpty() )
    {
      throw QgsBadRequestException( QStringLiteral( "InvalidParameterValue" ), QStringLiteral( "Invalid BBOX parameter" ) );
    }

    QString crs = mWmsParameters.crs();
    if ( crs.compare( "CRS:84", Qt::CaseInsensitive ) == 0 )
    {
      crs = QString( "EPSG:4326" );
      mapExtent.invert();
    }

    QgsCoordinateReferenceSystem outputCRS;

    //wms spec says that CRS parameter is mandatory.
    outputCRS = QgsCoordinateReferenceSystem::fromOgcWmsCrs( crs );
    if ( !outputCRS.isValid() )
    {
      QgsMessageLog::logMessage( QStringLiteral( "Error, could not create output CRS from EPSG" ) );
      throw QgsBadRequestException( QStringLiteral( "InvalidCRS" ), QStringLiteral( "Could not create output CRS" ) );
    }

    //then set destinationCrs
    mapSettings.setDestinationCrs( outputCRS );

    // Change x- and y- of BBOX for WMS 1.3.0 if axis inverted
    if ( mWmsParameters.versionAsNumber() >= QgsProjectVersion( 1, 3, 0 ) && outputCRS.hasAxisInverted() )
    {
      mapExtent.invert();
    }

    mapSettings.setExtent( mapExtent );

    /* Define the background color
     * Transparent or colored
     */
    QgsWmsParameters::Format format = mWmsParameters.format();
    bool transparent = mWmsParameters.transparentAsBool();
    QColor backgroundColor = mWmsParameters.backgroundColorAsColor();

    //set background color
    if ( transparent && format != QgsWmsParameters::JPG )
    {
      mapSettings.setBackgroundColor( QColor( 0, 0, 0, 0 ) );
    }
    else if ( backgroundColor.isValid() )
    {
      mapSettings.setBackgroundColor( backgroundColor );
    }
  }

  QDomDocument QgsRenderer::featureInfoDocument( QList<QgsMapLayer *> &layers, const QgsMapSettings &mapSettings,
      const QImage *outputImage, const QString &version ) const
  {
    QStringList queryLayers = mWmsParameters.queryLayersNickname();

    bool ijDefined = ( !mWmsParameters.i().isEmpty() && !mWmsParameters.j().isEmpty() );

    bool xyDefined = ( !mWmsParameters.x().isEmpty() && !mWmsParameters.y().isEmpty() );

    bool filtersDefined = !mWmsParameters.filters().isEmpty();

    bool filterGeomDefined = !mWmsParameters.filterGeom().isEmpty();

    int featureCount = mWmsParameters.featureCountAsInt();
    if ( featureCount < 1 )
    {
      featureCount = 1;
    }

    int i = mWmsParameters.iAsInt();
    int j = mWmsParameters.jAsInt();
    if ( xyDefined && !ijDefined )
    {
      i = mWmsParameters.xAsInt();
      j = mWmsParameters.yAsInt();
    }
    int width = mWmsParameters.widthAsInt();
    int height = mWmsParameters.heightAsInt();
    if ( ( i != -1 && j != -1 && width != 0 && height != 0 ) && ( width != outputImage->width() || height != outputImage->height() ) )
    {
      i *= ( outputImage->width() / ( double )width );
      j *= ( outputImage->height() / ( double )height );
    }

    // init search variables
    std::unique_ptr<QgsRectangle> featuresRect;
    std::unique_ptr<QgsGeometry> filterGeom;
    std::unique_ptr<QgsPointXY> infoPoint;

    if ( i != -1 && j != -1 )
    {
      infoPoint.reset( new QgsPointXY() );
      infoPointToMapCoordinates( i, j, infoPoint.get(), mapSettings );
    }
    else if ( filtersDefined )
    {
      featuresRect.reset( new QgsRectangle() );
    }
    else if ( filterGeomDefined )
    {
      filterGeom.reset( new QgsGeometry( QgsGeometry::fromWkt( mWmsParameters.filterGeom() ) ) );
    }

    QDomDocument result;

    QDomElement getFeatureInfoElement;
    QgsWmsParameters::Format infoFormat = mWmsParameters.infoFormat();
    if ( infoFormat == QgsWmsParameters::Format::GML )
    {
      getFeatureInfoElement = result.createElement( QStringLiteral( "wfs:FeatureCollection" ) );
      getFeatureInfoElement.setAttribute( QStringLiteral( "xmlns:wfs" ), QStringLiteral( "http://www.opengis.net/wfs" ) );
      getFeatureInfoElement.setAttribute( QStringLiteral( "xmlns:ogc" ), QStringLiteral( "http://www.opengis.net/ogc" ) );
      getFeatureInfoElement.setAttribute( QStringLiteral( "xmlns:gml" ), QStringLiteral( "http://www.opengis.net/gml" ) );
      getFeatureInfoElement.setAttribute( QStringLiteral( "xmlns:ows" ), QStringLiteral( "http://www.opengis.net/ows" ) );
      getFeatureInfoElement.setAttribute( QStringLiteral( "xmlns:xlink" ), QStringLiteral( "http://www.w3.org/1999/xlink" ) );
      getFeatureInfoElement.setAttribute( QStringLiteral( "xmlns:qgs" ), QStringLiteral( "http://qgis.org/gml" ) );
      getFeatureInfoElement.setAttribute( QStringLiteral( "xmlns:xsi" ), QStringLiteral( "http://www.w3.org/2001/XMLSchema-instance" ) );
      getFeatureInfoElement.setAttribute( QStringLiteral( "xsi:schemaLocation" ), QStringLiteral( "http://www.opengis.net/wfs http://schemas.opengis.net/wfs/1.0.0/wfs.xsd http://qgis.org/gml" ) );
    }
    else
    {
      QString featureInfoElemName = QgsServerProjectUtils::wmsFeatureInfoDocumentElement( *mProject );
      if ( featureInfoElemName.isEmpty() )
      {
        featureInfoElemName = QStringLiteral( "GetFeatureInfoResponse" );
      }
      QString featureInfoElemNs = QgsServerProjectUtils::wmsFeatureInfoDocumentElementNs( *mProject );
      if ( featureInfoElemNs.isEmpty() )
      {
        getFeatureInfoElement = result.createElement( featureInfoElemName );
      }
      else
      {
        getFeatureInfoElement = result.createElementNS( featureInfoElemNs, featureInfoElemName );
      }
      //feature info schema
      QString featureInfoSchema = QgsServerProjectUtils::wmsFeatureInfoSchema( *mProject );
      if ( !featureInfoSchema.isEmpty() )
      {
        getFeatureInfoElement.setAttribute( QStringLiteral( "xmlns:xsi" ), QStringLiteral( "http://www.w3.org/2001/XMLSchema-instance" ) );
        getFeatureInfoElement.setAttribute( QStringLiteral( "xsi:schemaLocation" ), featureInfoSchema );
      }
    }
    result.appendChild( getFeatureInfoElement );

    //Render context is needed to determine feature visibility for vector layers
    QgsRenderContext renderContext = QgsRenderContext::fromMapSettings( mapSettings );

    bool sia2045 = QgsServerProjectUtils::wmsInfoFormatSia2045( *mProject );

    //layers can have assigned a different name for GetCapabilities
    QHash<QString, QString> layerAliasMap = QgsServerProjectUtils::wmsFeatureInfoLayerAliasMap( *mProject );

    Q_FOREACH ( QString queryLayer, queryLayers )
    {
      bool validLayer = false;
      Q_FOREACH ( QgsMapLayer *layer, layers )
      {
        if ( queryLayer == layerNickname( *layer ) )
        {
          validLayer = true;

          QDomElement layerElement;
          if ( infoFormat == QgsWmsParameters::Format::GML )
          {
            layerElement = getFeatureInfoElement;
          }
          else
          {
            layerElement = result.createElement( QStringLiteral( "Layer" ) );
            QString layerName = queryLayer;

            //check if the layer is given a different name for GetFeatureInfo output
            QHash<QString, QString>::const_iterator layerAliasIt = layerAliasMap.find( layerName );
            if ( layerAliasIt != layerAliasMap.constEnd() )
            {
              layerName = layerAliasIt.value();
            }

            layerElement.setAttribute( QStringLiteral( "name" ), layerName );
            getFeatureInfoElement.appendChild( layerElement );
            if ( sia2045 ) //the name might not be unique after alias replacement
            {
              layerElement.setAttribute( QStringLiteral( "id" ), layer->id() );
            }
          }

          if ( layer->type() == QgsMapLayer::VectorLayer )
          {
            QgsVectorLayer *vectorLayer = qobject_cast<QgsVectorLayer *>( layer );
            if ( vectorLayer )
            {
              ( void )featureInfoFromVectorLayer( vectorLayer, infoPoint.get(), featureCount, result, layerElement, mapSettings, renderContext, version, featuresRect.get(), filterGeom.get() );
              break;
            }
          }
          else
          {
            if ( infoFormat == QgsWmsParameters::Format::GML )
            {
              layerElement = result.createElement( QStringLiteral( "gml:featureMember" )/*wfs:FeatureMember*/ );
              getFeatureInfoElement.appendChild( layerElement );
            }

            QgsRasterLayer *rasterLayer = qobject_cast<QgsRasterLayer *>( layer );
            if ( rasterLayer )
            {
              if ( !infoPoint )
              {
                break;
              }
              QgsPointXY layerInfoPoint = mapSettings.mapToLayerCoordinates( layer, *( infoPoint.get() ) );
              ( void )featureInfoFromRasterLayer( rasterLayer, mapSettings, &layerInfoPoint, result, layerElement, version );
              break;
            }
          }
          break;
        }
      }

      if ( !validLayer )
      {
        QString msg = QObject::tr( "Layer '%1' not found" ).arg( queryLayer );
        throw QgsBadRequestException( QStringLiteral( "LayerNotDefined" ), msg );
      }
    }

    if ( featuresRect )
    {
      if ( infoFormat == QgsWmsParameters::Format::GML )
      {
        QDomElement bBoxElem = result.createElement( QStringLiteral( "gml:boundedBy" ) );
        QDomElement boxElem;
        int gmlVersion = mWmsParameters.infoFormatVersion();
        if ( gmlVersion < 3 )
        {
          boxElem = QgsOgcUtils::rectangleToGMLBox( featuresRect.get(), result, 8 );
        }
        else
        {
          boxElem = QgsOgcUtils::rectangleToGMLEnvelope( featuresRect.get(), result, 8 );
        }

        QgsCoordinateReferenceSystem crs = mapSettings.destinationCrs();
        if ( crs.isValid() )
        {
          boxElem.setAttribute( QStringLiteral( "srsName" ), crs.authid() );
        }
        bBoxElem.appendChild( boxElem );
        getFeatureInfoElement.insertBefore( bBoxElem, QDomNode() ); //insert as first child
      }
      else
      {
        QDomElement bBoxElem = result.createElement( QStringLiteral( "BoundingBox" ) );
        bBoxElem.setAttribute( QStringLiteral( "CRS" ), mapSettings.destinationCrs().authid() );
        bBoxElem.setAttribute( QStringLiteral( "minx" ), qgsDoubleToString( featuresRect->xMinimum(), 8 ) );
        bBoxElem.setAttribute( QStringLiteral( "maxx" ), qgsDoubleToString( featuresRect->xMaximum(), 8 ) );
        bBoxElem.setAttribute( QStringLiteral( "miny" ), qgsDoubleToString( featuresRect->yMinimum(), 8 ) );
        bBoxElem.setAttribute( QStringLiteral( "maxy" ), qgsDoubleToString( featuresRect->yMaximum(), 8 ) );
        getFeatureInfoElement.insertBefore( bBoxElem, QDomNode() ); //insert as first child
      }
    }

    if ( sia2045 && infoFormat == QgsWmsParameters::Format::XML )
    {
      convertFeatureInfoToSia2045( result );
    }

    return result;
  }

  bool QgsRenderer::featureInfoFromVectorLayer( QgsVectorLayer *layer,
      const QgsPointXY *infoPoint,
      int nFeatures,
      QDomDocument &infoDocument,
      QDomElement &layerElement,
      const QgsMapSettings &mapSettings,
      QgsRenderContext &renderContext,
      const QString &version,
      QgsRectangle *featureBBox,
      QgsGeometry *filterGeom ) const
  {
    if ( !layer )
    {
      return false;
    }

    //we need a selection rect (0.01 of map width)
    QgsRectangle mapRect = mapSettings.extent();
    QgsRectangle layerRect = mapSettings.mapToLayerCoordinates( layer, mapRect );


    QgsRectangle searchRect;

    //info point could be 0 in case there is only an attribute filter
    if ( infoPoint )
    {
      searchRect = featureInfoSearchRect( layer, mapSettings, renderContext, *infoPoint );
    }
    else if ( filterGeom )
    {
      searchRect = filterGeom->boundingBox();
    }
    else if ( mParameters.contains( QStringLiteral( "BBOX" ) ) )
    {
      searchRect = layerRect;
    }

    //do a select with searchRect and go through all the features

    QgsFeature feature;
    QgsAttributes featureAttributes;
    int featureCounter = 0;
    layer->updateFields();
    const QgsFields &fields = layer->pendingFields();
    bool addWktGeometry = QgsServerProjectUtils::wmsFeatureInfoAddWktGeometry( *mProject );
    bool segmentizeWktGeometry = QgsServerProjectUtils::wmsFeatureInfoSegmentizeWktGeometry( *mProject );
    const QSet<QString> &excludedAttributes = layer->excludeAttributesWms();

    QgsFeatureRequest fReq;
    bool hasGeometry = addWktGeometry || featureBBox || filterGeom;
    fReq.setFlags( ( ( hasGeometry ) ? QgsFeatureRequest::NoFlags : QgsFeatureRequest::NoGeometry ) | QgsFeatureRequest::ExactIntersect );

    if ( ! searchRect.isEmpty() )
    {
      fReq.setFilterRect( searchRect );
    }
    else
    {
      fReq.setFlags( fReq.flags() & ~ QgsFeatureRequest::ExactIntersect );
    }

    if ( filterGeom )
    {
      fReq.setFilterExpression( QString( "intersects( $geometry, geom_from_wkt('%1') )" ).arg( filterGeom->exportToWkt() ) );
    }

#ifdef HAVE_SERVER_PYTHON_PLUGINS
    mAccessControl->filterFeatures( layer, fReq );

    QStringList attributes;
    QgsField field;
    Q_FOREACH ( field, layer->pendingFields().toList() )
    {
      attributes.append( field.name() );
    }
    attributes = mAccessControl->layerAttributes( layer, attributes );
    fReq.setSubsetOfAttributes( attributes, layer->pendingFields() );
#endif

    QgsFeatureIterator fit = layer->getFeatures( fReq );
    QgsFeatureRenderer *r2 = layer->renderer();
    if ( r2 )
    {
      r2->startRender( renderContext, layer->pendingFields() );
    }

    bool featureBBoxInitialized = false;
    while ( fit.nextFeature( feature ) )
    {
      if ( layer->wkbType() == QgsWkbTypes::NoGeometry && ! searchRect.isEmpty() )
      {
        break;
      }

      ++featureCounter;
      if ( featureCounter > nFeatures )
      {
        break;
      }

      renderContext.expressionContext().setFeature( feature );

      if ( layer->wkbType() != QgsWkbTypes::NoGeometry && ! searchRect.isEmpty() )
      {
        if ( !r2 )
        {
          continue;
        }

        //check if feature is rendered at all
        bool render = r2->willRenderFeature( feature, renderContext );
        if ( !render )
        {
          continue;
        }
      }

      QgsRectangle box;
      if ( layer->wkbType() != QgsWkbTypes::NoGeometry && hasGeometry )
      {
        box = mapSettings.layerExtentToOutputExtent( layer, feature.geometry().boundingBox() );
        if ( featureBBox ) //extend feature info bounding box if requested
        {
          if ( !featureBBoxInitialized && featureBBox->isEmpty() )
          {
            *featureBBox = box;
            featureBBoxInitialized = true;
          }
          else
          {
            featureBBox->combineExtentWith( box );
          }
        }
      }

      QgsCoordinateReferenceSystem outputCrs = layer->crs();
      if ( layer->crs() != mapSettings.destinationCrs() )
      {
        outputCrs = mapSettings.destinationCrs();
      }

      if ( mWmsParameters.infoFormat() == QgsWmsParameters::Format::GML )
      {
        bool withGeom = layer->wkbType() != QgsWkbTypes::NoGeometry && addWktGeometry;
        int gmlVersion = mWmsParameters.infoFormatVersion();
        QString typeName = layerNickname( *layer );
        QDomElement elem = createFeatureGML(
                             &feature, layer, infoDocument, outputCrs, mapSettings, typeName, withGeom, gmlVersion
#ifdef HAVE_SERVER_PYTHON_PLUGINS
                             , &attributes
#endif
                           );
        QDomElement featureMemberElem = infoDocument.createElement( QStringLiteral( "gml:featureMember" )/*wfs:FeatureMember*/ );
        featureMemberElem.appendChild( elem );
        layerElement.appendChild( featureMemberElem );
        continue;
      }
      else
      {
        QDomElement featureElement = infoDocument.createElement( QStringLiteral( "Feature" ) );
        featureElement.setAttribute( QStringLiteral( "id" ), FID_TO_STRING( feature.id() ) );
        layerElement.appendChild( featureElement );

        //read all attribute values from the feature
        featureAttributes = feature.attributes();
        for ( int i = 0; i < featureAttributes.count(); ++i )
        {
          //skip attribute if it is explicitly excluded from WMS publication
          if ( excludedAttributes.contains( fields.at( i ).name() ) )
          {
            continue;
          }
#ifdef HAVE_SERVER_PYTHON_PLUGINS
          //skip attribute if it is excluded by access control
          if ( !attributes.contains( fields.at( i ).name() ) )
          {
            continue;
          }
#endif

          //replace attribute name if there is an attribute alias?
          QString attributeName = layer->attributeDisplayName( i );

          QDomElement attributeElement = infoDocument.createElement( QStringLiteral( "Attribute" ) );
          attributeElement.setAttribute( QStringLiteral( "name" ), attributeName );
          attributeElement.setAttribute( QStringLiteral( "value" ),
                                         replaceValueMapAndRelation(
                                           layer, i,
                                           featureAttributes[i].isNull() ?  QString() : QgsExpression::replaceExpressionText( featureAttributes[i].toString(), &renderContext.expressionContext() )
                                         )
                                       );
          featureElement.appendChild( attributeElement );
        }

        //add maptip attribute based on html/expression (in case there is no maptip attribute)
        QString mapTip = layer->mapTipTemplate();
        if ( !mapTip.isEmpty() )
        {
          QDomElement maptipElem = infoDocument.createElement( QStringLiteral( "Attribute" ) );
          maptipElem.setAttribute( QStringLiteral( "name" ), QStringLiteral( "maptip" ) );
          maptipElem.setAttribute( QStringLiteral( "value" ),  QgsExpression::replaceExpressionText( mapTip, &renderContext.expressionContext() ) );
          featureElement.appendChild( maptipElem );
        }

        //append feature bounding box to feature info xml
        if ( layer->wkbType() != QgsWkbTypes::NoGeometry && hasGeometry )
        {
          QDomElement bBoxElem = infoDocument.createElement( QStringLiteral( "BoundingBox" ) );
          bBoxElem.setAttribute( version == QLatin1String( "1.1.1" ) ? "SRS" : "CRS", outputCrs.authid() );
          bBoxElem.setAttribute( QStringLiteral( "minx" ), qgsDoubleToString( box.xMinimum(), getWMSPrecision() ) );
          bBoxElem.setAttribute( QStringLiteral( "maxx" ), qgsDoubleToString( box.xMaximum(), getWMSPrecision() ) );
          bBoxElem.setAttribute( QStringLiteral( "miny" ), qgsDoubleToString( box.yMinimum(), getWMSPrecision() ) );
          bBoxElem.setAttribute( QStringLiteral( "maxy" ), qgsDoubleToString( box.yMaximum(), getWMSPrecision() ) );
          featureElement.appendChild( bBoxElem );
        }

        //also append the wkt geometry as an attribute
        if ( layer->wkbType() != QgsWkbTypes::NoGeometry && addWktGeometry && hasGeometry )
        {
          QgsGeometry geom = feature.geometry();
          if ( !geom.isNull() )
          {
            if ( layer->crs() != outputCrs )
            {
              QgsCoordinateTransform transform = mapSettings.layerTransform( layer );
              if ( transform.isValid() )
                geom.transform( transform );
            }

            if ( segmentizeWktGeometry )
            {
              QgsAbstractGeometry *abstractGeom = geom.geometry();
              if ( abstractGeom )
              {
                if ( QgsWkbTypes::isCurvedType( abstractGeom->wkbType() ) )
                {
                  QgsAbstractGeometry *segmentizedGeom = abstractGeom->segmentize();
                  geom.setGeometry( segmentizedGeom );
                }
              }
            }
            QDomElement geometryElement = infoDocument.createElement( QStringLiteral( "Attribute" ) );
            geometryElement.setAttribute( QStringLiteral( "name" ), QStringLiteral( "geometry" ) );
            geometryElement.setAttribute( QStringLiteral( "value" ), geom.exportToWkt( getWMSPrecision() ) );
            geometryElement.setAttribute( QStringLiteral( "type" ), QStringLiteral( "derived" ) );
            featureElement.appendChild( geometryElement );
          }
        }
      }
    }
    if ( r2 )
    {
      r2->stopRender( renderContext );
    }

    return true;
  }

  bool QgsRenderer::featureInfoFromRasterLayer( QgsRasterLayer *layer,
      const QgsMapSettings &mapSettings,
      const QgsPointXY *infoPoint,
      QDomDocument &infoDocument,
      QDomElement &layerElement,
      const QString &version ) const
  {
    Q_UNUSED( version );

    if ( !infoPoint || !layer || !layer->dataProvider() )
    {
      return false;
    }

    QgsMessageLog::logMessage( QStringLiteral( "infoPoint: %1 %2" ).arg( infoPoint->x() ).arg( infoPoint->y() ) );

    if ( !( layer->dataProvider()->capabilities() & QgsRasterDataProvider::IdentifyValue ) )
    {
      return false;
    }

    QgsRasterIdentifyResult identifyResult;
    // use context extent, width height (comes with request) to use WCS cache
    // We can only use context if raster is not reprojected, otherwise it is difficult
    // to guess correct source resolution
    if ( layer->dataProvider()->crs() != mapSettings.destinationCrs() )
    {
      identifyResult = layer->dataProvider()->identify( *infoPoint, QgsRaster::IdentifyFormatValue );
    }
    else
    {
      identifyResult = layer->dataProvider()->identify( *infoPoint, QgsRaster::IdentifyFormatValue, mapSettings.extent(), mapSettings.outputSize().width(), mapSettings.outputSize().height() );
    }

    if ( !identifyResult.isValid() )
      return false;

    QMap<int, QVariant> attributes = identifyResult.results();

    if ( mWmsParameters.infoFormat() == QgsWmsParameters::Format::GML )
    {
      QgsFeature feature;
      QgsFields fields;
      feature.initAttributes( attributes.count() );
      int index = 0;
      for ( auto it = attributes.constBegin(); it != attributes.constEnd(); ++it )
      {
        fields.append( QgsField( layer->bandName( it.key() ), QVariant::Double ) );
        feature.setAttribute( index++, QString::number( it.value().toDouble() ) );
      }
      feature.setFields( fields );

      QgsCoordinateReferenceSystem layerCrs = layer->crs();
      int gmlVersion = mWmsParameters.infoFormatVersion();
      QString typeName = layerNickname( *layer );
      QDomElement elem = createFeatureGML(
                           &feature, nullptr, infoDocument, layerCrs, mapSettings, typeName, false, gmlVersion, nullptr );
      layerElement.appendChild( elem );
    }
    else
    {
      for ( auto it = attributes.constBegin(); it != attributes.constEnd(); ++it )
      {
        QDomElement attributeElement = infoDocument.createElement( QStringLiteral( "Attribute" ) );
        attributeElement.setAttribute( QStringLiteral( "name" ), layer->bandName( it.key() ) );
        attributeElement.setAttribute( QStringLiteral( "value" ), QString::number( it.value().toDouble() ) );
        layerElement.appendChild( attributeElement );
      }
    }
    return true;
  }

  bool QgsRenderer::testFilterStringSafety( const QString &filter ) const
  {
    //; too dangerous for sql injections
    if ( filter.contains( QLatin1String( ";" ) ) )
    {
      return false;
    }

    QStringList tokens = filter.split( ' ', QString::SkipEmptyParts );
    groupStringList( tokens, QStringLiteral( "'" ) );
    groupStringList( tokens, QStringLiteral( "\"" ) );

    for ( auto tokenIt = tokens.constBegin() ; tokenIt != tokens.constEnd(); ++tokenIt )
    {
      //whitelist of allowed characters and keywords
      if ( tokenIt->compare( QLatin1String( "," ) ) == 0
           || tokenIt->compare( QLatin1String( "(" ) ) == 0
           || tokenIt->compare( QLatin1String( ")" ) ) == 0
           || tokenIt->compare( QLatin1String( "=" ) ) == 0
           || tokenIt->compare( QLatin1String( "!=" ) ) == 0
           || tokenIt->compare( QLatin1String( "<" ) ) == 0
           || tokenIt->compare( QLatin1String( "<=" ) ) == 0
           || tokenIt->compare( QLatin1String( ">" ) ) == 0
           || tokenIt->compare( QLatin1String( ">=" ) ) == 0
           || tokenIt->compare( QLatin1String( "%" ) ) == 0
           || tokenIt->compare( QLatin1String( "AND" ), Qt::CaseInsensitive ) == 0
           || tokenIt->compare( QLatin1String( "OR" ), Qt::CaseInsensitive ) == 0
           || tokenIt->compare( QLatin1String( "IN" ), Qt::CaseInsensitive ) == 0
           || tokenIt->compare( QLatin1String( "LIKE" ), Qt::CaseInsensitive ) == 0
           || tokenIt->compare( QLatin1String( "ILIKE" ), Qt::CaseInsensitive ) == 0
           || tokenIt->compare( QLatin1String( "DMETAPHONE" ), Qt::CaseInsensitive ) == 0
           || tokenIt->compare( QLatin1String( "SOUNDEX" ), Qt::CaseInsensitive ) == 0 )
      {
        continue;
      }

      //numbers are OK
      bool isNumeric;
      tokenIt->toDouble( &isNumeric );
      if ( isNumeric )
      {
        continue;
      }

      //numeric strings need to be quoted once either with single or with double quotes

      //empty strings are OK
      if ( *tokenIt == QLatin1String( "''" ) )
      {
        continue;
      }

      //single quote
      if ( tokenIt->size() > 2
           && ( *tokenIt )[0] == QChar( '\'' )
           && ( *tokenIt )[tokenIt->size() - 1] == QChar( '\'' )
           && ( *tokenIt )[1] != QChar( '\'' )
           && ( *tokenIt )[tokenIt->size() - 2] != QChar( '\'' ) )
      {
        continue;
      }

      //double quote
      if ( tokenIt->size() > 2
           && ( *tokenIt )[0] == QChar( '"' )
           && ( *tokenIt )[tokenIt->size() - 1] == QChar( '"' )
           && ( *tokenIt )[1] != QChar( '"' )
           && ( *tokenIt )[tokenIt->size() - 2] != QChar( '"' ) )
      {
        continue;
      }

      return false;
    }

    return true;
  }

  void QgsRenderer::groupStringList( QStringList &list, const QString &groupString )
  {
    //group contents within single quotes together
    bool groupActive = false;
    int startGroup = -1;
    QString concatString;

    for ( int i = 0; i < list.size(); ++i )
    {
      QString &str = list[i];
      if ( str.startsWith( groupString ) )
      {
        startGroup = i;
        groupActive = true;
        concatString.clear();
      }

      if ( groupActive )
      {
        if ( i != startGroup )
        {
          concatString.append( " " );
        }
        concatString.append( str );
      }

      if ( str.endsWith( groupString ) )
      {
        int endGroup = i;
        groupActive = false;

        if ( startGroup != -1 )
        {
          list[startGroup] = concatString;
          for ( int j = startGroup + 1; j <= endGroup; ++j )
          {
            list.removeAt( startGroup + 1 );
            --i;
          }
        }

        concatString.clear();
        startGroup = -1;
      }
    }
  }

  bool QgsRenderer::checkMaximumWidthHeight() const
  {
    //test if maxWidth / maxHeight set and WIDTH / HEIGHT parameter is in the range
    int wmsMaxWidth = QgsServerProjectUtils::wmsMaxWidth( *mProject );
    int width = mWmsParameters.widthAsInt();
    if ( wmsMaxWidth != -1 && width > wmsMaxWidth )
    {
      return false;
    }

    int wmsMaxHeight = QgsServerProjectUtils::wmsMaxHeight( *mProject );
    int height = mWmsParameters.heightAsInt();
    if ( wmsMaxHeight != -1 && height > wmsMaxHeight )
    {
      return false;
    }

    return true;
  }

  void QgsRenderer::convertFeatureInfoToSia2045( QDomDocument &doc ) const
  {
    QDomDocument SIAInfoDoc;
    QDomElement infoDocElement = doc.documentElement();
    QDomElement SIAInfoDocElement = SIAInfoDoc.importNode( infoDocElement, false ).toElement();
    SIAInfoDoc.appendChild( SIAInfoDocElement );

    QString currentAttributeName;
    QString currentAttributeValue;
    QDomElement currentAttributeElem;
    QString currentLayerName;
    QDomElement currentLayerElem;
    QDomNodeList layerNodeList = infoDocElement.elementsByTagName( QStringLiteral( "Layer" ) );
    for ( int i = 0; i < layerNodeList.size(); ++i )
    {
      currentLayerElem = layerNodeList.at( i ).toElement();
      currentLayerName = currentLayerElem.attribute( QStringLiteral( "name" ) );

      QDomElement currentFeatureElem;

      QDomNodeList featureList = currentLayerElem.elementsByTagName( QStringLiteral( "Feature" ) );
      if ( featureList.isEmpty() )
      {
        //raster?
        QDomNodeList attributeList = currentLayerElem.elementsByTagName( QStringLiteral( "Attribute" ) );
        QDomElement rasterLayerElem;
        if ( !attributeList.isEmpty() )
        {
          rasterLayerElem = SIAInfoDoc.createElement( currentLayerName );
        }
        for ( int j = 0; j < attributeList.size(); ++j )
        {
          currentAttributeElem = attributeList.at( j ).toElement();
          currentAttributeName = currentAttributeElem.attribute( QStringLiteral( "name" ) );
          currentAttributeValue = currentAttributeElem.attribute( QStringLiteral( "value" ) );
          QDomElement outAttributeElem = SIAInfoDoc.createElement( currentAttributeName );
          QDomText outAttributeText = SIAInfoDoc.createTextNode( currentAttributeValue );
          outAttributeElem.appendChild( outAttributeText );
          rasterLayerElem.appendChild( outAttributeElem );
        }
        if ( !attributeList.isEmpty() )
        {
          SIAInfoDocElement.appendChild( rasterLayerElem );
        }
      }
      else //vector
      {
        //property attributes
        QSet<QString> layerPropertyAttributes;
        QString currentLayerId = currentLayerElem.attribute( QStringLiteral( "id" ) );
        if ( !currentLayerId.isEmpty() )
        {
          QgsMapLayer *currentLayer = mProject->mapLayer( currentLayerId );
          if ( currentLayer )
          {
            QString WMSPropertyAttributesString = currentLayer->customProperty( QStringLiteral( "WMSPropertyAttributes" ) ).toString();
            if ( !WMSPropertyAttributesString.isEmpty() )
            {
              QStringList propertyList = WMSPropertyAttributesString.split( QStringLiteral( "//" ) );
              for ( auto propertyIt = propertyList.constBegin() ; propertyIt != propertyList.constEnd(); ++propertyIt )
              {
                layerPropertyAttributes.insert( *propertyIt );
              }
            }
          }
        }

        QDomElement propertyRefChild; //child to insert the next property after (or
        for ( int j = 0; j < featureList.size(); ++j )
        {
          QDomElement SIAFeatureElem = SIAInfoDoc.createElement( currentLayerName );
          currentFeatureElem = featureList.at( j ).toElement();
          QDomNodeList attributeList = currentFeatureElem.elementsByTagName( QStringLiteral( "Attribute" ) );

          for ( int k = 0; k < attributeList.size(); ++k )
          {
            currentAttributeElem = attributeList.at( k ).toElement();
            currentAttributeName = currentAttributeElem.attribute( QStringLiteral( "name" ) );
            currentAttributeValue = currentAttributeElem.attribute( QStringLiteral( "value" ) );
            if ( layerPropertyAttributes.contains( currentAttributeName ) )
            {
              QDomElement propertyElem = SIAInfoDoc.createElement( QStringLiteral( "property" ) );
              QDomElement identifierElem = SIAInfoDoc.createElement( QStringLiteral( "identifier" ) );
              QDomText identifierText = SIAInfoDoc.createTextNode( currentAttributeName );
              identifierElem.appendChild( identifierText );
              QDomElement valueElem = SIAInfoDoc.createElement( QStringLiteral( "value" ) );
              QDomText valueText = SIAInfoDoc.createTextNode( currentAttributeValue );
              valueElem.appendChild( valueText );
              propertyElem.appendChild( identifierElem );
              propertyElem.appendChild( valueElem );
              if ( propertyRefChild.isNull() )
              {
                SIAFeatureElem.insertBefore( propertyElem, QDomNode() );
                propertyRefChild = propertyElem;
              }
              else
              {
                SIAFeatureElem.insertAfter( propertyElem, propertyRefChild );
              }
            }
            else
            {
              QDomElement SIAAttributeElem = SIAInfoDoc.createElement( currentAttributeName );
              QDomText SIAAttributeText = SIAInfoDoc.createTextNode( currentAttributeValue );
              SIAAttributeElem.appendChild( SIAAttributeText );
              SIAFeatureElem.appendChild( SIAAttributeElem );
            }
          }
          SIAInfoDocElement.appendChild( SIAFeatureElem );
        }
      }
    }
    doc = SIAInfoDoc;
  }

  QByteArray QgsRenderer::convertFeatureInfoToHtml( const QDomDocument &doc ) const
  {
    QString featureInfoString;

    //the HTML head
    featureInfoString.append( "<HEAD>\n" );
    featureInfoString.append( "<TITLE> GetFeatureInfo results </TITLE>\n" );
    featureInfoString.append( "<meta http-equiv=\"Content-Type\" content=\"text/html;charset=utf-8\">\n" );
    featureInfoString.append( "</HEAD>\n" );

    //start the html body
    featureInfoString.append( "<BODY>\n" );

    QDomNodeList layerList = doc.elementsByTagName( QStringLiteral( "Layer" ) );

    //layer loop
    for ( int i = 0; i < layerList.size(); ++i )
    {
      QDomElement layerElem = layerList.at( i ).toElement();

      featureInfoString.append( "<TABLE border=1 width=100%>\n" );
      featureInfoString.append( "<TR><TH width=25%>Layer</TH><TD>" + layerElem.attribute( QStringLiteral( "name" ) ) + "</TD></TR>\n" );
      featureInfoString.append( "</BR>" );

      //feature loop (for vector layers)
      QDomNodeList featureNodeList = layerElem.elementsByTagName( QStringLiteral( "Feature" ) );
      QDomElement currentFeatureElement;

      if ( !featureNodeList.isEmpty() ) //vector layer
      {
        for ( int j = 0; j < featureNodeList.size(); ++j )
        {
          QDomElement featureElement = featureNodeList.at( j ).toElement();
          featureInfoString.append( "<TABLE border=1 width=100%>\n" );
          featureInfoString.append( "<TR><TH>Feature</TH><TD>" + featureElement.attribute( QStringLiteral( "id" ) ) +
                                    "</TD></TR>\n" );

          //attribute loop
          QDomNodeList attributeNodeList = featureElement.elementsByTagName( QStringLiteral( "Attribute" ) );
          for ( int k = 0; k < attributeNodeList.size(); ++k )
          {
            QDomElement attributeElement = attributeNodeList.at( k ).toElement();
            featureInfoString.append( "<TR><TH>" + attributeElement.attribute( QStringLiteral( "name" ) ) +
                                      "</TH><TD>" + attributeElement.attribute( QStringLiteral( "value" ) ) + "</TD></TR>\n" );
          }

          featureInfoString.append( "</TABLE>\n</BR>\n" );
        }
      }
      else //raster layer
      {
        QDomNodeList attributeNodeList = layerElem.elementsByTagName( QStringLiteral( "Attribute" ) );
        for ( int j = 0; j < attributeNodeList.size(); ++j )
        {
          QDomElement attributeElement = attributeNodeList.at( j ).toElement();
          featureInfoString.append( "<TR><TH>" + attributeElement.attribute( QStringLiteral( "name" ) ) +
                                    "</TH><TD>" + attributeElement.attribute( QStringLiteral( "value" ) ) + "</TD></TR>\n" );
        }
      }

      featureInfoString.append( "</TABLE>\n<BR></BR>\n" );
    }

    //start the html body
    featureInfoString.append( "</BODY>\n" );

    return featureInfoString.toUtf8();
  }

  QByteArray QgsRenderer::convertFeatureInfoToText( const QDomDocument &doc ) const
  {
    QString featureInfoString;

    //the Text head
    featureInfoString.append( "GetFeatureInfo results\n" );
    featureInfoString.append( "\n" );

    QDomNodeList layerList = doc.elementsByTagName( QStringLiteral( "Layer" ) );

    //layer loop
    for ( int i = 0; i < layerList.size(); ++i )
    {
      QDomElement layerElem = layerList.at( i ).toElement();

      featureInfoString.append( "Layer '" + layerElem.attribute( QStringLiteral( "name" ) ) + "'\n" );

      //feature loop (for vector layers)
      QDomNodeList featureNodeList = layerElem.elementsByTagName( QStringLiteral( "Feature" ) );
      QDomElement currentFeatureElement;

      if ( !featureNodeList.isEmpty() ) //vector layer
      {
        for ( int j = 0; j < featureNodeList.size(); ++j )
        {
          QDomElement featureElement = featureNodeList.at( j ).toElement();
          featureInfoString.append( "Feature " + featureElement.attribute( QStringLiteral( "id" ) ) + "\n" );

          //attribute loop
          QDomNodeList attributeNodeList = featureElement.elementsByTagName( QStringLiteral( "Attribute" ) );
          for ( int k = 0; k < attributeNodeList.size(); ++k )
          {
            QDomElement attributeElement = attributeNodeList.at( k ).toElement();
            featureInfoString.append( attributeElement.attribute( QStringLiteral( "name" ) ) + " = '" +
                                      attributeElement.attribute( QStringLiteral( "value" ) ) + "'\n" );
          }
        }
      }
      else //raster layer
      {
        QDomNodeList attributeNodeList = layerElem.elementsByTagName( QStringLiteral( "Attribute" ) );
        for ( int j = 0; j < attributeNodeList.size(); ++j )
        {
          QDomElement attributeElement = attributeNodeList.at( j ).toElement();
          featureInfoString.append( attributeElement.attribute( QStringLiteral( "name" ) ) + " = '" +
                                    attributeElement.attribute( QStringLiteral( "value" ) ) + "'\n" );
        }
      }

      featureInfoString.append( "\n" );
    }

    return featureInfoString.toUtf8();
  }

  QDomElement QgsRenderer::createFeatureGML(
    QgsFeature *feat,
    QgsVectorLayer *layer,
    QDomDocument &doc,
    QgsCoordinateReferenceSystem &crs,
    const QgsMapSettings &mapSettings,
    const QString &typeName,
    bool withGeom,
    int version,
    QStringList *attributes ) const
  {
    //qgs:%TYPENAME%
    QDomElement typeNameElement = doc.createElement( "qgs:" + typeName /*qgs:%TYPENAME%*/ );
    typeNameElement.setAttribute( QStringLiteral( "fid" ), typeName + "." + QString::number( feat->id() ) );

    QgsCoordinateTransform transform;
    if ( layer && layer->crs() != crs )
    {
      transform = mapSettings.layerTransform( layer );
    }

    QgsGeometry geom = feat->geometry();

    QgsExpressionContext expressionContext;
    expressionContext << QgsExpressionContextUtils::globalScope()
                      << QgsExpressionContextUtils::projectScope( mProject );
    if ( layer )
      expressionContext << QgsExpressionContextUtils::layerScope( layer );
    expressionContext.setFeature( *feat );

    // always add bounding box info if feature contains geometry
    if ( !geom.isNull() && geom.type() != QgsWkbTypes::UnknownGeometry && geom.type() != QgsWkbTypes::NullGeometry )
    {
      QgsRectangle box = feat->geometry().boundingBox();
      if ( transform.isValid() )
      {
        try
        {
          QgsRectangle transformedBox = transform.transformBoundingBox( box );
          box = transformedBox;
        }
        catch ( QgsCsException &e )
        {
          QgsMessageLog::logMessage( QStringLiteral( "Transform error caught: %1" ).arg( e.what() ) );
        }
      }

      QDomElement bbElem = doc.createElement( QStringLiteral( "gml:boundedBy" ) );
      QDomElement boxElem;
      if ( version < 3 )
      {
        boxElem = QgsOgcUtils::rectangleToGMLBox( &box, doc, getWMSPrecision() );
      }
      else
      {
        boxElem = QgsOgcUtils::rectangleToGMLEnvelope( &box, doc, getWMSPrecision() );
      }

      if ( crs.isValid() )
      {
        boxElem.setAttribute( QStringLiteral( "srsName" ), crs.authid() );
      }
      bbElem.appendChild( boxElem );
      typeNameElement.appendChild( bbElem );
    }

    if ( withGeom && !geom.isNull() )
    {
      //add geometry column (as gml)

      if ( transform.isValid() )
      {
        geom.transform( transform );
      }

      QDomElement geomElem = doc.createElement( QStringLiteral( "qgs:geometry" ) );
      QDomElement gmlElem;
      if ( version < 3 )
      {
        gmlElem = QgsOgcUtils::geometryToGML( geom, doc, getWMSPrecision() );
      }
      else
      {
        gmlElem = QgsOgcUtils::geometryToGML( geom, doc, QStringLiteral( "GML3" ), getWMSPrecision() );
      }

      if ( !gmlElem.isNull() )
      {
        if ( crs.isValid() )
        {
          gmlElem.setAttribute( QStringLiteral( "srsName" ), crs.authid() );
        }
        geomElem.appendChild( gmlElem );
        typeNameElement.appendChild( geomElem );
      }
    }

    //read all allowed attribute values from the feature
    QgsAttributes featureAttributes = feat->attributes();
    QgsFields fields = feat->fields();
    for ( int i = 0; i < fields.count(); ++i )
    {
      QString attributeName = fields.at( i ).name();
      //skip attribute if it is explicitly excluded from WMS publication
      if ( layer && layer->excludeAttributesWms().contains( attributeName ) )
      {
        continue;
      }
      //skip attribute if it is excluded by access control
      if ( attributes && !attributes->contains( attributeName ) )
      {
        continue;
      }

      QDomElement fieldElem = doc.createElement( "qgs:" + attributeName.replace( QStringLiteral( " " ), QStringLiteral( "_" ) ) );
      QString fieldTextString = featureAttributes.at( i ).toString();
      if ( layer )
      {
        fieldTextString = replaceValueMapAndRelation( layer, i, QgsExpression::replaceExpressionText( fieldTextString, &expressionContext ) );
      }
      QDomText fieldText = doc.createTextNode( fieldTextString );
      fieldElem.appendChild( fieldText );
      typeNameElement.appendChild( fieldElem );
    }

    //add maptip attribute based on html/expression (in case there is no maptip attribute)
    if ( layer )
    {
      QString mapTip = layer->mapTipTemplate();

      if ( !mapTip.isEmpty() )
      {
        QString fieldTextString = QgsExpression::replaceExpressionText( mapTip, &expressionContext );
        QDomElement fieldElem = doc.createElement( QStringLiteral( "qgs:maptip" ) );
        QDomText maptipText = doc.createTextNode( fieldTextString );
        fieldElem.appendChild( maptipText );
        typeNameElement.appendChild( fieldElem );
      }
    }

    return typeNameElement;
  }

  QString QgsRenderer::replaceValueMapAndRelation( QgsVectorLayer *vl, int idx, const QString &attributeVal )
  {
    const QgsEditorWidgetSetup setup = vl->editorWidgetSetup( idx );
    QgsFieldFormatter *fieldFormatter = QgsApplication::fieldFormatterRegistry()->fieldFormatter( setup.type() );
    QString value( fieldFormatter->representValue( vl, idx, setup.config(), QVariant(), attributeVal ) );

    if ( setup.config().value( QStringLiteral( "AllowMulti" ) ).toBool() && value.startsWith( QLatin1String( "{" ) ) && value.endsWith( QLatin1String( "}" ) ) )
    {
      value = value.mid( 1, value.size() - 2 );
    }
    return value;
  }

  int QgsRenderer::getImageQuality() const
  {

    // First taken from QGIS project
    int imageQuality = QgsServerProjectUtils::wmsImageQuality( *mProject );

    // Then checks if a parameter is given, if so use it instead
    if ( mParameters.contains( QStringLiteral( "IMAGE_QUALITY" ) ) )
    {
      bool conversionSuccess;
      int imageQualityParameter;
      imageQualityParameter = mParameters[ QStringLiteral( "IMAGE_QUALITY" )].toInt( &conversionSuccess );
      if ( conversionSuccess )
      {
        imageQuality = imageQualityParameter;
      }
    }
    return imageQuality;
  }

  int QgsRenderer::getWMSPrecision() const
  {
    // First taken from QGIS project and the default value is 6
    int WMSPrecision = QgsServerProjectUtils::wmsFeatureInfoPrecision( *mProject );

    // Then checks if a parameter is given, if so use it instead
    int WMSPrecisionParameter = mWmsParameters.wmsPrecisionAsInt();

    if ( WMSPrecisionParameter > -1 )
      return WMSPrecisionParameter;
    else
      return WMSPrecision;
  }

  QgsRectangle QgsRenderer::featureInfoSearchRect( QgsVectorLayer *ml, const QgsMapSettings &mapSettings, const QgsRenderContext &rct, const QgsPointXY &infoPoint ) const
  {
    if ( !ml )
    {
      return QgsRectangle();
    }

    double mapUnitTolerance = 0.0;
    if ( ml->geometryType() == QgsWkbTypes::PolygonGeometry )
    {
      QMap<QString, QString>::const_iterator tolIt = mParameters.find( QStringLiteral( "FI_POLYGON_TOLERANCE" ) );
      if ( tolIt != mParameters.constEnd() )
      {
        mapUnitTolerance = tolIt.value().toInt() * rct.mapToPixel().mapUnitsPerPixel();
      }
      else
      {
        mapUnitTolerance = mapSettings.extent().width() / 400.0;
      }
    }
    else if ( ml->geometryType() == QgsWkbTypes::LineGeometry )
    {
      QMap<QString, QString>::const_iterator tolIt = mParameters.find( QStringLiteral( "FI_LINE_TOLERANCE" ) );
      if ( tolIt != mParameters.constEnd() )
      {
        mapUnitTolerance = tolIt.value().toInt() * rct.mapToPixel().mapUnitsPerPixel();
      }
      else
      {
        mapUnitTolerance = mapSettings.extent().width() / 200.0;
      }
    }
    else //points
    {
      QMap<QString, QString>::const_iterator tolIt = mParameters.find( QStringLiteral( "FI_POINT_TOLERANCE" ) );
      if ( tolIt != mParameters.constEnd() )
      {
        mapUnitTolerance = tolIt.value().toInt() * rct.mapToPixel().mapUnitsPerPixel();
      }
      else
      {
        mapUnitTolerance = mapSettings.extent().width() / 100.0;
      }
    }

    QgsRectangle mapRectangle( infoPoint.x() - mapUnitTolerance, infoPoint.y() - mapUnitTolerance,
                               infoPoint.x() + mapUnitTolerance, infoPoint.y() + mapUnitTolerance );
    return ( mapSettings.mapToLayerCoordinates( ml, mapRectangle ) );
  }


  void QgsRenderer::initRestrictedLayers()
  {
    mRestrictedLayers.clear();

    // get name of restricted layers/groups in project
    QStringList restricted = QgsServerProjectUtils::wmsRestrictedLayers( *mProject );

    // extract restricted layers from excluded groups
    QStringList restrictedLayersNames;
    QgsLayerTreeGroup *root = mProject->layerTreeRoot();

    Q_FOREACH ( QString l, restricted )
    {
      QgsLayerTreeGroup *group = root->findGroup( l );
      if ( group )
      {
        QList<QgsLayerTreeLayer *> groupLayers = group->findLayers();
        Q_FOREACH ( QgsLayerTreeLayer *treeLayer, groupLayers )
        {
          restrictedLayersNames.append( treeLayer->name() );
        }
      }
      else
      {
        restrictedLayersNames.append( l );
      }
    }

    // build output with names, ids or short name according to the configuration
    QList<QgsLayerTreeLayer *> layers = root->findLayers();
    Q_FOREACH ( QgsLayerTreeLayer *layer, layers )
    {
      if ( restrictedLayersNames.contains( layer->name() ) )
      {
        mRestrictedLayers.append( layerNickname( *layer->layer() ) );
      }
    }
  }

  void QgsRenderer::initNicknameLayers()
  {
    Q_FOREACH ( QgsMapLayer *ml, mProject->mapLayers() )
    {
      mNicknameLayers[ layerNickname( *ml ) ] = ml;
    }
  }

  QString QgsRenderer::layerNickname( const QgsMapLayer &layer ) const
  {
    QString name = layer.shortName();
    if ( QgsServerProjectUtils::wmsUseLayerIds( *mProject ) )
    {
      name = layer.id();
    }
    else if ( name.isEmpty() )
    {
      name = layer.name();
    }

    return name;
  }

  bool QgsRenderer::layerScaleVisibility( const QgsMapLayer &layer, double scaleDenominator ) const
  {
    bool visible = false;
    bool scaleBasedVisibility = layer.hasScaleBasedVisibility();
    bool useScaleConstraint = ( scaleDenominator > 0 && scaleBasedVisibility );

    if ( !useScaleConstraint || layer.isInScaleRange( scaleDenominator ) )
    {
      visible = true;
    }

    return visible;
  }

  QList<QgsMapLayer *> QgsRenderer::highlightLayers( QList<QgsWmsParametersHighlightLayer> params )
  {
    QList<QgsMapLayer *> highlightLayers;

    // try to create highlight layer for each geometry
    QString crs = mWmsParameters.crs();
    Q_FOREACH ( QgsWmsParametersHighlightLayer param, params )
    {
      // create sld document from symbology
      QDomDocument sldDoc;
      if ( !sldDoc.setContent( param.mSld, true ) )
      {
        continue;
      }

      // create renderer from sld document
      QString errorMsg;
      std::unique_ptr<QgsFeatureRenderer> renderer;
      QDomElement el = sldDoc.documentElement();
      renderer.reset( QgsFeatureRenderer::loadSld( el, param.mGeom.type(), errorMsg ) );
      if ( !renderer )
      {
        QgsMessageLog::logMessage( errorMsg, "Server", QgsMessageLog::INFO );
        continue;
      }

      // build url for vector layer
      QString typeName = QgsWkbTypes::geometryDisplayString( param.mGeom.type() );
      QString url = typeName + "?crs=" + crs;
      if ( ! param.mLabel.isEmpty() )
      {
        url += "&field=label:string";
      }

      // create vector layer
      std::unique_ptr<QgsVectorLayer> layer;
      layer.reset( new QgsVectorLayer( url, param.mName, "memory" ) );
      if ( !layer->isValid() )
      {
        continue;
      }

      // create feature with label if necessary
      QgsFeature fet( layer->pendingFields() );
      if ( ! param.mLabel.isEmpty() )
      {
        fet.setAttribute( 0, param.mLabel );

        // init labeling engine
        QgsPalLayerSettings palSettings;
        palSettings.fieldName = "label"; // defined in url
        palSettings.priority = 10; // always drawn
        palSettings.displayAll = true;

        QgsPalLayerSettings::Placement placement = QgsPalLayerSettings::AroundPoint;
        switch ( param.mGeom.type() )
        {
          case QgsWkbTypes::PointGeometry:
          {
            placement = QgsPalLayerSettings::AroundPoint;
            palSettings.dist = 2; // in mm
            palSettings.placementFlags = 0;
            break;
          }
          case QgsWkbTypes::PolygonGeometry:
          {
            QgsGeometry point = param.mGeom.pointOnSurface();
            QgsPointXY pt = point.asPoint();
            placement = QgsPalLayerSettings::AroundPoint;

            QgsPalLayerSettings::Property pX = QgsPalLayerSettings::PositionX;
            QVariant x( pt.x() );
            palSettings.dataDefinedProperties().setProperty( pX, x );

            QgsPalLayerSettings::Property pY = QgsPalLayerSettings::PositionY;
            QVariant y( pt.y() );
            palSettings.dataDefinedProperties().setProperty( pY, y );

            QgsPalLayerSettings::Property pHali = QgsPalLayerSettings::Hali;
            QVariant hali( "Center" );
            palSettings.dataDefinedProperties().setProperty( pHali, hali );

            QgsPalLayerSettings::Property pVali = QgsPalLayerSettings::Vali;
            QVariant vali( "Half" );
            palSettings.dataDefinedProperties().setProperty( pVali, vali );
            break;
          }
          default:
          {
            placement = QgsPalLayerSettings::Line;
            palSettings.dist = 2;
            palSettings.placementFlags = 10;
            break;
          }
        }
        palSettings.placement = placement;
        QgsTextFormat textFormat;
        QgsTextBufferSettings bufferSettings;

        if ( param.mColor.isValid() )
        {
          textFormat.setColor( param.mColor );
        }

        if ( param.mSize > 0 )
        {
          textFormat.setSize( param.mSize );
        }

        // no weight property in PAL settings or QgsTextFormat
        /* if ( param.fontWeight > 0 )
        {
        } */

        if ( ! param.mFont.isEmpty() )
        {
          textFormat.setFont( param.mFont );
        }

        if ( param.mBufferColor.isValid() )
        {
          bufferSettings.setColor( param.mBufferColor );
        }

        if ( param.mBufferSize > 0 )
        {
          bufferSettings.setEnabled( true );
          bufferSettings.setSize( param.mBufferSize );
        }

        textFormat.setBuffer( bufferSettings );
        palSettings.setFormat( textFormat );

        QgsVectorLayerSimpleLabeling *simpleLabeling = new QgsVectorLayerSimpleLabeling( palSettings );
        layer->setLabeling( simpleLabeling );
      }
      fet.setGeometry( param.mGeom );

      // add feature to layer and set the SLD renderer
      layer->dataProvider()->addFeatures( QgsFeatureList() << fet );
      layer->setRenderer( renderer.release() );

      // keep the vector as an highlight layer
      if ( layer->isValid() )
      {
        highlightLayers.append( layer.release() );
      }
    }

    mTemporaryLayers.append( highlightLayers );
    return highlightLayers;
  }

  QList<QgsMapLayer *> QgsRenderer::sldStylizedLayers( const QString &sld ) const
  {
    QList<QgsMapLayer *> layers;

    if ( !sld.isEmpty() )
    {
      QDomDocument doc;
      ( void )doc.setContent( sld, true );
      QDomElement docEl = doc.documentElement();

      QDomElement root = doc.firstChildElement( "StyledLayerDescriptor" );
      QDomElement namedElem = root.firstChildElement( "NamedLayer" );

      if ( !docEl.isNull() )
      {
        QDomNodeList named = docEl.elementsByTagName( "NamedLayer" );
        for ( int i = 0; i < named.size(); ++i )
        {
          QDomNodeList names = named.item( i ).toElement().elementsByTagName( "Name" );
          if ( !names.isEmpty() )
          {
            QString lname = names.item( 0 ).toElement().text();
            QString err;
            if ( mNicknameLayers.contains( lname ) && !mRestrictedLayers.contains( lname ) )
            {
              mNicknameLayers[lname]->readSld( namedElem, err );
              mNicknameLayers[lname]->setCustomProperty( "readSLD", true );
              layers.append( mNicknameLayers[lname] );
            }
            else
            {
              throw QgsBadRequestException( QStringLiteral( "LayerNotDefined" ),
                                            QStringLiteral( "Layer \"%1\" does not exist" ).arg( lname ) );
            }
          }
        }
      }
    }

    return layers;
  }

  QList<QgsMapLayer *> QgsRenderer::stylizedLayers( const QList<QgsWmsParametersLayer> &params )
  {
    QList<QgsMapLayer *> layers;

    Q_FOREACH ( QgsWmsParametersLayer param, params )
    {
      QString nickname = param.mNickname;
      QString style = param.mStyle;
      if ( nickname.startsWith( "EXTERNAL_WMS:" ) )
      {
        QString externalLayerId = nickname;
        externalLayerId.remove( 0, 13 );
        QgsMapLayer *externalWMSLayer = createExternalWMSLayer( externalLayerId );
        if ( externalWMSLayer )
        {
          layers.append( externalWMSLayer );
          mNicknameLayers[nickname] = externalWMSLayer; //might be used later in GetPrint request
          mTemporaryLayers.append( externalWMSLayer );
        }
      }
      else if ( mNicknameLayers.contains( nickname ) && !mRestrictedLayers.contains( nickname ) )
      {
        if ( !style.isEmpty() )
        {
          bool rc = mNicknameLayers[nickname]->styleManager()->setCurrentStyle( style );
          if ( ! rc )
          {
            throw QgsMapServiceException( QStringLiteral( "StyleNotDefined" ), QStringLiteral( "Style \"%1\" does not exist for layer \"%2\"" ).arg( style, nickname ) );
          }
        }

        layers.append( mNicknameLayers[nickname] );
      }
      else
      {
        throw QgsBadRequestException( QStringLiteral( "LayerNotDefined" ),
                                      QStringLiteral( "Layer \"%1\" does not exist" ).arg( nickname ) );
      }
    }

    return layers;
  }

  QgsMapLayer *QgsRenderer::createExternalWMSLayer( const QString &externalLayerId ) const
  {
    QString wmsUri = mWmsParameters.externalWMSUri( externalLayerId.toUpper() );
    QgsMapLayer *wmsLayer = new QgsRasterLayer( wmsUri, externalLayerId, QStringLiteral( "wms" ) );
    if ( !wmsLayer->isValid() )
    {
      delete wmsLayer;
      return 0;
    }

    return wmsLayer;
  }

  void QgsRenderer::removeTemporaryLayers()
  {
    qDeleteAll( mTemporaryLayers );
    mTemporaryLayers.clear();
  }

  QPainter *QgsRenderer::layersRendering( const QgsMapSettings &mapSettings, QImage &image, HitTest *hitTest ) const
  {
    QPainter *painter = nullptr;
    if ( hitTest )
    {
      runHitTest( mapSettings, *hitTest );
      painter = new QPainter();
    }
    else
    {
#ifdef HAVE_SERVER_PYTHON_PLUGINS
      mAccessControl->resolveFilterFeatures( mapSettings.layers() );
#endif
      QgsMapRendererJobProxy renderJob( mSettings.parallelRendering(), mSettings.maxThreads(), mAccessControl );
      renderJob.render( mapSettings, &image );
      painter = renderJob.takePainter();
    }

    return painter;
  }

  void QgsRenderer::setLayerOpacity( QgsMapLayer *layer, int opacity ) const
  {
    if ( opacity >= 0 && opacity <= 255 )
    {
      if ( layer->type() == QgsMapLayer::LayerType::VectorLayer )
      {
        QgsVectorLayer *vl = qobject_cast<QgsVectorLayer *>( layer );
        vl->setOpacity( opacity / 255. );
      }
      else if ( layer->type() == QgsMapLayer::LayerType::RasterLayer )
      {
        QgsRasterLayer *rl = qobject_cast<QgsRasterLayer *>( layer );
        QgsRasterRenderer *rasterRenderer = rl->renderer();
        rasterRenderer->setOpacity( opacity / 255. );
      }
    }
  }

  void QgsRenderer::setLayerFilter( QgsMapLayer *layer, const QStringList &filters ) const
  {
    if ( layer->type() == QgsMapLayer::VectorLayer )
    {
      QgsVectorLayer *filteredLayer = qobject_cast<QgsVectorLayer *>( layer );
      Q_FOREACH ( QString filter, filters )
      {
        if ( !testFilterStringSafety( filter ) )
        {
          throw QgsBadRequestException( QStringLiteral( "Filter string rejected" ),
                                        QStringLiteral( "The filter string %1"
                                            " has been rejected because of security reasons."
                                            " Note: Text strings have to be enclosed in single or double quotes."
                                            " A space between each word / special character is mandatory."
                                            " Allowed Keywords and special characters are "
                                            " AND,OR,IN,<,>=,>,>=,!=,',',(,),DMETAPHONE,SOUNDEX."
                                            " Not allowed are semicolons in the filter expression." ).arg( filter ) );
        }

        QString newSubsetString = filter;
        if ( !filteredLayer->subsetString().isEmpty() )
        {
          newSubsetString.prepend( ") AND (" );
          newSubsetString.append( ")" );
          newSubsetString.prepend( filteredLayer->subsetString() );
          newSubsetString.prepend( "(" );
        }
        filteredLayer->setSubsetString( newSubsetString );
      }
    }
  }

  void QgsRenderer::setLayerSelection( QgsMapLayer *layer, const QStringList &fids ) const
  {
    if ( layer->type() == QgsMapLayer::VectorLayer )
    {
      QgsFeatureIds selectedIds;

      Q_FOREACH ( const QString &id, fids )
      {
        selectedIds.insert( STRING_TO_FID( id ) );
      }

      QgsVectorLayer *vl = qobject_cast<QgsVectorLayer *>( layer );
      vl->selectByIds( selectedIds );
    }
  }

  void QgsRenderer::setLayerAccessControlFilter( QgsMapLayer *layer ) const
  {
#ifdef HAVE_SERVER_PYTHON_PLUGINS
    QgsOWSServerFilterRestorer::applyAccessControlLayerFilters( mAccessControl, layer );
#else
    Q_UNUSED( layer );
#endif
  }

  void QgsRenderer::updateExtent( const QgsMapLayer *layer, QgsMapSettings &mapSettings ) const
  {
    QgsRectangle layerExtent = mapSettings.layerToMapCoordinates( layer, layer->extent() );
    QgsRectangle mapExtent = mapSettings.extent();
    if ( !layerExtent.isEmpty() )
    {
      mapExtent.combineExtentWith( layerExtent );
      mapSettings.setExtent( mapExtent );
    }
  }

  void QgsRenderer::annotationsRendering( QPainter *painter ) const
  {
    const QgsAnnotationManager *annotationManager = mProject->annotationManager();
    QList< QgsAnnotation * > annotations = annotationManager->annotations();

    QgsRenderContext renderContext = QgsRenderContext::fromQPainter( painter );
    Q_FOREACH ( QgsAnnotation *annotation, annotations )
    {
      annotation->render( renderContext );
    }
  }

  QImage *QgsRenderer::scaleImage( const QImage *image ) const
  {
    //test if width / height ratio of image is the same as the ratio of
    // WIDTH / HEIGHT parameters. If not, the image has to be scaled (required
    // by WMS spec)
    QImage *scaledImage = nullptr;
    int width = mWmsParameters.widthAsInt();
    int height = mWmsParameters.heightAsInt();
    if ( width != image->width() || height != image->height() )
    {
      scaledImage = new QImage( image->scaled( width, height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation ) );
    }

    return scaledImage;
  }

  void QgsRenderer::checkLayerReadPermissions( QgsMapLayer *layer ) const
  {
#ifdef HAVE_SERVER_PYTHON_PLUGINS
    if ( !mAccessControl->layerReadPermission( layer ) )
    {
      throw QgsSecurityException( QStringLiteral( "You are not allowed to access to the layer: %1" ).arg( layer->name() ) );
    }
#else
    Q_UNUSED( layer );
#endif
  }

  void QgsRenderer::removeUnwantedLayers( QList<QgsMapLayer *> &layers, double scaleDenominator ) const
  {
    QList<QgsMapLayer *> wantedLayers;

    Q_FOREACH ( QgsMapLayer *layer, layers )
    {
      if ( !layerScaleVisibility( *layer, scaleDenominator ) )
        continue;

      if ( mRestrictedLayers.contains( layerNickname( *layer ) ) )
        continue;

      wantedLayers.append( layer );
    }

    layers = wantedLayers;
  }

  void QgsRenderer::removeNonIdentifiableLayers( QList<QgsMapLayer *> &layers ) const
  {
    QStringList nonIdentifiableLayers = mProject->nonIdentifiableLayers();
    if ( !nonIdentifiableLayers.isEmpty() )
    {
      QList<QgsMapLayer *> wantedLayers;

      Q_FOREACH ( QgsMapLayer *layer, layers )
      {
        if ( nonIdentifiableLayers.contains( layer->id() ) )
          continue;

        wantedLayers.append( layer );
      }

      layers = wantedLayers;
    }
  }

  QgsLayerTreeModel *QgsRenderer::buildLegendTreeModel( const QList<QgsMapLayer *> &layers, double scaleDenominator, QgsLayerTree &rootGroup )
  {
    // get params
    bool showFeatureCount = mWmsParameters.showFeatureCountAsBool();
    bool drawLegendLayerLabel = mWmsParameters.layerTitleAsBool();
    bool drawLegendItemLabel = mWmsParameters.ruleLabelAsBool();

    bool ruleDefined = false;
    if ( !mWmsParameters.rule().isEmpty() )
      ruleDefined = true;

    bool contentBasedLegend = false;
    QgsRectangle contentBasedLegendExtent;
    if ( ! mWmsParameters.bbox().isEmpty() )
    {
      contentBasedLegend = true;
      contentBasedLegendExtent = mWmsParameters.bboxAsRectangle();
      if ( contentBasedLegendExtent.isEmpty() )
        throw QgsBadRequestException( QStringLiteral( "InvalidParameterValue" ),
                                      QStringLiteral( "Invalid BBOX parameter" ) );

      if ( !mWmsParameters.rule().isEmpty() )
        throw QgsBadRequestException( QStringLiteral( "InvalidParameterValue" ),
                                      QStringLiteral( "BBOX parameter cannot be combined with RULE" ) );
    }

    // build layer tree
    rootGroup.clear();
    QList<QgsVectorLayerFeatureCounter *> counters;
    Q_FOREACH ( QgsMapLayer *ml, layers )
    {
      QgsLayerTreeLayer *lt = rootGroup.addLayer( ml );
      lt->setCustomProperty( QStringLiteral( "showFeatureCount" ), showFeatureCount );

      if ( !ml->title().isEmpty() )
        lt->setName( ml->title() );

      if ( ml->type() != QgsMapLayer::VectorLayer || !showFeatureCount )
        continue;

      QgsVectorLayer *vl = qobject_cast<QgsVectorLayer *>( ml );
      QgsVectorLayerFeatureCounter *counter = vl->countSymbolFeatures();
      if ( !counter )
        continue;
      counters.append( counter );
    }

    // build legend model
    QgsLayerTreeModel *legendModel = new QgsLayerTreeModel( &rootGroup );
    if ( scaleDenominator > 0 )
      legendModel->setLegendFilterByScale( scaleDenominator );

    QgsMapSettings contentBasedMapSettings;
    if ( contentBasedLegend )
    {
      HitTest hitTest;
      getMap( contentBasedMapSettings, &hitTest );

      Q_FOREACH ( QgsLayerTreeNode *node, rootGroup.children() )
      {
        Q_ASSERT( QgsLayerTree::isLayer( node ) );
        QgsLayerTreeLayer *nodeLayer = QgsLayerTree::toLayer( node );

        QgsVectorLayer *vl = qobject_cast<QgsVectorLayer *>( nodeLayer->layer() );
        if ( !vl || !vl->renderer() )
          continue;

        const SymbolSet &usedSymbols = hitTest[vl];
        QList<int> order;
        int i = 0;
        Q_FOREACH ( const QgsLegendSymbolItem &legendItem, vl->renderer()->legendSymbolItems() )
        {
          QString sProp = QgsSymbolLayerUtils::symbolProperties( legendItem.legacyRuleKey() );
          if ( usedSymbols.contains( sProp ) )
            order.append( i );
          ++i;
        }

        // either remove the whole layer or just filter out some items
        if ( order.isEmpty() )
          rootGroup.removeChildNode( nodeLayer );
        else
        {
          QgsMapLayerLegendUtils::setLegendNodeOrder( nodeLayer, order );
          legendModel->refreshLayerLegend( nodeLayer );
        }
      }
    }

    // if legend is not based on rendering rules
    if ( ! ruleDefined )
    {
      QList<QgsLayerTreeNode *> rootChildren = rootGroup.children();
      Q_FOREACH ( QgsLayerTreeNode *node, rootChildren )
      {
        if ( QgsLayerTree::isLayer( node ) )
        {
          QgsLayerTreeLayer *nodeLayer = QgsLayerTree::toLayer( node );

          // layer titles - hidden or not
          QgsLegendRenderer::setNodeLegendStyle( nodeLayer, drawLegendLayerLabel ? QgsLegendStyle::Subgroup : QgsLegendStyle::Hidden );

          // rule item titles
          if ( !drawLegendItemLabel )
          {
            Q_FOREACH ( QgsLayerTreeModelLegendNode *legendNode, legendModel->layerLegendNodes( nodeLayer ) )
            {
              legendNode->setUserLabel( QStringLiteral( " " ) ); // empty string = no override, so let's use one space
            }
          }
          else if ( !drawLegendLayerLabel )
          {
            Q_FOREACH ( QgsLayerTreeModelLegendNode *legendNode, legendModel->layerLegendNodes( nodeLayer ) )
            {
              if ( legendNode->isEmbeddedInParent() )
                legendNode->setEmbeddedInParent( false );
            }
          }
        }
      }
    }

    for ( QgsVectorLayerFeatureCounter *c : counters )
    {
      c->waitForFinished();
    }

    return legendModel;
  }

  qreal QgsRenderer::dotsPerMm() const
  {
    std::unique_ptr<QImage> tmpImage( createImage( 1, 1, false ) );
    return tmpImage->dotsPerMeterX() / 1000.0;
  }
} // namespace QgsWms
