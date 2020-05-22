/***************************************************************************
                         qgstemporalcontrollerwidget.cpp
                         ------------------------------
    begin                : February 2020
    copyright            : (C) 2020 by Samweli Mwakisambwe
    email                : samweli at kartoza dot com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgstemporalcontrollerwidget.h"
#include "qgsgui.h"
#include "qgsmaplayermodel.h"
#include "qgsproject.h"
#include "qgsprojecttimesettings.h"
#include "qgstemporalmapsettingswidget.h"
#include "qgstemporalutils.h"
#include "qgsmaplayertemporalproperties.h"

#include <QAction>
#include <QMenu>

QgsTemporalControllerWidget::QgsTemporalControllerWidget( QWidget *parent )
  : QgsPanelWidget( parent )
{
  setupUi( this );

  mNavigationObject = new QgsTemporalNavigationObject( this );

  connect( mForwardButton, &QPushButton::clicked, this, &QgsTemporalControllerWidget::togglePlayForward );
  connect( mBackButton, &QPushButton::clicked, this, &QgsTemporalControllerWidget::togglePlayBackward );
  connect( mStopButton, &QPushButton::clicked, this, &QgsTemporalControllerWidget::togglePause );
  connect( mNextButton, &QPushButton::clicked, mNavigationObject, &QgsTemporalNavigationObject::next );
  connect( mPreviousButton, &QPushButton::clicked, mNavigationObject, &QgsTemporalNavigationObject::previous );
  connect( mFastForwardButton, &QPushButton::clicked, mNavigationObject, &QgsTemporalNavigationObject::skipToEnd );
  connect( mRewindButton, &QPushButton::clicked, mNavigationObject, &QgsTemporalNavigationObject::rewindToStart );
  connect( mLoopingCheckBox, &QCheckBox::toggled, this, [ = ]( bool state ) { mNavigationObject->setLooping( state ); } );

  setWidgetStateFromNavigationMode( mNavigationObject->navigationMode() );
  connect( mNavigationObject, &QgsTemporalNavigationObject::navigationModeChanged, this, &QgsTemporalControllerWidget::setWidgetStateFromNavigationMode );
  connect( mNavigationOff, &QPushButton::clicked, this, &QgsTemporalControllerWidget::mNavigationOff_clicked );
  connect( mNavigationFixedRange, &QPushButton::clicked, this, &QgsTemporalControllerWidget::mNavigationFixedRange_clicked );
  connect( mNavigationAnimated, &QPushButton::clicked, this, &QgsTemporalControllerWidget::mNavigationAnimated_clicked );

  connect( mNavigationObject, &QgsTemporalNavigationObject::stateChanged, this, [ = ]( QgsTemporalNavigationObject::AnimationState state )
  {
    mForwardButton->setChecked( state == QgsTemporalNavigationObject::Forward );
    mBackButton->setChecked( state == QgsTemporalNavigationObject::Reverse );
    mStopButton->setChecked( state == QgsTemporalNavigationObject::Idle );
  } );

  connect( mStartDateTime, &QDateTimeEdit::dateTimeChanged, this, &QgsTemporalControllerWidget::startEndDateTime_changed );
  connect( mEndDateTime, &QDateTimeEdit::dateTimeChanged, this, &QgsTemporalControllerWidget::startEndDateTime_changed );
  connect( mFixedRangeStartDateTime, &QDateTimeEdit::dateTimeChanged, this, &QgsTemporalControllerWidget::fixedRangeStartEndDateTime_changed );
  connect( mFixedRangeEndDateTime, &QDateTimeEdit::dateTimeChanged, this, &QgsTemporalControllerWidget::fixedRangeStartEndDateTime_changed );
  connect( mStepSpinBox, qgis::overload<double>::of( &QDoubleSpinBox::valueChanged ), this, &QgsTemporalControllerWidget::updateFrameDuration );
  connect( mTimeStepsComboBox, qgis::overload<int>::of( &QComboBox::currentIndexChanged ), this, &QgsTemporalControllerWidget::updateFrameDuration );
  connect( mSlider, &QSlider::valueChanged, this, &QgsTemporalControllerWidget::timeSlider_valueChanged );

  mStepSpinBox->setClearValue( 1 );

  connect( mNavigationObject, &QgsTemporalNavigationObject::updateTemporalRange, this, &QgsTemporalControllerWidget::updateSlider );

  connect( mSettings, &QPushButton::clicked, this, &QgsTemporalControllerWidget::settings_clicked );

  mRangeMenu.reset( new QMenu( this ) );

  mRangeSetToAllLayersAction = new QAction( tr( "Set to layers' cummulative date time range" ), mRangeMenu.get() );
  connect( mRangeSetToAllLayersAction, &QAction::triggered, this, &QgsTemporalControllerWidget::mRangeSetToAllLayersAction_triggered );
  mRangeMenu->addAction( mRangeSetToAllLayersAction );
  mRangeSetToProjectAction = new QAction( tr( "Set to project's date time range" ), mRangeMenu.get() );
  connect( mRangeSetToProjectAction, &QAction::triggered, this, &QgsTemporalControllerWidget::mRangeSetToProjectAction_triggered );
  mRangeMenu->addAction( mRangeSetToProjectAction );

  mRangeMenu->addSeparator();

  mRangeLayersSubMenu.reset( new QMenu( tr( "Set to single layer's date time range" ), mRangeMenu.get() ) );
  mRangeLayersSubMenu->setEnabled( false );
  mRangeMenu->addMenu( mRangeLayersSubMenu.get() );
  connect( mRangeMenu.get(), &QMenu::aboutToShow, this, &QgsTemporalControllerWidget::aboutToShowRangeMenu );

  mMapLayerModel = new QgsMapLayerModel( this );

  mSetToProjectTimeButton->setPopupMode( QToolButton::InstantPopup );
  mSetToProjectTimeButton->setMenu( mRangeMenu.get() );
  mFixedRangeSetToProjectTimeButton->setPopupMode( QToolButton::InstantPopup );
  mFixedRangeSetToProjectTimeButton->setMenu( mRangeMenu.get() );

  connect( mExportAnimationButton, &QPushButton::clicked, this, &QgsTemporalControllerWidget::exportAnimation );

  QgsDateTimeRange range;

  if ( QgsProject::instance()->timeSettings() )
    range = QgsProject::instance()->timeSettings()->temporalRange();

  mStartDateTime->setDisplayFormat( "yyyy-MM-dd HH:mm:ss" );
  mEndDateTime->setDisplayFormat( "yyyy-MM-dd HH:mm:ss" );
  mFixedRangeStartDateTime->setDisplayFormat( "yyyy-MM-dd HH:mm:ss" );
  mFixedRangeEndDateTime->setDisplayFormat( "yyyy-MM-dd HH:mm:ss" );

  if ( range.begin().isValid() && range.end().isValid() )
  {
    whileBlocking( mStartDateTime )->setDateTime( range.begin() );
    whileBlocking( mEndDateTime )->setDateTime( range.end() );
    whileBlocking( mFixedRangeStartDateTime )->setDateTime( range.begin() );
    whileBlocking( mFixedRangeEndDateTime )->setDateTime( range.end() );
  }

  QString setToProjectTimeButtonToolTip = tr( "Match time range to project. \n"
                                          "If a project has no explicit time range set, \n"
                                          "then the range will be calculated based on the \n"
                                          "minimum and maximum dates from any temporal-enabled layers." );

  mSetToProjectTimeButton->setToolTip( setToProjectTimeButtonToolTip );
  mFixedRangeSetToProjectTimeButton->setToolTip( setToProjectTimeButtonToolTip );

  for ( QgsUnitTypes::TemporalUnit u :
        {
          QgsUnitTypes::TemporalMilliseconds,
          QgsUnitTypes::TemporalSeconds,
          QgsUnitTypes::TemporalMinutes,
          QgsUnitTypes::TemporalHours,
          QgsUnitTypes::TemporalDays,
          QgsUnitTypes::TemporalWeeks,
          QgsUnitTypes::TemporalMonths,
          QgsUnitTypes::TemporalYears,
          QgsUnitTypes::TemporalDecades,
          QgsUnitTypes::TemporalCenturies
        } )
  {
    mTimeStepsComboBox->addItem( QgsUnitTypes::toString( u ), u );
  }

  // TODO: might want to choose an appropriate default unit based on the range
  mTimeStepsComboBox->setCurrentIndex( mTimeStepsComboBox->findData( QgsUnitTypes::TemporalHours ) );

  mStepSpinBox->setMinimum( 0.0000001 );
  mStepSpinBox->setMaximum( std::numeric_limits<int>::max() );
  mStepSpinBox->setSingleStep( 1 );
  mStepSpinBox->setValue( 1 );

  mForwardButton->setToolTip( tr( "Play" ) );
  mBackButton->setToolTip( tr( "Reverse" ) );
  mNextButton->setToolTip( tr( "Go to next frame" ) );
  mPreviousButton->setToolTip( tr( "Go to previous frame" ) );
  mStopButton->setToolTip( tr( "Pause" ) );
  mRewindButton->setToolTip( tr( "Rewind to start" ) );
  mFastForwardButton->setToolTip( tr( "Fast forward to end" ) );

  updateFrameDuration();

  connect( QgsProject::instance(), &QgsProject::readProject, this, &QgsTemporalControllerWidget::setWidgetStateFromProject );
  connect( QgsProject::instance(), &QgsProject::layersAdded, this, &QgsTemporalControllerWidget::onLayersAdded );
  connect( QgsProject::instance(), &QgsProject::cleared, this, &QgsTemporalControllerWidget::onProjectCleared );
}

void QgsTemporalControllerWidget::keyPressEvent( QKeyEvent *e )
{
  if ( mSlider->hasFocus() && e->key() == Qt::Key_Space )
  {
    togglePause();
  }
  QWidget::keyPressEvent( e );
}

void QgsTemporalControllerWidget::aboutToShowRangeMenu()
{
  mRangeLayersSubMenu->clear();

  for ( int i = 0; i < mMapLayerModel->rowCount(); ++i )
  {
    QModelIndex index = mMapLayerModel->index( i, 0 );
    QgsMapLayer *currentLayer = mMapLayerModel->data( index, QgsMapLayerModel::LayerRole ).value<QgsMapLayer *>();
    if ( !currentLayer->temporalProperties() || !currentLayer->temporalProperties()->isActive() )
      continue;

    QIcon icon = qvariant_cast<QIcon>( mMapLayerModel->data( index, Qt::DecorationRole ) );
    QString text = mMapLayerModel->data( index, Qt::DisplayRole ).toString();
    QgsDateTimeRange range = currentLayer->temporalProperties()->calculateTemporalExtent( currentLayer );

    QAction *action = new QAction( icon, text, mRangeLayersSubMenu.get() );
    connect( action, &QAction::triggered, this, [ = ]
    {
      setDates( range );
      saveRange();
    } );
    mRangeLayersSubMenu->addAction( action );
  }

  mRangeLayersSubMenu->setEnabled( !mRangeLayersSubMenu->actions().isEmpty() );
}

void QgsTemporalControllerWidget::togglePlayForward()
{
  mPlayingForward = true;

  if ( mNavigationObject->animationState() != QgsTemporalNavigationObject::Forward )
  {
    mStopButton->setChecked( false );
    mBackButton->setChecked( false );
    mForwardButton->setChecked( true );
    mNavigationObject->playForward();
  }
  else
  {
    mBackButton->setChecked( true );
    mForwardButton->setChecked( false );
    mNavigationObject->pause();
  }
}

void QgsTemporalControllerWidget::togglePlayBackward()
{
  mPlayingForward = false;

  if ( mNavigationObject->animationState() != QgsTemporalNavigationObject::Reverse )
  {
    mStopButton->setChecked( false );
    mBackButton->setChecked( true );
    mForwardButton->setChecked( false );
    mNavigationObject->playBackward();
  }
  else
  {
    mBackButton->setChecked( true );
    mBackButton->setChecked( false );
    mNavigationObject->pause();
  }
}

void QgsTemporalControllerWidget::togglePause()
{
  if ( mNavigationObject->animationState() != QgsTemporalNavigationObject::Idle )
  {
    mStopButton->setChecked( true );
    mBackButton->setChecked( false );
    mForwardButton->setChecked( false );
    mNavigationObject->pause();
  }
  else
  {
    mBackButton->setChecked( mPlayingForward ? false : true );
    mForwardButton->setChecked( mPlayingForward ? false : true );
    if ( mPlayingForward )
    {
      mNavigationObject->playForward();
    }
    else
    {
      mNavigationObject->playBackward();
    }
  }
}

void QgsTemporalControllerWidget::updateTemporalExtent()
{
  QgsDateTimeRange temporalExtent = QgsDateTimeRange( mStartDateTime->dateTime(),
                                    mEndDateTime->dateTime() );
  mNavigationObject->setTemporalExtents( temporalExtent );
  mSlider->setRange( 0, mNavigationObject->totalFrameCount() - 1 );
  mSlider->setValue( 0 );
}

void QgsTemporalControllerWidget::updateFrameDuration()
{
  if ( mBlockSettingUpdates )
    return;

  // save new settings into project
  QgsProject::instance()->timeSettings()->setTimeStepUnit( static_cast< QgsUnitTypes::TemporalUnit>( mTimeStepsComboBox->currentData().toInt() ) );
  QgsProject::instance()->timeSettings()->setTimeStep( mStepSpinBox->value() );

  mNavigationObject->setFrameDuration( QgsInterval( QgsProject::instance()->timeSettings()->timeStep(),
                                       QgsProject::instance()->timeSettings()->timeStepUnit() ) );
  mSlider->setRange( 0, mNavigationObject->totalFrameCount() - 1 );
}

void QgsTemporalControllerWidget::setWidgetStateFromProject()
{
  mBlockSettingUpdates++;
  mTimeStepsComboBox->setCurrentIndex( mTimeStepsComboBox->findData( QgsProject::instance()->timeSettings()->timeStepUnit() ) );
  mStepSpinBox->setValue( QgsProject::instance()->timeSettings()->timeStep() );
  mBlockSettingUpdates--;

  bool ok = false;
  QgsTemporalNavigationObject::NavigationMode mode = static_cast< QgsTemporalNavigationObject::NavigationMode>( QgsProject::instance()->readNumEntry( QStringLiteral( "TemporalControllerWidget" ),
      QStringLiteral( "/NavigationMode" ), 0, &ok ) );
  if ( ok )
  {
    mNavigationObject->setNavigationMode( mode );
    setWidgetStateFromNavigationMode( mode );
  }
  else
  {
    mNavigationObject->setNavigationMode( QgsTemporalNavigationObject::NavigationOff );
    setWidgetStateFromNavigationMode( QgsTemporalNavigationObject::NavigationOff );
  }

  const QString startString = QgsProject::instance()->readEntry( QStringLiteral( "TemporalControllerWidget" ), QStringLiteral( "/StartDateTime" ) );
  const QString endString = QgsProject::instance()->readEntry( QStringLiteral( "TemporalControllerWidget" ), QStringLiteral( "/EndDateTime" ) );
  if ( !startString.isEmpty() && !endString.isEmpty() )
  {
    whileBlocking( mStartDateTime )->setDateTime( QDateTime::fromString( startString, Qt::ISODate ) );
    whileBlocking( mEndDateTime )->setDateTime( QDateTime::fromString( endString, Qt::ISODate ) );
    whileBlocking( mFixedRangeStartDateTime )->setDateTime( QDateTime::fromString( startString, Qt::ISODate ) );
    whileBlocking( mFixedRangeEndDateTime )->setDateTime( QDateTime::fromString( endString, Qt::ISODate ) );
  }
  else
  {
    setDatesToProjectTime();
  }
  updateTemporalExtent();
  updateFrameDuration();

  mNavigationObject->setFramesPerSecond( QgsProject::instance()->timeSettings()->framesPerSecond() );
  mNavigationObject->setTemporalRangeCumulative( QgsProject::instance()->timeSettings()->isTemporalRangeCumulative() );
}

void QgsTemporalControllerWidget::mNavigationOff_clicked()
{
  QgsProject::instance()->writeEntry( QStringLiteral( "TemporalControllerWidget" ), QStringLiteral( "/NavigationMode" ),
                                      static_cast<int>( QgsTemporalNavigationObject::NavigationOff ) );

  mNavigationObject->setNavigationMode( QgsTemporalNavigationObject::NavigationOff );
  setWidgetStateFromNavigationMode( QgsTemporalNavigationObject::NavigationOff );
}

void QgsTemporalControllerWidget::mNavigationFixedRange_clicked()
{
  QgsProject::instance()->writeEntry( QStringLiteral( "TemporalControllerWidget" ), QStringLiteral( "/NavigationMode" ),
                                      static_cast<int>( QgsTemporalNavigationObject::FixedRange ) );

  mNavigationObject->setNavigationMode( QgsTemporalNavigationObject::FixedRange );
  setWidgetStateFromNavigationMode( QgsTemporalNavigationObject::FixedRange );
}

void QgsTemporalControllerWidget::mNavigationAnimated_clicked()
{
  QgsProject::instance()->writeEntry( QStringLiteral( "TemporalControllerWidget" ), QStringLiteral( "/NavigationMode" ),
                                      static_cast<int>( QgsTemporalNavigationObject::Animated ) );

  mNavigationObject->setNavigationMode( QgsTemporalNavigationObject::Animated );
  setWidgetStateFromNavigationMode( QgsTemporalNavigationObject::Animated );
}

void QgsTemporalControllerWidget::setWidgetStateFromNavigationMode( const QgsTemporalNavigationObject::NavigationMode mode )
{
  mNavigationOff->setChecked( mode == QgsTemporalNavigationObject::NavigationOff );
  mNavigationFixedRange->setChecked( mode  == QgsTemporalNavigationObject::FixedRange );
  mNavigationAnimated->setChecked( mode  == QgsTemporalNavigationObject::Animated );

  switch ( mode )
  {
    case QgsTemporalNavigationObject::NavigationOff:
      mNavigationModeStackedWidget->setCurrentIndex( 0 );
      break;
    case QgsTemporalNavigationObject::FixedRange:
      mNavigationModeStackedWidget->setCurrentIndex( 1 );
      break;
    case QgsTemporalNavigationObject::Animated:
      mNavigationModeStackedWidget->setCurrentIndex( 2 );
      break;
  }
}

void QgsTemporalControllerWidget::onLayersAdded( const QList<QgsMapLayer *> &layers )
{
  if ( !mHasTemporalLayersLoaded )
  {
    for ( QgsMapLayer *layer : layers )
    {
      if ( layer->temporalProperties() )
      {
        mHasTemporalLayersLoaded |= layer->temporalProperties()->isActive();

        if ( !mHasTemporalLayersLoaded )
        {
          connect( layer, &QgsMapLayer::dataSourceChanged, this, [ this, layer ]
          {
            if ( layer->isValid() && layer->temporalProperties()->isActive() && !mHasTemporalLayersLoaded )
            {
              mHasTemporalLayersLoaded = true;
              // if we are moving from zero temporal layers to non-zero temporal layers, let's set temporal extent
              this->setDatesToProjectTime();
            }
          } );
        }
      }
    }

    if ( mHasTemporalLayersLoaded )
      setDatesToProjectTime();
  }
}

void QgsTemporalControllerWidget::onProjectCleared()
{
  mHasTemporalLayersLoaded = false;

  mNavigationObject->setNavigationMode( QgsTemporalNavigationObject::NavigationOff );
  setWidgetStateFromNavigationMode( QgsTemporalNavigationObject::NavigationOff );

  whileBlocking( mStartDateTime )->setDateTime( QDateTime( QDate::currentDate(), QTime( 0, 0, 0, Qt::UTC ) ) );
  whileBlocking( mEndDateTime )->setDateTime( mStartDateTime->dateTime() );
  whileBlocking( mFixedRangeStartDateTime )->setDateTime( QDateTime( QDate::currentDate(), QTime( 0, 0, 0, Qt::UTC ) ) );
  whileBlocking( mFixedRangeEndDateTime )->setDateTime( mStartDateTime->dateTime() );
  updateTemporalExtent();
}

void QgsTemporalControllerWidget::updateSlider( const QgsDateTimeRange &range )
{
  whileBlocking( mSlider )->setValue( mNavigationObject->currentFrameNumber() );
  updateRangeLabel( range );
}

void QgsTemporalControllerWidget::updateRangeLabel( const QgsDateTimeRange &range )
{
  switch ( mNavigationObject->navigationMode() )
  {
    case QgsTemporalNavigationObject::Animated:
      mCurrentRangeLabel->setText( tr( "Frame: %1 to %2" ).arg(
                                     range.begin().toString( "yyyy-MM-dd HH:mm:ss" ),
                                     range.end().toString( "yyyy-MM-dd HH:mm:ss" ) ) );
      break;
    case QgsTemporalNavigationObject::FixedRange:
      mCurrentRangeLabel->setText( tr( "Range: %1 to %2" ).arg(
                                     range.begin().toString( "yyyy-MM-dd HH:mm:ss" ),
                                     range.end().toString( "yyyy-MM-dd HH:mm:ss" ) ) );
      break;
    case QgsTemporalNavigationObject::NavigationOff:
      mCurrentRangeLabel->setText( tr( "Temporal navigation disabled" ) );
      break;
  }
}

QgsTemporalController *QgsTemporalControllerWidget::temporalController()
{
  return mNavigationObject;
}

void QgsTemporalControllerWidget::settings_clicked()
{
  QgsTemporalMapSettingsWidget *settingsWidget = new QgsTemporalMapSettingsWidget( this );
  settingsWidget->setFrameRateValue( mNavigationObject->framesPerSecond() );
  settingsWidget->setIsTemporalRangeCumulative( mNavigationObject->temporalRangeCumulative() );

  connect( settingsWidget, &QgsTemporalMapSettingsWidget::frameRateChanged, this, [ = ]( double rate )
  {
    // save new settings into project
    QgsProject::instance()->timeSettings()->setFramesPerSecond( rate );
    mNavigationObject->setFramesPerSecond( rate );
  } );

  connect( settingsWidget, &QgsTemporalMapSettingsWidget::temporalRangeCumulativeChanged, this, [ = ]( bool state )
  {
    // save new settings into project
    QgsProject::instance()->timeSettings()->setIsTemporalRangeCumulative( state );
    mNavigationObject->setTemporalRangeCumulative( state );
  } );
  openPanel( settingsWidget );
}

void QgsTemporalControllerWidget::timeSlider_valueChanged( int value )
{
  mNavigationObject->setCurrentFrameNumber( value );
}

void QgsTemporalControllerWidget::startEndDateTime_changed()
{
  whileBlocking( mFixedRangeStartDateTime )->setDateTime( mStartDateTime->dateTime() );
  whileBlocking( mFixedRangeEndDateTime )->setDateTime( mEndDateTime->dateTime() );

  updateTemporalExtent();
  saveRange();
}

void QgsTemporalControllerWidget::fixedRangeStartEndDateTime_changed()
{
  whileBlocking( mStartDateTime )->setDateTime( mFixedRangeStartDateTime->dateTime() );
  whileBlocking( mEndDateTime )->setDateTime( mFixedRangeEndDateTime->dateTime() );

  updateTemporalExtent();
  saveRange();
}

void QgsTemporalControllerWidget::mRangeSetToAllLayersAction_triggered()
{
  setDatesToAllLayers();
  saveRange();
}

void QgsTemporalControllerWidget::mRangeSetToProjectAction_triggered()
{
  setDatesToProjectTime();
  saveRange();
}

void QgsTemporalControllerWidget::setDates( const QgsDateTimeRange &range )
{
  if ( range.begin().isValid() && range.end().isValid() )
  {
    whileBlocking( mStartDateTime )->setDateTime( range.begin() );
    whileBlocking( mEndDateTime )->setDateTime( range.end() );
    whileBlocking( mFixedRangeStartDateTime )->setDateTime( range.begin() );
    whileBlocking( mFixedRangeEndDateTime )->setDateTime( range.end() );
    updateTemporalExtent();
  }
}

void QgsTemporalControllerWidget::setDatesToAllLayers()
{
  QgsDateTimeRange range;

  range = QgsTemporalUtils::calculateTemporalRangeForProject( QgsProject::instance() );

  setDates( range );
}

void QgsTemporalControllerWidget::setDatesToProjectTime()
{
  QgsDateTimeRange range;

  // by default try taking the project's fixed temporal extent
  if ( QgsProject::instance()->timeSettings() )
    range = QgsProject::instance()->timeSettings()->temporalRange();

  // if that's not set, calculate the extent from the project's layers
  if ( !range.begin().isValid() || !range.end().isValid() )
  {
    range = QgsTemporalUtils::calculateTemporalRangeForProject( QgsProject::instance() );
  }

  setDates( range );
}

void QgsTemporalControllerWidget::setDateInputsEnable( bool enabled )
{
  mStartDateTime->setEnabled( enabled );
  mEndDateTime->setEnabled( enabled );
}

void QgsTemporalControllerWidget::updateButtonsEnable( bool enabled )
{
  mPreviousButton->setEnabled( enabled );
  mNextButton->setEnabled( enabled );
  mBackButton->setEnabled( enabled );
  mForwardButton->setEnabled( enabled );
}

void QgsTemporalControllerWidget::saveRange()
{
  QgsProject::instance()->writeEntry( QStringLiteral( "TemporalControllerWidget" ),
                                      QStringLiteral( "/StartDateTime" ), mStartDateTime->dateTime().toTimeSpec( Qt::OffsetFromUTC ).toString( Qt::ISODate ) );
  QgsProject::instance()->writeEntry( QStringLiteral( "TemporalControllerWidget" ),
                                      QStringLiteral( "/EndDateTime" ), mEndDateTime->dateTime().toTimeSpec( Qt::OffsetFromUTC ).toString( Qt::ISODate ) );
}
