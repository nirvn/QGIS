/************************************************************************
 * This file has been generated automatically from                      *
 *                                                                      *
 * src/core/raster/qgsrasterdataprovider.h                              *
 *                                                                      *
 * Do not edit manually ! Edit header and run scripts/sipify.pl again   *
 ************************************************************************/









class QgsImageFetcher : QObject
{
%Docstring
 Handles asynchronous download of images
.. versionadded:: 2.8
%End

%TypeHeaderCode
#include "qgsrasterdataprovider.h"
%End
  public:
    QgsImageFetcher( QObject *parent = 0 );
%Docstring
Constructor
%End

    virtual void start() = 0;
%Docstring
 Starts the image download
.. note::

   Make sure to connect to "finish" and "error" before starting *
%End

  signals:

    void finish( const QImage &legend );
%Docstring
 Emitted when the download completes
  \param legend The downloaded legend image *
%End
    void progress( qint64 received, qint64 total );
%Docstring
Emitted to report progress
%End
    void error( const QString &msg );
%Docstring
Emitted when an error occurs
%End
};


class QgsRasterDataProvider : QgsDataProvider, QgsRasterInterface
{
%Docstring
 Base class for raster data providers.
%End

%TypeHeaderCode
#include "qgsrasterdataprovider.h"
%End
  public:
    QgsRasterDataProvider();

    QgsRasterDataProvider( const QString &uri );

    virtual QgsRasterInterface *clone() const = 0;

    virtual bool setInput( QgsRasterInterface *input );
%Docstring
 It makes no sense to set input on provider */
 :rtype: bool
%End

    virtual QgsRectangle extent() const = 0;

    virtual Qgis::DataType dataType( int bandNo ) const = 0;
%Docstring
Returns data type for the band specified by number
 :rtype: Qgis.DataType
%End

    virtual Qgis::DataType sourceDataType( int bandNo ) const = 0;
%Docstring
 Returns source data type for the band specified by number,
  source data type may be shorter than dataType
 :rtype: Qgis.DataType
%End

    virtual int colorInterpretation( int bandNo ) const;
%Docstring
Returns data type for the band specified by number
 :rtype: int
%End

    QString colorName( int colorInterpretation ) const;
%Docstring
 :rtype: str
%End
    virtual bool reload();
%Docstring
Reload data (data could change)
 :rtype: bool
%End

    virtual QString colorInterpretationName( int bandNo ) const;
%Docstring
 :rtype: str
%End

    virtual double bandScale( int bandNo ) const;
%Docstring
 Read band scale for raster value
.. versionadded:: 2.3
 :rtype: float
%End

    virtual double bandOffset( int bandNo ) const;
%Docstring
 Read band offset for raster value
.. versionadded:: 2.3
 :rtype: float
%End


    virtual QgsRasterBlock *block( int bandNo, const QgsRectangle &boundingBox, int width, int height, QgsRasterBlockFeedback *feedback = 0 );
%Docstring
Read block of data using given extent and size.
 :rtype: QgsRasterBlock
%End

    virtual bool sourceHasNoDataValue( int bandNo ) const;
%Docstring
Return true if source band has no data value
 :rtype: bool
%End

    virtual bool useSourceNoDataValue( int bandNo ) const;
%Docstring
 Get source nodata value usage
 :rtype: bool
%End

    virtual void setUseSourceNoDataValue( int bandNo, bool use );
%Docstring
 Set source nodata value usage
%End

    virtual double sourceNoDataValue( int bandNo ) const;
%Docstring
Value representing no data value.
 :rtype: float
%End

    virtual void setUserNoDataValue( int bandNo, const QgsRasterRangeList &noData );

    virtual QgsRasterRangeList userNoDataValues( int bandNo ) const;
%Docstring
Get list of user no data value ranges
 :rtype: QgsRasterRangeList
%End

    virtual QList<QgsColorRampShader::ColorRampItem> colorTable( int bandNo ) const;
%Docstring
 :rtype: list of QgsColorRampShader.ColorRampItem
%End

    virtual QStringList subLayers() const;
%Docstring
 Returns the sublayers of this layer - useful for providers that manage
  their own layers, such as WMS *
 :rtype: list of str
%End

    virtual bool supportsLegendGraphic() const;
%Docstring
 Returns whether the provider supplies a legend graphic
 :rtype: bool
%End


    virtual QgsImageFetcher *getLegendGraphicFetcher( const QgsMapSettings *mapSettings ) /Factory/;
%Docstring
 Get an image downloader for the raster legend

 \param mapSettings map settings for legend providers supporting
                    contextual legends.

 :return: a download handler or null if the provider does not support
         legend at all. Ownership of the returned object is transferred
         to caller.

.. versionadded:: 2.8
 :rtype: QgsImageFetcher
%End

    virtual QString buildPyramids( const QList<QgsRasterPyramid> &pyramidList,
                                   const QString &resamplingMethod = "NEAREST",
                                   QgsRaster::RasterPyramidsFormat format = QgsRaster::PyramidsGTiff,
                                   const QStringList &configOptions = QStringList(),
                                   QgsRasterBlockFeedback *feedback = 0 );
%Docstring
 Create pyramid overviews
 :rtype: str
%End

    virtual QList<QgsRasterPyramid> buildPyramidList( QList<int> overviewList = QList<int>() );
%Docstring
 Accessor for the raster layers pyramid list.
 \param overviewList used to construct the pyramid list (optional), when empty the list is defined by the provider.
 A pyramid list defines the
 POTENTIAL pyramids that can be in a raster. To know which of the pyramid layers
 ACTUALLY exists you need to look at the existsFlag member in each struct stored in the
 list.
 :rtype: list of QgsRasterPyramid
%End

    bool hasPyramids();
%Docstring
 Returns true if raster has at least one populated histogram.
 :rtype: bool
%End

    virtual QString htmlMetadata() = 0;
%Docstring
 Get metadata in a format suitable for feeding directly
 into a subset of the GUI raster properties "Metadata" tab.
 :rtype: str
%End

    virtual QgsRasterIdentifyResult identify( const QgsPointXY &point, QgsRaster::IdentifyFormat format, const QgsRectangle &boundingBox = QgsRectangle(), int width = 0, int height = 0, int dpi = 96 );
%Docstring
 :rtype: QgsRasterIdentifyResult
%End

    virtual QString lastErrorTitle() = 0;
%Docstring
   Returns the caption error text for the last error in this provider

 If an operation returns 0 (e.g. draw()), this function
 returns the text of the error associated with the failure.
 Interactive users of this provider can then, for example,
 call a QMessageBox to display the contents.
 :rtype: str
%End

    virtual QString lastError() = 0;
%Docstring
   Returns the verbose error text for the last error in this provider

 If an operation returns 0 (e.g. draw()), this function
 returns the text of the error associated with the failure.
 Interactive users of this provider can then, for example,
 call a QMessageBox to display the contents.
 :rtype: str
%End

    virtual QString lastErrorFormat();
%Docstring
Returns the format of the error text for the last error in this provider
 :rtype: str
%End

    int dpi() const;
%Docstring
Returns the dpi of the output device.
 :rtype: int
%End

    void setDpi( int dpi );
%Docstring
Sets the output device resolution.
%End

    virtual QDateTime timestamp() const;
%Docstring
Time stamp of data source in the moment when data/metadata were loaded by provider
 :rtype: QDateTime
%End

    virtual QDateTime dataTimestamp() const;
%Docstring
Current time stamp of data source
 :rtype: QDateTime
%End

    virtual bool isEditable() const;
%Docstring
 Checks whether the provider is in editing mode, i.e. raster write operations will be accepted.
 By default providers are not editable. Use setEditable() method to enable/disable editing.
.. seealso:: setEditable(), writeBlock()
.. versionadded:: 3.0
 :rtype: bool
%End

    virtual bool setEditable( bool enabled );
%Docstring
 Turns on/off editing mode of the provider. When in editing mode, it is possible
 to overwrite data of the provider using writeBlock() calls.
.. note::

   Only some providers support editing mode and even those may fail to turn
 the underlying data source into editing mode, so it is necessary to check the return
 value whether the operation was successful.
 :return: true if the switch to/from editing mode was successful
.. seealso:: isEditable(), writeBlock()
.. versionadded:: 3.0
 :rtype: bool
%End

    virtual bool write( void *data, int band, int width, int height, int xOffset, int yOffset );
%Docstring
 :rtype: bool
%End

    bool writeBlock( QgsRasterBlock *block, int band, int xOffset = 0, int yOffset = 0 );
%Docstring
 Writes pixel data from a raster block into the provider data source.

 This will override previously stored pixel values. It is assumed that cells in the passed
 raster block are aligned with the cells of the data source. If raster block does not cover
 the whole area of the data source, only a subset of pixels covered by the raster block
 will be overwritten. By default, writing of raster data starts from the first cell
 of the raster - it is possible to set offset in pixels by specifying non-zero
 xOffset and yOffset values.

 Writing is supported only by some data providers. Provider has to be in editing mode
 in order to allow write operations.
.. seealso:: isEditable(), setEditable()
 :return: true on success
.. versionadded:: 3.0
 :rtype: bool
%End

    static QgsRasterDataProvider *create( const QString &providerKey,
                                          const QString &uri,
                                          const QString &format, int nBands,
                                          Qgis::DataType type,
                                          int width, int height, double *geoTransform,
                                          const QgsCoordinateReferenceSystem &crs,
                                          const QStringList &createOptions = QStringList() );
%Docstring
Creates a new dataset with mDataSourceURI
 :rtype: QgsRasterDataProvider
%End

    virtual bool setNoDataValue( int bandNo, double noDataValue );
%Docstring
 Set no data value on created dataset
  \param bandNo band number
  \param noDataValue no data value
 :rtype: bool
%End

    virtual bool remove();
%Docstring
Remove dataset
 :rtype: bool
%End

    static QList<QPair<QString, QString> > pyramidResamplingMethods( const QString &providerKey );
%Docstring
 Returns a list of pyramid resampling method name and label pairs
 for given provider
 :rtype: list of QPair<str, QString>
%End

    virtual QString validateCreationOptions( const QStringList &createOptions, const QString &format );
%Docstring
 Validates creation options for a specific dataset and destination format.
.. note::

   used by GDAL provider only
.. note::

   see also validateCreationOptionsFormat() in gdal provider for validating options based on format only
 :rtype: str
%End

    virtual QString validatePyramidsConfigOptions( QgsRaster::RasterPyramidsFormat pyramidsFormat,
        const QStringList &configOptions, const QString &fileFormat );
%Docstring
 Validates pyramid creation options for a specific dataset and destination format
.. note::

   used by GDAL provider only
 :rtype: str
%End

    static QString identifyFormatName( QgsRaster::IdentifyFormat format );
%Docstring
 :rtype: str
%End
    static QgsRaster::IdentifyFormat identifyFormatFromName( const QString &formatName );
%Docstring
 :rtype: QgsRaster.IdentifyFormat
%End
    static QString identifyFormatLabel( QgsRaster::IdentifyFormat format );
%Docstring
 :rtype: str
%End
    static Capability identifyFormatToCapability( QgsRaster::IdentifyFormat format );
%Docstring
 :rtype: Capability
%End

    virtual int stepWidth() const;
%Docstring
 Step width for raster iterations.
.. seealso:: stepHeight()
.. versionadded:: 3.0
 :rtype: int
%End

    virtual int stepHeight() const;
%Docstring
 Step height for raster iterations.
.. seealso:: stepWidth()
.. versionadded:: 3.0
 :rtype: int
%End

  signals:

    void statusChanged( const QString & ) const;
%Docstring
 Emit a message to be displayed on status bar, usually used by network providers (WMS,WCS)
.. versionadded:: 2.14
%End

  protected:



    bool userNoDataValuesContains( int bandNo, double value ) const;
%Docstring
Returns true if user no data contains value
 :rtype: bool
%End

    void copyBaseSettings( const QgsRasterDataProvider &other );
%Docstring
Copy member variables from other raster data provider. Useful for implementation of clone() method in subclasses
%End


    static QString makeTableCell( const QString &value );
%Docstring
 :rtype: str
%End
    static QString makeTableCells( const QStringList &values );
%Docstring
 :rtype: str
%End








};


/************************************************************************
 * This file has been generated automatically from                      *
 *                                                                      *
 * src/core/raster/qgsrasterdataprovider.h                              *
 *                                                                      *
 * Do not edit manually ! Edit header and run scripts/sipify.pl again   *
 ************************************************************************/
