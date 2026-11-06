/********************************************************************************************************\

	File Name:	OracleRecorder.cpp
	Descript:	Writes the oracle trace.  Record layout (one record per line, fields separated by a space;
				lines starting with '#' are metadata and are not hashed):

				S <sample> <ship> grid <x> <y> raw <gx> <gy> <i0..i4> <f0..f4> spd <speed> dmg <percent>
				  tgt <ship> dist <distance> st <status>
					One per ship in play per sample.  <ship> is "c<customID>" when the game ship ID maps
					to a script ship, otherwise "id<shipID>".  "raw" is the whole tShipSystemsInfo the game
					returns for kGetShipPosition, sentinel-filled beforehand, so any field the game fills
					beyond the 1-based grid cell the SDK exposes is captured too.

				E <sample> <event> <ship> i <i0..i4> f <f0..f4>
					Every event the game delivers to the script, in arrival order.  <event> is the
					iNotifyEvents bit in hex.  For kNotifyDamage, i0 (the hostile ship) is mapped to a
					ship name too.  Shield and hull values arrive this way, as kNotifyShipSystemUpdate
					(0x01000000) replies to the requests made in each sample.

				END <sample> <reason>
					Last line.  The driver waits for it.

				Floats are printed with %.9g, which round-trips a 32-bit float exactly.

\********************************************************************************************************/

#include <windows.h>
#include <string.h>
#include "OracleConfig.h"
#include "OracleRecorder.h"
#include "OracleSeed.h"

namespace
{
	const int32 kSentinelInt = 0x7EEEEEEE;
	const float kSentinelFloat = -98765.4375f;

	//	The systems whose current value is requested every sample: the group hard points, which every
	//	ship has.  The game answers kGetHealthOfSystem by indexing the ship's table of system groups with
	//	the hard point and calling the group found there, so a single system's hard point (a shield,
	//	the bridge, armor) reads a null entry and ends the game; only the groups are answered.
	const eSystemHardPoint kSampledSystems[] =
	{
		kShieldGroupHardPoint,
		kHullGroupHardPoint,
		kEngineGroupHardPoint,
	};
}

tOracleRecorder::tOracleRecorder( void ) :
	fTrace( NULL ),
	fSample( 0 ),
	fQuietSamples( 0 ),
	fFinished( kFalse ),
	fTimerArmed( kFalse )
{}

/*
	tEventMgr owns registered actors and deletes them when the mission is torn down, possibly after the
	game side is gone, so this only closes the file and does not touch the timer.
*/
tOracleRecorder::~tOracleRecorder( void )
{
	fTimerArmed = kFalse;
	mFinish( "aborted" );
}

/*
	Purpose:	Opens the trace next to the game executable, writes the header and starts the timer.
				Called once, from the script's mStartMission().
*/
void
tOracleRecorder::mStart( const std::string& SeedReport )
{
	char path[MAX_PATH];
	DWORD length = GetModuleFileNameA( NULL, path, MAX_PATH );
	while( length > 0 && path[length - 1] != '\\' && path[length - 1] != '/' )
		length--;
	path[length] = '\0';
	strcat( path, ORACLE_TRACE_FILE );

	fTrace = fopen( path, "w" );
	if( fTrace == NULL )
		return;

	fprintf( fTrace, "# oracle-trace v%d\n", kOracleTraceVersion );
	fprintf( fTrace, "# sample_turns %.9g max_samples %d quiet_samples %d\n",
			 kOracleSampleTurns, kOracleMaxSamples, kOracleQuietSamples );
	fprintf( fTrace, "# ships team1 %s c%d team2 %s c%d autopilot %d\n",
			 ORACLE_SHIP_TEAM1, kOracleShipTeam1, ORACLE_SHIP_TEAM2, kOracleShipTeam2, ORACLE_PLAYER_AUTOPILOT );
	fprintf( fTrace, "# rng pin %d %s\n", ORACLE_PIN_RNG, SeedReport.c_str() );

	for( int32 team = kTeam1; team <= kTeam2; team++ )
	{
		tTeamInfo* teamInfo = fMissionInfo->mGetTeamHandle( static_cast< eTeamID >( team ) );
		if( teamInfo == NULL )
			continue;
		fprintf( fTrace, "# team %d race %d human %d\n",
				 team, teamInfo->mGetRace(), teamInfo->mIsTeamHumanControlled() );
		for( tShipIterator ship = teamInfo->mGetFirstShip(); *ship != NULL; ++ship )
		{
			fprintf( fTrace, "# shipmap id%d c%d team %d class %s bpv %d\n",
					 ship->mGetShipID(), ship->mGetCustomID(), team, ship->mGetClassName(), ship->mGetBPV() );
		}
	}
	fflush( fTrace );

	mArmTimer();
}

/*
	Purpose:	(Re)arms the sample timer.  Whether the engine repeats a turn timer is not documented, so
				the old one is killed and a new one set on every tick; this is correct either way.
*/
void
tOracleRecorder::mArmTimer( void )
{
	if( fTimerArmed )
		gEventMgr.mKillTimer( fTimer );
	fTimer = gEventMgr.mSetTimedInterval( kTimerTurnsSet, kOracleTimerID, kOracleSampleTurns, fActorID );
	fTimerArmed = kTrue;
}

void
tOracleRecorder::mHandleShipEvent( tShipInfo* Ship, const tEventMgr::tEventInfo& EventInfo )
{
	if( fTrace == NULL || fFinished )
		return;

	if( EventInfo.fEvent == kNotifyAtTimedInterval )
	{
		if( EventInfo.fEventItem[2] == kOracleTimerID )
		{
			mSample();
		}
		return;
	}

	int32 items[tEventMgr::tEventInfo::kEventItemCount];
	for( int32 i = 0; i < tEventMgr::tEventInfo::kEventItemCount; i++ )
		items[i] = EventInfo.fEventItem[i];

	char hostile[32] = "-";
	if( EventInfo.fEvent == kNotifyDamage )
	{
		int32 customID = mCustomIDOf( items[0] );
		if( customID >= 0 )
		{
			sprintf( hostile, "c%d", customID );
			items[0] = 0;
		}
	}

	char ship[32] = "-";
	if( Ship )
		sprintf( ship, "c%d", Ship->mGetCustomID() );

	fprintf( fTrace, "E %d %08X %s i %d %d %d %d %d f %.9g %.9g %.9g %.9g %.9g h %s\n",
			 fSample, EventInfo.fEvent, ship,
			 items[0], items[1], items[2], items[3], items[4],
			 EventInfo.fEventItem_Float[0], EventInfo.fEventItem_Float[1], EventInfo.fEventItem_Float[2],
			 EventInfo.fEventItem_Float[3], EventInfo.fEventItem_Float[4],
			 hostile );
}

void
tOracleRecorder::mSample( void )
{
	if( fSample == 0 && ORACLE_PIN_RNG )
	{
		//	Pin again at the first sample, in case the engine reseeded between mStartMission and now.
		fprintf( fTrace, "# rng repin %s\n", nOracleSeed::mPin( kOracleCrtSeed, kOracleLcgSeed ).c_str() );
	}

	tShipInfo* first = mFirstShip( kTeam1 );
	tShipInfo* second = mFirstShip( kTeam2 );
	for( int32 team = kTeam1; team <= kTeam2; team++ )
	{
		tTeamInfo* teamInfo = fMissionInfo->mGetTeamHandle( static_cast< eTeamID >( team ) );
		if( teamInfo == NULL )
			continue;
		for( tShipIterator ship = teamInfo->mGetFirstShip(); *ship != NULL; ++ship )
		{
			mSampleShip( *ship, team == kTeam1 ? second : first );
		}
	}
	fflush( fTrace );

	fSample++;
	if( mShipsInPlay( kTeam1 ) == 0 || mShipsInPlay( kTeam2 ) == 0 )
		fQuietSamples++;

	if( fSample >= kOracleMaxSamples )
		mFinish( "max_samples" );
	else if( fQuietSamples >= kOracleQuietSamples )
		mFinish( "one_side_gone" );
	else
		mArmTimer();
}

void
tOracleRecorder::mSampleShip( tShipInfo* Ship, tShipInfo* Other )
{
	tShipSystemsInfo info;
	info.fAction = kGetShipPosition;
	info.fShipEntityID = Ship->mGetShipID();
	info.fGridX = kSentinelInt;
	info.fGridY = kSentinelInt;
	for( int32 i = 0; i < 5; i++ )
	{
		info.fInt32[i] = kSentinelInt;
		info.fFloat[i] = kSentinelFloat;
	}
	gScriptInterface.mEntityMgr( info );

	int32 gridX, gridY;
	Ship->mGetGridPosition( &gridX, &gridY );

	char target[32];
	int32 targetID = Ship->mGetShipTargetID();
	int32 targetCustomID = mCustomIDOf( targetID );
	if( targetCustomID >= 0 )
		sprintf( target, "c%d", targetCustomID );
	else
		sprintf( target, "id%d", targetID );

	float distance = ( Other && Other != Ship ) ? Ship->mGetTacticalDistance( Other ) : -1.0f;

	int32u status = 0;
	for( int32 bit = 0; bit <= 6; bit++ )
	{
		if( Ship->mTestShipStatus( 1u << bit ) )
			status |= 1u << bit;
	}

	fprintf( fTrace, "S %d c%d grid %d %d raw %d %d %d %d %d %d %d %.9g %.9g %.9g %.9g %.9g"
					 " spd %d dmg %.9g tgt %s dist %.9g st %X\n",
			 fSample, Ship->mGetCustomID(), gridX, gridY,
			 info.fGridX, info.fGridY,
			 info.fInt32[0], info.fInt32[1], info.fInt32[2], info.fInt32[3], info.fInt32[4],
			 info.fFloat[0], info.fFloat[1], info.fFloat[2], info.fFloat[3], info.fFloat[4],
			 Ship->mGetShipSpeed(), Ship->mGetDamagePercent(), target, distance, status );

	//
	//	Events are one-shot: tEventMgr::mEntityEvent clears the bit after each delivery.  Re-arm before
	//	every request so that each system reply gets through even if replies are synchronous.
	//
	Ship->mSetShipCallbackEvents( kWatchedEvents );
	for( int32 s = 0; s < int32( sizeof( kSampledSystems ) / sizeof( kSampledSystems[0] ) ); s++ )
		mRequestSystem( Ship, kSampledSystems[s] );
}

void
tOracleRecorder::mRequestSystem( tShipInfo* Ship, eSystemHardPoint System )
{
	Ship->mSetShipCallbackEvents( kNotifyShipSystemUpdate );
	Ship->mGetHealthOfSystem( System );
}

void
tOracleRecorder::mFinish( const char* Reason )
{
	if( fFinished )
		return;
	fFinished = kTrue;
	if( fTimerArmed )
	{
		gEventMgr.mKillTimer( fTimer );
		fTimerArmed = kFalse;
	}
	if( fTrace )
	{
		fprintf( fTrace, "END %d %s\n", fSample, Reason );
		fclose( fTrace );
		fTrace = NULL;
	}

	//
	//	The mission has no victory condition of its own, so nothing would end it: once END is on disk,
	//	the human team's game is over, as a script's victory state ends it (the SDK's templates call
	//	gScriptInterface.mSetGameStatus), and the game shows its end screen.  Not on teardown.
	//
	if( strcmp( Reason, "aborted" ) != 0 )
	{
		gScriptInterface.mSetGameStatus( kTeam1, kPlayerWins );
		fMissionInfo->mCheckForMissionEnd();
	}
}

int32
tOracleRecorder::mCustomIDOf( int32 ShipID )
{
	tShipInfo* ship = fMissionInfo->mFindShipID( ShipID );
	return ship ? ship->mGetCustomID() : -1;
}

int32
tOracleRecorder::mShipsInPlay( eTeamID Team )
{
	tTeamInfo* team = fMissionInfo->mGetTeamHandle( Team );
	return team ? team->mShipsRemainingInPlay() : 0;
}

tShipInfo*
tOracleRecorder::mFirstShip( eTeamID Team )
{
	tTeamInfo* team = fMissionInfo->mGetTeamHandle( Team );
	if( team == NULL )
		return NULL;
	tShipIterator ship = team->mGetFirstShip();
	return *ship;
}
