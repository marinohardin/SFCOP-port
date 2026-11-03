/********************************************************************************************************\

	File Name:	OracleRecorder.h
	Descript:	An actor that samples every ship on a game-turn timer and logs every event the game sends
				to the script.  It has no states: it overrides mHandleShipEvent directly so it sees the raw
				event stream before any state filtering.

\********************************************************************************************************/

#if !defined( OracleRecorder_h )
#define OracleRecorder_h

#include <stdio.h>
#include <string>
#include "ScriptInterface.h"

class tOracleRecorder : public tActor
{
public:

	tOracleRecorder( void );
	~tOracleRecorder( void );

	void			mStart( const std::string& SeedReport );
	virtual void	mHandleShipEvent( tShipInfo* Ship, const tEventMgr::tEventInfo& EventInfo );

protected:

	enum
	{
		kWatchedEvents = kNotifyAtDeath | kNotifyOnCapture | kNotifyDisengaged | kNotifyDamage |
						 kNotifyAlertStatusChange | kNotifyProbeLaunch | kNotifyBeginTractor |
						 kNotifyEndTractor | kNotifyShipSystemUpdate,
	};

	void	mArmTimer( void );
	void	mSample( void );
	void	mSampleShip( tShipInfo* Ship, tShipInfo* Other );
	void	mRequestSystem( tShipInfo* Ship, eSystemHardPoint System );
	void	mFinish( const char* Reason );
	int32	mCustomIDOf( int32 ShipID );
	int32	mShipsInPlay( eTeamID Team );
	tShipInfo* mFirstShip( eTeamID Team );

	FILE*				fTrace;
	int32				fSample;
	int32				fQuietSamples;
	iTruth				fFinished;
	iTruth				fTimerArmed;
	tEventMgr::tTimerID	fTimer;
};

#endif
